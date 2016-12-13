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
 */

#include "httplib_main.h"
#include "httplib_string.h"
#include "httplib_utils.h"

void XX_httplib_send_http_error( struct mg_connection *conn, int status, const char *fmt, ... ) {

	char buf[MG_BUF_LEN];
	va_list ap;
	int len;
	int i;
	int page_handler_found;
	int scope;
	int truncated;
	int has_body;
	char date[64];
	time_t curtime = time(NULL);
	const char *error_handler = NULL;
	struct file error_page_file = STRUCT_FILE_INITIALIZER;
	const char *error_page_file_ext, *tstr;

	const char *status_text = mg_get_response_code_text(conn, status);

	if (conn == NULL) return;

	conn->status_code = status;
	if (conn->in_error_handler || conn->ctx->callbacks.http_error == NULL
	    || conn->ctx->callbacks.http_error(conn, status)) {
		if (!conn->in_error_handler) {
			/* Send user defined error pages, if defined */
			error_handler = conn->ctx->config[ERROR_PAGES];
			error_page_file_ext = conn->ctx->config[INDEX_FILES];
			page_handler_found = 0;
			if (error_handler != NULL) {
				for (scope = 1; (scope <= 3) && !page_handler_found; scope++) {
					switch (scope) {
					case 1: /* Handler for specific error, e.g. 404 error */
						XX_httplib_snprintf(conn, &truncated, buf, sizeof(buf) - 32, "%serror%03u.", error_handler, status);
						break;
					case 2: /* Handler for error group, e.g., 5xx error handler
					         * for all server errors (500-599) */
						XX_httplib_snprintf(conn, &truncated, buf, sizeof(buf) - 32, "%serror%01uxx.", error_handler, status / 100);
						break;
					default: /* Handler for all errors */
						XX_httplib_snprintf(conn, &truncated, buf, sizeof(buf) - 32, "%serror.", error_handler);
						break;
					}

					/* String truncation in buf may only occur if error_handler
					 * is too long. This string is from the config, not from a
					 * client. */
					(void)truncated;

					len = (int)strlen(buf);

					tstr = strchr(error_page_file_ext, '.');

					while (tstr) {
						for (i = 1; i < 32 && tstr[i] != 0 && tstr[i] != ',';
						     i++)
							buf[len + i - 1] = tstr[i];
						buf[len + i - 1] = 0;
						if (XX_httplib_stat(conn, buf, &error_page_file)) {
							page_handler_found = 1;
							break;
						}
						tstr = strchr(tstr + i, '.');
					}
				}
			}

			if (page_handler_found) {
				conn->in_error_handler = 1;
				XX_httplib_handle_file_based_request(conn, buf, &error_page_file);
				conn->in_error_handler = 0;
				return;
			}
		}

		/* No custom error page. Send default error page. */
		XX_httplib_gmt_time_string(date, sizeof(date), &curtime);

		/* Errors 1xx, 204 and 304 MUST NOT send a body */
		has_body = (status > 199 && status != 204 && status != 304);

		conn->must_close = 1;
		mg_printf(conn, "HTTP/1.1 %d %s\r\n", status, status_text);
		XX_httplib_send_no_cache_header(conn);
		if (has_body) mg_printf(conn, "%s", "Content-Type: text/plain; charset=utf-8\r\n");
		mg_printf(conn, "Date: %s\r\n" "Connection: close\r\n\r\n", date);

		/* Errors 1xx, 204 and 304 MUST NOT send a body */
		if (has_body) {
			mg_printf(conn, "Error %d: %s\n", status, status_text);

			if (fmt != NULL) {
				va_start(ap, fmt);
				XX_httplib_vsnprintf(conn, NULL, buf, sizeof(buf), fmt, ap);
				va_end(ap);
				mg_write(conn, buf, strlen(buf));
			}

		} else {
			/* No body allowed. Close the connection. */
		}
	}

}  /* XX_httplib_send_http_error */