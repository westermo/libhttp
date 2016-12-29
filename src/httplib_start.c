/* 
 * Copyright (c) 2016 Lammert Bies
 * Copyright (c) 2013-2016 the Civetweb developers
 * Copyright (c) 2004-2013 Sergey Lyubka
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * ============
 * Release: 2.0
 */

#include "httplib_main.h"
#include "httplib_pthread.h"
#include "httplib_ssl.h"
#include "httplib_string.h"
#include "httplib_utils.h"

static bool			check_bool( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, bool *config  );
static bool			check_dbg(  struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, enum debug_level_t *config );
static bool			check_dir(  struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
static bool			check_file( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
static bool			check_int(  struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, int *config, int minval, int maxval );
static bool			check_patt( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
static bool			check_str(  struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
static struct httplib_context *	cleanup( struct httplib_context *ctx, PRINTF_FORMAT_STRING(const char *fmt), ...) PRINTF_ARGS(2, 3);
static bool			process_options( struct httplib_context *ctx, const struct httplib_option_t *options );

/*
 * struct httplib_context *httplib_start( const struct httplib_callbacks *callbacks, void *user_data, const struct httplib_t *options );
 *
 * The function httplib_start() functions as the main entry point for the LibHTTP
 * server. The function starts all threads and when finished returns the
 * context to the running server for future reference.
 */

struct httplib_context *httplib_start( const struct httplib_callbacks *callbacks, void *user_data, const struct httplib_option_t *options ) {

	struct httplib_context *ctx;
	int i;
	void (*exit_callback)(const struct httplib_context *ctx);
	struct httplib_workerTLS tls;

#if defined(_WIN32)

	/*
	 * Yes, this is Windows and nothing works out of the box. We first have
	 * to initialize socket communications by telling Windows which socket
	 * version we want to use. 2.2 in this case.
	 */

	WSADATA data;
	WSAStartup( MAKEWORD(2, 2), &data );

#endif /* _WIN32 */

	/*
	 * No memory for the ctx structure is the only error which we
	 * don't log through httplib_cry() for the simple reason that we do not
	 * have enough configured yet to make that function working. Having an
	 * OOM in this state of the process though should be noticed by the
	 * calling process in other parts of their execution anyway.
	 */

	exit_callback = NULL;
	ctx           = httplib_calloc( 1, sizeof(*ctx) );

	if ( ctx == NULL ) return NULL;

	/*
	 * Setup callback functions very early. This is necessary to make the
	 * log_message() callback function available in case an error occurs.
	 *
	 * We first set the exit_context() callback to NULL becasue no proper
	 * context is available yet and we do not want to mess up things if the
	 * function exits and that callback is given a half-decent structure to
	 * work on and without a call to init_context() before.
	 */

	if ( callbacks != NULL ) {

		ctx->callbacks              = *callbacks;
		exit_callback               = callbacks->exit_context;
		ctx->callbacks.exit_context = NULL;
	}

	/*
	 * Random number generator will initialize at the first call
	 */

	ctx->auth_nonce_mask = httplib_get_random() ^ (uint64_t)(ptrdiff_t)(options);

	if ( httplib_atomic_inc( & XX_httplib_sTlsInit ) == 1 ) {

#if defined(_WIN32)
		InitializeCriticalSection( & global_log_file_lock );
#else  /* _WIN32 */
		pthread_mutexattr_init(    & XX_httplib_pthread_mutex_attr                          );
		pthread_mutexattr_settype( & XX_httplib_pthread_mutex_attr, PTHREAD_MUTEX_RECURSIVE );
#endif  /* _WIN32 */

		if ( httplib_pthread_key_create( & XX_httplib_sTlsKey, XX_httplib_tls_dtor ) != 0 ) {

			/*
			 * Fatal error - abort start. However, this situation should
			 * never occur in practice.
			 */

			httplib_atomic_dec( & XX_httplib_sTlsInit );
			httplib_cry( DEBUG_LEVEL_CRASH, ctx, NULL, "Cannot initialize thread local storage" );
			ctx = httplib_free( ctx );

			return NULL;
		}
	}
	
	else {
		/*
		 * TODO (low): istead of sleeping, check if XX_httplib_sTlsKey is already
		 * initialized.
		 */

		httplib_sleep( 1 );
	}

	tls.is_master  = -1;
	tls.thread_idx = (unsigned)httplib_atomic_inc( & XX_httplib_thread_idx_max );
#if defined(_WIN32)
	tls.pthread_cond_helper_mutex = NULL;
#endif
	httplib_pthread_setspecific( XX_httplib_sTlsKey, & tls );

	if ( httplib_pthread_mutex_init( & ctx->thread_mutex, &XX_httplib_pthread_mutex_attr )  ) return cleanup( ctx, "Cannot initialize thread mutex"          );
#if !defined(ALTERNATIVE_QUEUE)
	if ( httplib_pthread_cond_init(  & ctx->sq_empty, NULL )                                ) return cleanup( ctx, "Cannot initialize empty queue condition" );
	if ( httplib_pthread_cond_init(  & ctx->sq_full,  NULL )                                ) return cleanup( ctx, "Cannot initialize full queue condition"  );
#endif
	if ( httplib_pthread_mutex_init( & ctx->nonce_mutex,  & XX_httplib_pthread_mutex_attr ) ) return cleanup( ctx, "Cannot initialize nonce mutex"           );

	ctx->user_data = user_data;
	ctx->handlers  = NULL;

	if ( process_options( ctx, options ) ) return NULL;

	XX_httplib_get_system_name( & ctx->systemName );

	/*
	 * NOTE(lsm): order is important here. SSL certificates must
	 * be initialized before listening ports. UID must be set last.
	 */

	if ( ! XX_httplib_set_gpass_option( ctx ) ) return cleanup( ctx, "Error setting gpass option" );
#if !defined(NO_SSL)
	if ( ! XX_httplib_set_ssl_option(   ctx ) ) return cleanup( ctx, "Error setting SSL option"   );
#endif
	if ( ! XX_httplib_set_ports_option( ctx ) ) return cleanup( ctx, "Error setting ports option" );
	if ( ! XX_httplib_set_uid_option(   ctx ) ) return cleanup( ctx, "Error setting UID option"   );
	if ( ! XX_httplib_set_acl_option(   ctx ) ) return cleanup( ctx, "Error setting ACL option"   );

#if !defined(_WIN32)

	/*
	 * Ignore SIGPIPE signal, so if browser cancels the request, it
	 * won't kill the whole process.
	 */

	signal( SIGPIPE, SIG_IGN );

#endif /* !_WIN32 */

	if ( ctx->num_threads < 1 ) return cleanup( ctx, "No worker thread number specified" );

	if ( ctx->num_threads > MAX_WORKER_THREADS ) return cleanup( ctx, "Too many worker threads" );

	if ( ctx->num_threads > 0 ) {

		ctx->workerthreadids = httplib_calloc( ctx->num_threads, sizeof(pthread_t) );
		if ( ctx->workerthreadids == NULL ) return cleanup( ctx, "Not enough memory for worker thread ID array" );

#if defined(ALTERNATIVE_QUEUE)

		ctx->client_wait_events = httplib_calloc( sizeof(ctx->client_wait_events[0]), ctx->num_threads );
		if ( ctx->client_wait_events == NULL ) return cleanup( ctx, "Not enough memory for worker event array" );

		ctx->client_socks = httplib_calloc( sizeof(ctx->client_socks[0]), ctx->num_threads );
		if ( ctx->client_socks == NULL ) return cleanup( ctx, "Not enough memory for worker socket array" );

		for (i=0; i<ctx->num_threads; i++) {

			ctx->client_wait_events[i] = event_create();
			if ( ctx->client_wait_events[i] == 0 ) return cleanup( ctx, "Error creating worker event %u", i );
		}
#endif
	}

#if defined(USE_TIMERS)
	if ( timers_init( ctx ) != 0 ) return cleanup( ctx, "Error creating timers" );
#endif

	/*
	 * Context has been created - init user libraries
	 *
	 * Context has been properly setup. It is now safe to use exit_context
	 * in case the system needs a shutdown.
	 */

	if ( ctx->callbacks.init_context != NULL ) ctx->callbacks.init_context( ctx );

	ctx->callbacks.exit_context = exit_callback;
	ctx->ctx_type               = CTX_TYPE_SERVER;

	/*
	 * Start master (listening) thread
	 */

	XX_httplib_start_thread_with_id( XX_httplib_master_thread, ctx, &ctx->masterthreadid );

	/*
	 * Start worker threads
	 */
	for (i=0; i<ctx->num_threads; i++) {

		struct worker_thread_args *wta;
	       
		wta = httplib_malloc( sizeof(struct worker_thread_args) );

		if ( wta != NULL ) {

			wta->ctx   = ctx;
			wta->index = (int)i;
		}

		if ( wta == NULL  ||  XX_httplib_start_thread_with_id( XX_httplib_worker_thread, wta, &ctx->workerthreadids[i] ) != 0 ) {

			/*
			 * thread was not created
			 */

			wta = httplib_free( wta );

			if ( i > 0 ) httplib_cry( DEBUG_LEVEL_ERROR, ctx, NULL, "Cannot start worker thread %i: error %ld", i + 1, (long)ERRNO );
			
			else return cleanup( ctx, "Cannot create worker threads: error %ld", (long)ERRNO );

			break;
		}
	}

	httplib_pthread_setspecific( XX_httplib_sTlsKey, NULL );

	return ctx;

}  /* httplib_start */



/*
 * static bool process_options( struct httplib_context *ctx, const struct httplib_option_t *options );
 *
 * The function process_options() processes the user supplied options and adds
 * them to the central option list of the context. If en error occurs, the
 * function returns true, otherwise FALSE is returned. In case of an error all
 * cleanup is already done before returning and an error message has been
 * generated.
 */

static bool process_options( struct httplib_context *ctx, const struct httplib_option_t *options ) {

	if ( ctx == NULL ) return false;

	ctx->access_control_allow_origin = NULL;
	ctx->access_control_list         = NULL;
	ctx->access_log_file             = NULL;
	ctx->allow_sendfile_call         = true;
	ctx->authentication_domain       = NULL;
	ctx->cgi_environment             = NULL;
	ctx->cgi_interpreter             = NULL;
	ctx->cgi_pattern                 = NULL;
	ctx->debug_level                 = DEBUG_LEVEL_WARNING;
	ctx->decode_url                  = true;
	ctx->document_root               = NULL;
	ctx->enable_directory_listing    = true;
	ctx->enable_keep_alive           = false;
	ctx->error_log_file              = NULL;
	ctx->error_pages                 = NULL;
	ctx->extra_mime_types            = NULL;
	ctx->global_auth_file            = NULL;
	ctx->hide_file_pattern           = NULL;
	ctx->index_files                 = NULL;
	ctx->listening_ports             = NULL;
	ctx->num_threads                 = 50;
	ctx->protect_uri                 = NULL;
	ctx->put_delete_auth_file        = NULL;
	ctx->request_timeout             = 30000;
	ctx->run_as_user                 = NULL;
	ctx->ssi_pattern                 = NULL;
	ctx->ssl_ca_file                 = NULL;
	ctx->ssl_ca_path                 = NULL;
	ctx->ssl_certificate             = NULL;
	ctx->ssl_cipher_list             = NULL;
	ctx->ssl_protocol_version        = 0;
	ctx->ssl_short_trust             = false;
	ctx->ssl_verify_depth            = 9;
	ctx->ssl_verify_paths            = true;
	ctx->ssl_verify_peer             = false;
	ctx->static_file_max_age         = 0;
	ctx->throttle                    = NULL;
	ctx->tcp_nodelay                 = false;
	ctx->url_rewrite_patterns        = NULL;
	ctx->websocket_root              = NULL;
	ctx->websocket_timeout           = 30000;

	if ( (ctx->access_control_allow_origin = httplib_strdup( "*" )) == NULL ) {

		cleanup( ctx, "Out of memory creating context allocating \"access_control_allow_origin\"" );
		return true;
	}

	if ( (ctx->authentication_domain = httplib_strdup( "example.com" )) == NULL ) {

		cleanup( ctx, "Out of memory creating context allocating \"authentication_domain\"" );
		return true;
	}

	if ( (ctx->cgi_pattern = httplib_strdup( "**.cgi$|**.pl$|**.php$" )) == NULL ) {

		cleanup( ctx, "Out of memory creating context allocating \"cgi_pattern\"" );
		return true;
	}

	if ( (ctx->index_files = httplib_strdup( "index.xhtml,index.html,index.htm,index.cgi,index.shtml,index.php" )) == NULL ) {

		cleanup( ctx, "Out of memory creating context allocating \"index_files\"" );
		return true;
	}

	if ( (ctx->listening_ports = httplib_strdup( "8080" )) == NULL ) {

		cleanup( ctx, "Out of memory creating context allocating \"listening_ports\"" );
		return true;
	}

	if ( (ctx->ssi_pattern = httplib_strdup( "**.shtml$|**.shtm$" )) == NULL ) {

		cleanup( ctx, "Out of memory creating context allocating \"ssi_pattern\"" );
		return true;
	}

	while ( options != NULL  &&  options->name != NULL ) {

		if ( check_str(  ctx, options, "access_control_allow_origin", & ctx->access_control_allow_origin             ) ) return true;
		if ( check_str(  ctx, options, "access_control_list",         & ctx->access_control_list                     ) ) return true;
		if ( check_file( ctx, options, "access_log_file",             & ctx->access_log_file                         ) ) return true;
		if ( check_bool( ctx, options, "allow_sendfile_call",         & ctx->allow_sendfile_call                     ) ) return true;
		if ( check_str(  ctx, options, "authentication_domain",       & ctx->authentication_domain                   ) ) return true;
		if ( check_str(  ctx, options, "cgi_environment",             & ctx->cgi_environment                         ) ) return true;
		if ( check_file( ctx, options, "cgi_interpreter",             & ctx->cgi_interpreter                         ) ) return true;
		if ( check_patt( ctx, options, "cgi_pattern",                 & ctx->cgi_pattern                             ) ) return true;
		if ( check_dbg(  ctx, options, "debug_level",                 & ctx->debug_level                             ) ) return true;
		if ( check_bool( ctx, options, "decode_url",                  & ctx->decode_url                              ) ) return true;
		if ( check_dir(  ctx, options, "document_root",               & ctx->document_root                           ) ) return true;
		if ( check_bool( ctx, options, "enable_directory_listing",    & ctx->enable_directory_listing                ) ) return true;
		if ( check_bool( ctx, options, "enable_keep_alive",           & ctx->enable_keep_alive                       ) ) return true;
		if ( check_file( ctx, options, "error_log_file",              & ctx->error_log_file                          ) ) return true;
		if ( check_dir(  ctx, options, "error_pages",                 & ctx->error_pages                             ) ) return true;
		if ( check_str(  ctx, options, "extra_mime_types",            & ctx->extra_mime_types                        ) ) return true;
		if ( check_file( ctx, options, "global_auth_file",            & ctx->global_auth_file                        ) ) return true;
		if ( check_patt( ctx, options, "hide_file_pattern",           & ctx->hide_file_pattern                       ) ) return true;
		if ( check_str(  ctx, options, "index_files",                 & ctx->index_files                             ) ) return true;
		if ( check_str(  ctx, options, "listening_ports",             & ctx->listening_ports                         ) ) return true;
		if ( check_int(  ctx, options, "num_threads",                 & ctx->num_threads,                 1, INT_MAX ) ) return true;
		if ( check_str(  ctx, options, "protect_uri",                 & ctx->protect_uri                             ) ) return true;
		if ( check_file( ctx, options, "put_delete_auth_file",        & ctx->put_delete_auth_file                    ) ) return true;
		if ( check_int(  ctx, options, "request_timeout",             & ctx->request_timeout,             0, INT_MAX ) ) return true;
		if ( check_str(  ctx, options, "run_as_user",                 & ctx->run_as_user                             ) ) return true;
		if ( check_patt( ctx, options, "ssi_pattern",                 & ctx->ssi_pattern                             ) ) return true;
		if ( check_file( ctx, options, "ssl_ca_file",                 & ctx->ssl_ca_file                             ) ) return true;
		if ( check_dir(  ctx, options, "ssl_ca_path",                 & ctx->ssl_ca_path                             ) ) return true;
		if ( check_file( ctx, options, "ssl_certificate",             & ctx->ssl_certificate                         ) ) return true;
		if ( check_str(  ctx, options, "ssl_cipher_list",             & ctx->ssl_cipher_list                         ) ) return true;
		if ( check_int(  ctx, options, "ssl_protocol_version",        & ctx->ssl_protocol_version,        0, 4       ) ) return true;
		if ( check_bool( ctx, options, "ssl_short_trust",             & ctx->ssl_short_trust                         ) ) return true;
		if ( check_int(  ctx, options, "ssl_verify_depth",            & ctx->ssl_verify_depth,            0, 9       ) ) return true;
		if ( check_bool( ctx, options, "ssl_verify_paths",            & ctx->ssl_verify_paths                        ) ) return true;
		if ( check_bool( ctx, options, "ssl_verify_peer",             & ctx->ssl_verify_peer                         ) ) return true;
		if ( check_int(  ctx, options, "static_file_max_age",         & ctx->static_file_max_age,         0, INT_MAX ) ) return true;
		if ( check_str(  ctx, options, "throttle",                    & ctx->throttle                                ) ) return true;
		if ( check_bool( ctx, options, "tcp_nodelay",                 & ctx->tcp_nodelay                             ) ) return true;
		if ( check_str(  ctx, options, "url_rewrite_patterns",        & ctx->url_rewrite_patterns                    ) ) return true;
		if ( check_dir(  ctx, options, "websocket_root",              & ctx->websocket_root                          ) ) return true;
		if ( check_int(  ctx, options, "websocket_timeout",           & ctx->websocket_timeout,           0, INT_MAX ) ) return true;

		/*
		 * TODO: Currently silently ignoring unrecognized options
		 */

		options++;
	}

	return false;

}  /* process_options */



/*
 * static bool check_bool( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, bool *config );
 *
 * The function check_bool() checks if an option is equal to a boolean config
 * parameter and stores the value if that is the case. If the value cannot be
 * recognized, true is returned and the function performs a complete cleanup.
 * If the option name could not be found, the function returns false to
 * indicate that the search should go on. If the value could be found, also
 * false is returned.
 */

static bool check_bool( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, bool *config ) {

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing boolean option" );
		return true;
	}

	if (      httplib_strcasecmp(           option->name,  name   ) ) return false;
	if ( ! XX_httplib_option_value_to_bool( option->value, config ) ) return false;
			
	cleanup( ctx, "Invalid boolean value \"%s\" for option \"%s\"", option->value, option->name );
	return true;

}  /* check_bool */



/*
 * static bool check_dir( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
 *
 * The function check_dir() checks if an option is equal to a directory config
 * parameter and stores the value if that is the case. If the value cannot be
 * recognized, true is returned and the function performs a complete cleanup.
 * If the option name could not be found, the function returns false to
 * indicate that the search should go on. IF the value could be found, also
 * false is returned.
 */

static bool check_dir( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config ) {

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing directory option" );
		return true;
	}

	if ( httplib_strcasecmp( option->name, name ) ) return false;

	*config = httplib_free( *config );

	if ( option->value == NULL ) return false;

	*config = httplib_strdup( option->value );
	if ( *config != NULL ) return false;

	cleanup( ctx, "Out of memory assigning value \"%s\" to option \"%s\"", option->value, option->name );
	return true;

}  /* check_dir */



/*
 * static bool check_patt( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
 *
 * The function check_patt() checks if an option is equal to a pattern config
 * parameter and stores the value if that is the case. If the value cannot be
 * recognized, true is returned and the function performs a complete cleanup.
 * If the option name could not be found, the function returns false to
 * indicate that the search should go on. IF the value could be found, also
 * false is returned.
 */

static bool check_patt( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config ) {

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing pattern option" );
		return true;
	}

	if ( httplib_strcasecmp( option->name, name ) ) return false;

	*config = httplib_free( *config );

	if ( option->value == NULL ) return false;

	*config = httplib_strdup( option->value );
	if ( *config != NULL ) return false;

	cleanup( ctx, "Out of memory assigning value \"%s\" to option \"%s\"", option->value, option->name );
	return true;

}  /* check_patt */



/*
 * static bool check_file( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
 *
 * The function check_file() checks if an option is equal to a filename config
 * parameter and stores the value if that is the case. If the value cannot be
 * recognized, true is returned and the function performs a complete cleanup.
 * If the option name could not be found, the function returns false to
 * indicate that the search should go on. IF the value could be found, also
 * false is returned.
 */

static bool check_file( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config ) {

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing file option" );
		return true;
	}

	if ( httplib_strcasecmp( option->name, name ) ) return false;

	*config = httplib_free( *config );

	if ( option->value == NULL ) return false;

	*config = httplib_strdup( option->value );
	if ( *config != NULL ) return false;

	cleanup( ctx, "Out of memory assigning value \"%s\" to option \"%s\"", option->value, option->name );
	return true;

}  /* check_file */



