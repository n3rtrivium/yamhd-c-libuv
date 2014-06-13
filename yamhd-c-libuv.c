//
//     (c) 2014 Martin Wind and Rene Romanus
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// ### Yet another minimal HTTP daemon
//
// This is not a real server and never will be.
// See [Haywire](https://github.com/kellabyte/Haywire) for an asynchronous
// HTTP server framework on top of libuv.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <uv.h>

// ### Defines and function declaration

// If the request contains a `path` bigger than `MAX_PATH_SIZE` discard it.
#define MAX_PATH_SIZE 256
// Read files in chunks of size `READ_BUF_SIZE`.
#define READ_BUF_SIZE 4096
// Allocate `HEADER_BUF_SIZE` for generated response headers.
#define HEADERS_BUF_SIZE 1024

// All possible [parser](#parse) errors.
#define PARSER_ERR_STATE_NULL -1
#define PARSER_ERR_NOT_GET -2
#define PARSER_ERR_PATH_INVALID -3
#define PARSER_ERR_NOT_HTTP1 -4
#define PARSER_ERR_MALLOC -5

// Enable / Disable this line to toggle debug mode.
#if 1
#  define DEBUG_BUILD
#endif
#ifdef DEBUG_BUILD
#  define DEBUG(...) fprintf(stderr, __VA_ARGS__)
#else
#  define DEBUG(...) do {} while (0)
#endif

static uv_loop_t *loop;
static uv_tcp_t tcp_server;
static char* web_root;
static int web_root_len;

#ifdef DEBUG_BUILD
static size_t concurrent_states = 0;
#endif


// # Activity diagram
//
//                  +-+
//                  +-+
//                   |
//                   |
//          +--------v--------+
//          |                 |
//          |  on_connection  |     create state
//          |                 |
//          +--------+--------+
//                   |
//                   |
//                   |
//          +--------v--------+
//          |                 |     parse headers
//      +---+   on_tcp_read   |     open file
//      |   |                 |     close on error
//      |   +--------+--------+
//      |            |
//      |            |
//      |            |
//      |   +--------v--------+     close on error
//      |   |                 <--+  reopen on directory
//      +---+  on_file_open   |  |  send 404 on ENOENT
//      |   |                 +--+  send 200 header
//      |   +--------+--------+     read file
//      |            |
//      |            |
//      |            |
//      |   +--------v--------+
//      |   |                 |     close on error
//      +---+  on_tcp_write   <--+  or close flag
//      |   |                 |  |  read next file buffer
//      |   +--------+--------+  |
//      |            |           |
//      |            |           |
//      |            |           |
//      |   +--------v--------+  |
//      |   |                 |  |  close on error
//      |   |  on_file_read   +--+  close on EOF
//      |   |                 |     send file buffer
//      |   +--------+--------+
//      |            |
//      |            |
//      |            |
//      |   +--------v--------+
//      |   |                 |
//      +--->    on_close     |     close the stream
//          |                 |
//          +-----------------+


// # Event handler

// - **[on connection](#on-connection)**
// - **[on tcp read](#on-tcp-read)**
// - **[on tcp write](#on-tcp-write)**
// - **[on file open](#on-file-open)**
// - **[on file read](#on-file-read)**
// - **[on tcp write](#on-tcp-write)**
// - **[on close](#on-close)**
static void on_connection(uv_stream_t*, int status);
static void on_tcp_read(uv_stream_t*, ssize_t nread, const uv_buf_t* buf);
static void on_file_open(uv_fs_t* req);
static void on_tcp_write(uv_write_t* req, int status);
static void on_file_read(uv_fs_t * req);
static void on_close(uv_handle_t* peer);


// # State struct
//
// This struct is passed to all handlers to keep track of the state.

// - **stream**:  Socket handle.
// - **file**: File handle.
// - **method**: HTTP method (currently not used).
// - **path**: The mapped filesystem path.
// - **content_type**: The content type header.
//   4.2 for the max length.
// - **close_after_write (flag)**: Close the connection after the next write.
// - **read_on_write (flag)**: Read from file after the next write.
// - **dead (flag)**: Other request has returned an error. Expect an broken stream or
//   file handle.
// - **file_pos**: Position in the file.
// - **file_buf**: Buffer used to pipe data from a file to a tcp stream.
// - **headers_buf**: Buffer for generated headers.
// - **pending_requests**: Count of outstanding and dispatched requests.
typedef struct {
	uv_stream_t* stream;
	int file;
	char* method[8];
	uv_buf_t path;
	uv_buf_t content_type;
	int close_after_write;
	int read_on_write;
	int dead;
	size_t file_pos;
	uv_buf_t* file_buf;
	uv_buf_t* headers_buf;
	int pending_requests;
} req_res_t;

// # Event dispatcher

// - **[open path as file](#open-path-as-file)**: Open the `path`.
// - **[send headers buf](#send-headers-buf)**: Send the headers buffer.
// - **[send file buf](#send-file-buf)**: Send the file buffer.
// - **[read file](#read-file)**: Fill the file buffer.
// - **[req res free](#)**: Close the stream and file handle. Free the memory of the state.
static void open_path_as_file(req_res_t *rr);
static void send_headers_buf(req_res_t* rr);
static void send_file_buf(req_res_t* rr);
static void read_file(req_res_t *rr);
static void req_res_free(req_res_t* rr);

// ## Helpers

// - **[send not found](#send-not-found)**: Send a `404` response.
// - **[set headers](#set-headers-helper)**: Set the headers buffer.
// - **[parse](#simple-request-header-parser)**: Simple request header parser.
static void send_not_found(req_res_t* rr);
static void set_headers(req_res_t* rr, uv_buf_t code);
static int	parse(const uv_buf_t* req, req_res_t* rr);

// # Stop reading!
// Since this file starts with utility functions it is a good idea to read 
// the file bottom up. [Goto main](#main)

// ### Global fixed bufs
// This buffers should **never** be altered.
static uv_buf_t http_not_found_code;
static uv_buf_t http_not_found_body;
static uv_buf_t http_ok_code;
static uv_buf_t http_req_start;
static uv_buf_t http_req_after_path;
static uv_buf_t http_req_index;
static uv_buf_t http_res_start;
static uv_buf_t http_res_key_content_type;
static uv_buf_t http_res_content_type_html;
static uv_buf_t http_res_content_type_html_ext;
static uv_buf_t http_res_content_type_javascript;
static uv_buf_t http_res_content_type_javascript_ext;
static uv_buf_t http_res_content_type_css;
static uv_buf_t http_res_content_type_css_ext;
static uv_buf_t http_res_content_type_binary;
static uv_buf_t http_res_end;

// #### Initialize the global fixed buffers.
// This function is called early in the main function to initialize the
// the fixed buffers.
static void global_bufs_init() {
	http_not_found_code = uv_buf_init("404 Not Found", 13);
	http_not_found_body = uv_buf_init("<h1>Not Found</h1>\n\0", 20);
	http_ok_code = uv_buf_init("200 OK", 6);
	http_req_start = uv_buf_init("GET ", 4);
	http_req_after_path = uv_buf_init(" HTTP/1", 7);
	http_req_index = uv_buf_init("index.html\0", 11);
	http_res_start = uv_buf_init("HTTP/1.1 ", 8);
	http_res_key_content_type = uv_buf_init("Content-Type: ", 14);
	http_res_content_type_html = uv_buf_init("text/html", 9);
	http_res_content_type_html_ext = uv_buf_init(".html", 5);
	http_res_content_type_javascript = uv_buf_init("text/javascript", 15);
	http_res_content_type_javascript_ext = uv_buf_init(".js", 3);
	http_res_content_type_css = uv_buf_init("text/css", 8);
	http_res_content_type_css_ext = uv_buf_init(".css", 4);
	http_res_content_type_binary = uv_buf_init("application/octet-stream", 24);
	http_res_end = uv_buf_init("\n\n", 2);
}

// ## State

// ### req res init
// This function ensures the default values of the state struct. 
static req_res_t* req_res_init(uv_stream_t* handle) {
	req_res_t* rr = malloc(sizeof(req_res_t));
	if (rr != NULL) {
		rr->stream = handle;
		rr->file = 0;
		memset(rr->method, '\0', 8);
		rr->path.len = 0;
		rr->content_type = http_res_content_type_html;
		rr->close_after_write = 0;
		rr->read_on_write = 0;
		rr->dead = 0;
		rr->file_pos = 0;
		rr->file_buf = NULL;
		rr->headers_buf = NULL;
		rr->pending_requests = 0;
		
		DEBUG("concurrent_states (+1): %zu\n", ++concurrent_states);
		
	}
	else { /* Do nothing.*/ }
	return rr;
}