/*
 * static bool check_str( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config );
 *
 * The function check_str() checks if an option is equal to a string config
 * parameter and stores the value if that is the case. If the value cannot be
 * recognized, true is returned and the function performs a complete cleanup.
 * If the option name could not be found, the function returns false to
 * indicate that the search should go on. IF the value could be found, also
 * false is returned.
 */

static bool check_str( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, char **config ) {

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing string option" );
		return true;
	}

	if ( httplib_strcasecmp( option->name, name ) ) return false;

	*config = httplib_free( *config );

	if ( option->value == NULL ) return false;

	*config = httplib_strdup( option->value );
	if ( *config != NULL ) return false;

	cleanup( ctx, "Out of memory assigning value \"%s\" to option \"%s\"", option->value, option->name );
	return true;

}  /* check_str */



/*
 * static bool check_int( struct httplib_context *ctx, const struct httplib_opion_t *option, const char *name, int *config, int minval, int maxval );
 *
 * The function check_int() checks in an option is equal to an integer config
 * parameter and stores the value if that is the case. If the value cannot be
 * recognized, true is returned and the function performs a complete cleanup.
 * If the option name could not be found, the function returns false to
 * indicate that the search should go on. If the value could be found and is
 * valud, also false is returned.
 */

static bool check_int( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, int *config, int minval, int maxval ) {

	int val;

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing integer option" );
		return true;
	}

	if ( httplib_strcasecmp( option->name, name ) ) return false;

	if ( ! XX_httplib_option_value_to_int( option->value, & val ) ) {

		if ( val < minval ) { cleanup( ctx, "Integer \"%s\" too small for option \"%s\"", option->value, option->name ); return true; }
		if ( val > maxval ) { cleanup( ctx, "Integer \"%s\" too large for option \"%s\"", option->value, option->name ); return true; }

		*config = val;
		return false;
	}

	cleanup( ctx, "Invalid integer value \"%s\" for option \"%s\"", option->value, option->name );
	return true;

}  /* check_int */