// #### req res init file buf
// Initialize the read buffer of the sate.
static void req_res_init_file_buf(req_res_t* rr) {
	if (rr->file_buf != NULL) {
		DEBUG("req_res_init_file_buf error: read not NULL\n");
	}
	else{
		rr->file_buf = malloc(sizeof(uv_buf_t));
		rr->file_buf->base = malloc(READ_BUF_SIZE);
		rr->file_buf->len = READ_BUF_SIZE;
	}
}

// #### req res init headers buf
// Initialize the headers buffer of the sate.
static void req_res_init_headers_buf(req_res_t* rr) {
	if (rr->headers_buf != NULL) {
		DEBUG("req_res_init_headers_buf error: header not NULL\n");
	}
	else{
		rr->headers_buf = malloc(sizeof(uv_buf_t));
		rr->headers_buf->base = malloc(HEADERS_BUF_SIZE);
		rr->headers_buf->len = HEADERS_BUF_SIZE;
	}
}

// #### req res free file
// Close the file descriptor.
static void req_res_free_file(req_res_t* rr) {
	if (rr != NULL){
		if (rr->file != 0) {
			uv_fs_t close_req;
			// `NULL` as callback function makes the call synchron.
			if (0 != uv_fs_close(loop, &close_req, rr->file, NULL)) {
				fprintf(stderr, "uv_fs_close error\n");
			}
			uv_fs_req_cleanup(&close_req);
		}
		else {
			/* Do nothing yet.
			 ToDo: DEBUG msg proposed for later server state.*/
		}
	}
	else {
		DEBUG("req_res_free_file error: rr is NULL\n");
	}
}

// ### req res free
// Free all memory of one request.
static void req_res_free(req_res_t* rr) {
	// Check for pending requests.
	if(rr != NULL) {
		if (rr->pending_requests > 0) {
			DEBUG("req_res_free pending requests: %d\n", rr->pending_requests);
			// Close the stream if it is not already closed.
			if (!uv_is_closing((uv_handle_t *) rr->stream)) {
				uv_close((uv_handle_t *) rr->stream, on_close);
				rr->stream = NULL;
			}
			rr->dead = 1;
			// Here, return is fine. Change would be too complex.
			return;
		} else if(rr->dead) {
			DEBUG("req_res_free free dead request\n");
		}
		else {
			DEBUG("req_res_free has no request, but is not dead.\n");
		}
		// Free the `path` string.
		if (rr->path.len > 0) {
			/* ToDo: Please review, whether the 'if' statement is necessary. */
			free(rr->path.base);
		}
		else {
			/* Do nothing.*/
		}
		// Free the read buffer.
		if (rr->file_buf != NULL) {
			free(rr->file_buf->base);
			free(rr->file_buf);
		}
		else {
			/* Do nothing.*/
		}
		// Free the headers buffer.
		if (rr->headers_buf != NULL) {
			free(rr->headers_buf->base);
			free(rr->headers_buf);
		}
		else {
			/* Do nothing.*/
		}
		// Close the file descriptor.
		req_res_free_file(rr);
		// Close the stream if it is not already closed.
		if (rr->stream != NULL && !uv_is_closing((uv_handle_t *) rr->stream)) {
			uv_close((uv_handle_t *) rr->stream, on_close);
		}
		else {
			/* Do nothing.*/
		}
		// Free the state struct.
		free(rr);
		DEBUG("concurrent_states (-1): %zu\n", --concurrent_states);
		
	}
	else {
		DEBUG("req_res_free error: rr is NULL\n");
	}
}

// -----------
// # Event dispatcher

// ## send headers buf
// Create a tcp-stream-write-request, associate it with the state, and dispatch the
// request. [on_tcp_write](#on-tcp-write) gets called as soon as the buffer is
// written to the stream.
static void send_headers_buf(req_res_t* rr) {
	uv_write_t* wr = (uv_write_t*) malloc(sizeof(uv_write_t));
	if(wr != NULL) {
		DEBUG("send_headers_buf\n");
		wr->data = rr;
		rr->pending_requests++;
		if ( 0 != uv_write(wr, rr->stream, rr->headers_buf, 1, on_tcp_write) ) {
			fprintf(stderr, "uv_write send_headers_buf failed\n");
			rr->pending_requests--;
			free(wr);
			req_res_free(rr);
		}
		else {
			/* Write is OK, Do nothing. */
		}
	}
	else {
		DEBUG("send_headers_buf error: memory leak, no handle for wr.\n");
	}
}

// ## send file buf
// Create a tcp-stream-write-request, associate it with the state, and dispatch the
// request. [on_tcp_write](#on-tcp-write) gets called as soon as the buffer is
// written to the stream.
static void send_file_buf(req_res_t* rr) {
	uv_write_t* wr = (uv_write_t*) malloc(sizeof(uv_write_t));
	if(wr != NULL) {
		DEBUG("send_file_buf\n");

		wr->data = rr;
		rr->pending_requests++;
		if (0 != uv_write(wr, rr->stream, rr->file_buf, 1, on_tcp_write)) {
			fprintf(stderr, "uv_write send_file_buf failed\n");
			rr->pending_requests--;
			free(wr);
			/* ToDo: To be moved to finalize? */
			req_res_free(rr);
		}
		else {
			/* Write is OK, Do nothing. */
		}
	}
	else {
		DEBUG("send_file_buf error: memory leak, no handle for wr.\n");
	}
}

// ## open path as file
// Create a file-open-request, associate the state and dispatch it on the loop. The
// [on_file_open](#on-file-open) is invoked once the file is opened.
static void open_path_as_file(req_res_t *rr) {
	int r;
	uv_fs_t* file_open_req = malloc(sizeof(uv_fs_t));
	if (file_open_req != NULL) {
		DEBUG("read_file\n");

		file_open_req->data = rr;
		rr->pending_requests++;
		r = uv_fs_open(loop, file_open_req, rr->path.base, O_ASYNC | O_RDONLY, S_IFREG, on_file_open);
		if (r != 0) {
			fprintf(stderr, "open_path_as_file error: %d\n", r);
			rr->pending_requests--;
			free(file_open_req);
			/* ToDo: To be moved to finalize? */
			req_res_free(rr);
		}
		else {
			/* Open is OK, Do nothing. */
		}
	}
	else {
		DEBUG("open_path_as_file error: memory leak, no handle for file_open_req.\n");
	}
}

// ## read file
// Create a file-read-request and dispatch it.
// [on_file_read](#on-file-read) is invoked once the buffer is read from the file.
static void read_file(req_res_t *rr) {
	int r;
	uv_fs_t* fs_req = malloc(sizeof(uv_fs_t));
	if (fs_req != NULL) {
		DEBUG("read_file\n");
		
		fs_req->data = rr;
		rr->pending_requests++;
		r = uv_fs_read(loop, fs_req, rr->file, rr->file_buf, 1, rr->file_pos, on_file_read);
		if	(r != 0) {
			fprintf(stderr, "read error: %d\n", r);
			rr->pending_requests--;
			free(fs_req);
			/* ToDo: To be moved to finalize? */
			req_res_free(rr);
		}
		else {
			/* Read is OK, Do nothing. */
		}
	}
	else {
		DEBUG("read_file error: memory leak, no handle for fs_req.\n");
	}
}

// -----------
// # Helpers

// ## send not found
static void send_not_found(req_res_t* rr) {
	if(rr != NULL)
	{
		DEBUG("send_not_found\n");
		// `rr->close_after_write = 1` makes `on_tcp_write` close the connection.
		rr->close_after_write = 1;
		rr->content_type = http_res_content_type_html;
		set_headers(rr, http_not_found_code);
		// Check if `headers_buf` is initialized.
		if (rr->headers_buf == NULL) return;
		// Check if the buffer is big enough to add the `Not found` body.
		if (rr->headers_buf->len+http_not_found_body.len >= HEADERS_BUF_SIZE) {
			fprintf(stderr, "headers buffer too small: -3");
		}
		else {
			// Add the body and set the new buffer size.
			memcpy(rr->headers_buf->base+rr->headers_buf->len, http_not_found_body.base, http_not_found_body.len);
			rr->headers_buf->len += http_not_found_body.len;
			send_headers_buf(rr);
		}
	}
	else {
		DEBUG("send_not_found error: rr is NULL.\n");
	}
}