/*
 * static bool check_dbg( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name );
 *
 * The function check_dbg() checks if an option is equal to a debug level
 * config parameter and stores the value if that is the case. If the value
 * cannot be recognized, true is returned and the function performs a complete
 * cleanup. If the option name could not be found, the function returns false
 * to indicate that the search should go on. If the value could be found and is
 * valid, also false is returned.
 */

static bool check_dbg( struct httplib_context *ctx, const struct httplib_option_t *option, const char *name, enum debug_level_t *config ) {

	int val;

	if ( ctx == NULL  ||  option == NULL  ||  option->name == NULL  ||  name == NULL  ||  config == NULL ) {

		cleanup( ctx, "Internal error parsing debug level option" );
		return true;
	}

	if ( httplib_strcasecmp( option->name, name ) ) return false;

	if ( ! XX_httplib_option_value_to_int( option->value, &val ) ) {

		switch ( val ) {

			case DEBUG_LEVEL_NONE    :
			case DEBUG_LEVEL_CRASH   :
			case DEBUG_LEVEL_ERROR   :
			case DEBUG_LEVEL_WARNING :
			case DEBUG_LEVEL_INFO    :
				*config = val;
				return false;
		}
	}

	cleanup( ctx, "Invalid value \"%s\"  for option \"%s\"", option->value, option->name );
	return true;

}  /* check_dbg */



/* 
 * static struct httplib_context *cleanup( struct httplib_context *ctx, const char *fmt, ... );
 *
 * The function cleanup() is called to do some cleanup work when an error
 * occured initializing a context. The function returns NULL which is then
 * further returned to the calling party.
 */

static struct httplib_context *cleanup( struct httplib_context *ctx, const char *fmt, ... ) {

	va_list ap;
	char buf[MG_BUF_LEN];

	if ( ctx == NULL ) return NULL;

	if ( fmt != NULL ) {

		va_start( ap, fmt );
		vsnprintf_impl( buf, sizeof(buf), fmt, ap );
		va_end( ap );
		buf[sizeof(buf)-1] = 0;

		httplib_cry( DEBUG_LEVEL_CRASH, ctx, NULL, "%s", buf );
	}

	XX_httplib_free_context( ctx );

	httplib_pthread_setspecific( XX_httplib_sTlsKey, NULL );

	return NULL;

}  /* cleanup */