// ## set headers helper
static void set_headers(req_res_t* rr, uv_buf_t code) {
	if(rr != NULL) {
		DEBUG("set_headers\n");
		// Initialize the headers buffer.
		req_res_init_headers_buf(rr);
	
		if (code.len+http_res_start.len >= HEADERS_BUF_SIZE) {
			fprintf(stderr, "headers buffer too small: -1");
		}
		else {
			memcpy(rr->headers_buf->base, http_res_start.base, http_res_start.len);
			int pos = http_res_start.len;
			memcpy(rr->headers_buf->base+pos, code.base, code.len);
			pos += code.len;

			// Get the UTC `struct tm`.
			time_t now = time(NULL);
			struct tm* utc = gmtime(&now);
			if (utc == NULL) {
				fprintf(stderr, "gmtime error");
			}
			else {
				// Expand the headers.
				if (strftime(rr->headers_buf->base+pos, HEADERS_BUF_SIZE-pos, "\nDate: %a, %d %b %Y %H:%M:%S %Z\n", utc) == 0) {
					fprintf(stderr, "strftime error");
				}
				else {
					/* ToDo: reconsider */
					pos = strlen(rr->headers_buf->base);
					if (pos+http_res_key_content_type.len+rr->content_type.len+http_res_end.len >= HEADERS_BUF_SIZE) {
						fprintf(stderr, "headers buffer too small: -2");
					}
					else {
						memcpy(rr->headers_buf->base+pos, http_res_key_content_type.base, http_res_key_content_type.len);
						pos += http_res_key_content_type.len;
						memcpy(rr->headers_buf->base+pos, rr->content_type.base, rr->content_type.len);
						pos += rr->content_type.len;
						memcpy(rr->headers_buf->base+pos, http_res_end.base, http_res_end.len);
						pos += http_res_end.len;
						rr->headers_buf->len = pos;
					}
				}
			}
		}
	}
	else {
		DEBUG("set_headers error: rr is NULL.\n");
	}
}

// ## Simple request header parser

// Reads the buffer `req` and sets the `path` and `content_type` strings of
// the state.
static int parse(const uv_buf_t* req, req_res_t* rr) {
	int return_value = PARSER_ERR_STATE_NULL;
	if ((rr != NULL) && (req != NULL)) {
		
		DEBUG("parse\n");

		// Check if it is an `GET` request.
		if (strncmp(req->base, http_req_start.base, http_req_start.len) != 0) {
			fprintf(stderr, "Not a GET request\n");
			return_value = PARSER_ERR_NOT_GET;
		}
		else {
			// Search for the end of the `path`.
			int path_start = 4;
			int path_len = 0;
			/* ToDo: Potential endless loop? */
			while (path_len+path_start < req->len) {
				if (req->base[path_len+path_start] == ' ') {
					break;
				}
				path_len++;
			}
			// Validate the `path` length.
			if (path_len == 0 || path_len > MAX_PATH_SIZE) {
				fprintf(stderr, "Path invalid\n");
				return_value = PARSER_ERR_PATH_INVALID;
			}
			else {
				// Check if it is a `HTTP` version 1.* request.
				if (path_start+path_len + http_req_after_path.len > req->len ||
					strncmp(req->base+path_start+path_len, http_req_after_path.base, http_req_after_path.len) != 0)
				{
					fprintf(stderr, "Not a HTTP/1 request\n");
					return_value = PARSER_ERR_NOT_HTTP1;
				}
				else {
					// Allocate memory for the `path`.
					char * path_buf = malloc(web_root_len+path_len);
					if (path_buf == NULL) {
						fprintf(stderr, "malloc error\n");
						return_value = PARSER_ERR_MALLOC;
					}
					else {
						// Copy the path and terminate it with `\0`.
						strcpy(path_buf, web_root);
						memcpy(path_buf+web_root_len, req->base+path_start, path_len+1);
						path_buf[path_len+web_root_len] = '\0';
						rr->path = uv_buf_init(path_buf, web_root_len+path_len);
						
						// Set the content type based on the file extension.
						if (rr->path.len > http_res_content_type_html_ext.len &&
							strncmp(rr->path.base + rr->path.len - http_res_content_type_html_ext.len,
								http_res_content_type_html_ext.base,
								http_res_content_type_html_ext.len) == 0)
						{
							rr->content_type = http_res_content_type_html;
						} else if (rr->path.len > http_res_content_type_javascript_ext.len &&
							strncmp(rr->path.base + rr->path.len - http_res_content_type_javascript_ext.len,
								http_res_content_type_javascript_ext.base,
								http_res_content_type_javascript_ext.len) == 0)
						{
							rr->content_type = http_res_content_type_javascript;
						} else if (rr->path.len > http_res_content_type_css_ext.len &&
							strncmp(rr->path.base + rr->path.len - http_res_content_type_css_ext.len,
								http_res_content_type_css_ext.base,
								http_res_content_type_css_ext.len) == 0)
						{
							rr->content_type = http_res_content_type_css;
						} else {
							rr->content_type = http_res_content_type_binary;
						}
						return_value = 0;
					}
				}
			}
		}
	}
	else {
		DEBUG("parse error: rr or req is NULL.\n");
	}
	return return_value;
}

// -----------
// # Event handlers
// #### request alloc

// Libuv does not allocate memory. Thus this function is provided to `uv_read_start` to
// allocate memory for the request buffer.
static void request_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	/* ToDo: Param 'handle' incidently not used.*/
	if(buf != NULL) {
		buf->base = malloc(suggested_size);
		if(buf->base != NULL) {
			buf->len = suggested_size;
		}
		else {
			DEBUG("request_alloc error: memory leak, no handle for buf->base with length %zu.\n", suggested_size);
			/* ToDo: Proposal: Set buf_len to INVALID_LENGTH.*/
		}
	}
	else {
		DEBUG("request_alloc error: buf is NULL.\n");
	}
}

// #### on close
//
// Free the stream.
static void on_close(uv_handle_t* peer) {
	/* ToDo: Please check with Valgrind. Is there any heap usage nested?*/
	free(peer);
}

// ## on tcp write
// Either close the connection or read
static void on_tcp_write(uv_write_t* req, int status) {
	if(req != NULL) {
		req_res_t* rr = (req_res_t*) req->data;
		if(rr != NULL) {
			rr->pending_requests--;
			
			// Check if an error occurred in an other handler.
			if (rr->dead) {
				req_res_free(rr);
			}
			else {
				DEBUG("on_tcp_write\n");
				
				// Free the write request.
				free(req);
				
				// Check for write error.
				if (status != 0) {
					fprintf(stderr, "on_tcp_write error: %s - %s\n", uv_err_name(status), uv_strerror(status));
					req_res_free(rr);
				}
				else {
					// Either close the connection or read more data of the file.
					if (rr->close_after_write) {
						DEBUG("on_tcp_write is closing the connection\n");
						req_res_free(rr);
					} else if (rr->read_on_write != 0) {
						rr->read_on_write = 0;
						// `read_file` triggers [on_file_open](#on-file-open)
						read_file(rr);
					}
					else {
						/* Do nothing.*/
					}
				}
			}
		}
		else {
			DEBUG("on_tcp_write error: req->data is NULL.\n");
		}
	}
	else {
		DEBUG("on_tcp_write error: req is NULL.\n");
	}
}

// ## on file read
static void on_file_read(uv_fs_t * req) {
	if(req != NULL) {
		int status = req->result;
		req_res_t* rr = (req_res_t*) req->data;
		if(rr != NULL) {
			rr->pending_requests--;
			
			// Check if an error occurred in an other handler.
			if (rr->dead) {
				req_res_free(rr);
			}
			else {
				DEBUG("on_file_read %s\n", rr->path.base);
				
				// Free the read request.
				uv_fs_req_cleanup(req);
				free(req);
				
				// Check for read error.
				if((status < 0 ) && (status != UV_EOF)) {
					fprintf(stderr, "on_file_read error: %s\n", uv_strerror(status));
					req_res_free(rr);
				}
				else
				// Check if the whole file is already read.
				if (status == UV_EOF || status == 0) {
					DEBUG("on_file_read reached end of file - closing connection\n");
					req_res_free(rr);
				}
				else {
					DEBUG("on_file_read %d bytes\n", status);
				
					rr->read_on_write = 1; // Read until an error happens.
					rr->file_pos += status; // Update the file postion.
					rr->file_buf->len = status; // This will sometimes *shrink* the buffer.
					// `send_file_buf` triggers [on_tcp_write](#on-tcp-write)
					send_file_buf(rr);
				}
			}
		}
		else {
			DEBUG("on_file_read error: req->result is NULL.\n");
		}
	}
	else {
		DEBUG("on_file_read error: req is NULL.\n");
	}
}


// ## on file open
static void on_file_open(uv_fs_t* req) {
	if(req != NULL) {
		int status = req->result;
		req_res_t* rr = (req_res_t*) req->data;
		if(rr != NULL) {
			rr->pending_requests--;
			
			// Check if an error occurred in an other handler.
			if (rr->dead) {
				req_res_free(rr);
			}
			else {
				char* path_buf;
				int r;
				DEBUG("on_file_open %s\n", rr->path.base);
				
				uv_fs_req_cleanup(req);
				free(req);
				
				// Check if `uv_fs_open` returned an error.
				if (status < 0) {
					// Send an `404` response only on `File not found` and `Illegal on directory` error.
					// Note: It is important to close the file on `EMFILE` and other errors.
					if (status == UV_ENOENT || status == UV_EISDIR) {
						send_not_found(rr);
					}
					else {
						fprintf(stderr, "async open error '%s': %s\n", rr->path.base, uv_strerror(status));
						// Close the file and free the memory.
						req_res_free(rr);
					}
				}
				else {
					// TODO: Recheck if it is possible to fail in `on_file_open` and get `status < 0`.
					uv_fs_t stat_req;
					
					r = uv_fs_stat(uv_default_loop(), &stat_req, rr->path.base, NULL);
					if (r != 0) {
						fprintf(stderr, "uv_fs_stat error '%s': %s\n", rr->path.base, uv_strerror(r));
						req_res_free(rr);
					}
					else {
						uv_stat_t* s = &stat_req.statbuf;
						
						if(s != NULL) {
							if (!S_ISREG(s->st_mode)) {
								// If the file is a directory append `index.html` to the path and reopen it.
								if (S_ISDIR(s->st_mode)) {
									if (rr->path.len + http_req_index.len >= MAX_PATH_SIZE) {
										fprintf(stderr, "path to large: -4\n");
										req_res_free(rr);
									}
									else {
										path_buf = malloc(rr->path.len + http_req_index.len);
										if(path_buf != NULL) {
											
											memcpy(path_buf, rr->path.base, rr->path.len);
											memcpy(path_buf+rr->path.len, http_req_index.base, http_req_index.len);
											free(rr->path.base);
											rr->path = uv_buf_init(path_buf, rr->path.len + http_req_index.len);
											// Set the content type.
											rr->content_type = http_res_content_type_html;
											// Close the directory file handle.
											rr->file = status;
											req_res_free_file(rr);
											// Open the new path.
											open_path_as_file(rr);
										}
										else {
											DEBUG("on_file_open error: memory leak, no handle for path_buf with length %zu.\n", rr->path.len + http_req_index.len);
										}
									}
								} else {
									// Not a file or directory, send a `404`.
									send_not_found(rr);
								}
							} else {
								// Store the file handle.
								rr->file = status;
								// Send the `200` response headers.
								rr->read_on_write = 1;
								req_res_init_file_buf(rr);
								// Send the `200` response headers.
								set_headers(rr, http_ok_code);
								send_headers_buf(rr);
							}
						}
						else {
							DEBUG("on_file_open error: &stat_req.statbuf is NULL.\n");
						}
					}
				}
			}
		}
		else {
			DEBUG("on_file_open error: req->result is NULL.\n");
		}
	}
	else {
		DEBUG("on_file_open error: req is NULL.\n");
	}
}

// ## on tcp read
static void on_tcp_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
	if(handle != NULL) {
		int r;
		req_res_t *rr = (req_res_t*) handle->data;
		
		DEBUG("on_tcp_read\n");
		
		if(rr != NULL) {
			// Check if an error occurred in an other handler.
			if (rr->dead) {
				req_res_free(rr);
			}
			else {
				// Check if `uv_read` returned an error.
				if (nread < 0) {
					// Print every error except end of file (UV_EOF).
					if (nread != UV_EOF) {
						fprintf(stderr, "on_tcp_read error: %s\n", uv_err_name(nread));
					}
					else {
						/* Do nothing.*/
					}
					if(buf != NULL) {
						// Free the memory allocated by `request_alloc`.
						if (buf->base)
							free(buf->base);
						
						// Close the stream.
						req_res_free(rr);
					}
					else {
						DEBUG("on_tcp_read error: buf is NULL\n");
					}
				}
				else {
					if (nread == 0) {
						if(buf != NULL) {
							// Everything OK, but nothing read.
							free(buf->base);
						}
						else {
							DEBUG("on_tcp_read error: buf is NULL\n");
						}
					}
					else {
						// Check if an previous `on_tcp_read` has parsed the headers.
						if (rr->path.len > 0) {
							DEBUG("on_tcp_read on already parsed headers\n");
							free(buf->base);
						}
						else {
							// Parse the request headers.
							r = parse(buf, rr);
							if (r != 0) {
								req_res_free(rr);
							}
							else {
								// The state object contains now all parsed headers, free the request buffer.
								free(buf->base);
								DEBUG("path: %s\n", rr->path.base);
								// `open_path_as_file` triggers [on file open](#on-file-open).
								open_path_as_file(rr); 
							}
						}
					}
				}
			}
		}
		else {
			DEBUG("on_tcp_read error: handle->data is NULL.\n");
		}
	}
	else {
		DEBUG("on_tcp_read error: handle is NULL.\n");
	}
}

// ## on connection
static void on_connection(uv_stream_t* server, int status) {
	int r;
	req_res_t *rr;
	uv_stream_t* stream;
	
	DEBUG("on_connection\n");
	
	if (status != 0) {
		fprintf(stderr, "Connect error %s\n", uv_err_name(status));
	}
	else {
		// Create a new stream.
		stream = malloc(sizeof(uv_tcp_t));
		if (stream == NULL) {
			fprintf(stderr, "malloc error\n");
		}
		else {
			r = uv_tcp_init(loop, (uv_tcp_t*)stream);
			if (0 != r) {
				fprintf(stderr, "uv_tcp_init error %s\n", uv_err_name(r));
			}
			else {
				DEBUG("creating new req_res\n");
				rr = req_res_init(stream);
				/* Implicit parameter check.*/
				if (rr == NULL) {
					fprintf(stderr, "res_init error on stream\n");
				}
				else {
					// Associate the state with stream.
					stream->data = rr;
					
					// Accept the connection
					r = uv_accept(server, stream);
					if (0 != r) {
						fprintf(stderr, "uv_accept error %s\n", uv_err_name(r));
					}
					else {
						// Start reading on the stream and register the [on_tcp_read](#on-tcp-read)
						// handler. Libuv will call [request_alloc](#request-alloc) to allocate memory for
						// the request buffer.
						r = uv_read_start(stream, request_alloc, on_tcp_read);
						if (0 != r) {
							fprintf(stderr, "uv_read_start error %s\n", uv_err_name(r));
						}
					}
				}
			}
		}
	}
}
// -----------
// #main
int main(int argc, char *argv[]) {
	struct sockaddr_in addr;
	int port, return_value;
	
	if (argc != 4) {
		fprintf(stderr, "usage: %s <ip> <port> <web-root>\n", argv[0]);
		return_value = -1;
	}
	else {
		// Initialize the global buffers.
		global_bufs_init();
		
		// Create the libuv event loop.
		loop = uv_default_loop();
	
		web_root = argv[3];
		/* ToDo: Add check for argv ! */
		web_root_len = strlen(web_root);
		port = (int)strtol(argv[2], NULL, 0);
		return_value = uv_ip4_addr(argv[1], port, &addr);
		if (0 != return_value) {
			fprintf(stderr, "Listen error %s\n", uv_err_name(return_value));
			return_value = 2;
		}
		else {
			
			// Create a new TCP socket.
			return_value = uv_tcp_init(loop, &tcp_server);
			if (return_value) {
				fprintf(stderr, "Socket creation error %s\n", uv_err_name(return_value));
				return_value = 3;
			}
			
			// Bind the socket to the IPv4 address and port.
			return_value = uv_tcp_bind(&tcp_server, (const struct sockaddr*) &addr, 0);
			if (0 != return_value) {
				fprintf(stderr, "Bind error %s\n", uv_err_name(return_value));
				return_value = 4;
			}
			
			// Register the [on_connection](#on-connection) handler for connections on
			// the socket.
			return_value = uv_listen((uv_stream_t*)&tcp_server, SOMAXCONN, on_connection);
			if (0 != return_value) {
				fprintf(stderr, "Listen error %s\n", uv_err_name(return_value));
				return_value = 5;
			}
			/* ToDo: Add check for argv ! */
			DEBUG("Server listen on %s:%d serving %s\n", argv[1], port, web_root);
			
			// Start the event loop.
			uv_run(loop, UV_RUN_DEFAULT);
			
			return_value = 0;
		}
	}
	return return_value;
}
