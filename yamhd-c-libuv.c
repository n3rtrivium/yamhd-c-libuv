//     yamhd-c-libuv
//     (c) 2014 Martin Wind
//     http://windm.at

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <uv.h>

// ### Defines and function declaration
// 
// If the requeste contains an `path` bigger than `MAX_PATH_SIZE` discard it.
#define MAX_PATH_SIZE 256
#define READ_BUF_SIZE 4096
#define HEADERS_BUF_SIZE 1024

// Comment / uncommet this line to toggle debug mode.
#define DEBUG_BUILD

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

static void on_tcp_write(uv_write_t* req, int status);
static void on_tcp_read(uv_stream_t*, ssize_t nread, const uv_buf_t* buf);
static void on_file_read(uv_fs_t * req);
static void on_file_open(uv_fs_t* req);
static void on_close(uv_handle_t* peer);
static void on_connection(uv_stream_t*, int status);

// # State struct

// This struct is passed to all callbacks to associate the callback with an request 
// and response. Furthermore this struct is used to free all memory of one request in
// an convenient way. 
typedef struct {
	char* method[5];
	char* path;
	size_t path_len;
	int close_after_write;
	int read_on_write;
	int file;
	int pending_requests;
	int dead;
	size_t file_pos;
	uv_stream_t* stream;
	uv_buf_t* read;
	uv_buf_t* headers;
} req_res_t;

// # Stop reading!
// Since this file starts with utility and callback function it is an good idea to read 
// the file bottom up. [Goto main](#main)

// ## State

// This function ensures the default values of the state struct. 
static req_res_t* req_res_init(uv_stream_t* handle) {
	req_res_t* rr = malloc(sizeof(req_res_t));
	if (rr == NULL) return NULL;
	rr->path = NULL;
	rr->path_len = 0;
	rr->close_after_write = 0;
	rr->read_on_write = 0;
	rr->stream = handle;
	rr->read = NULL;
	rr->headers = NULL;
	rr->file = 0;
	rr->file_pos = 0;
	rr->pending_requests = 0;
	rr->dead = 0;
	DEBUG("concurrent_states: %zu\n", ++concurrent_states);
	return rr;
}

// Initialize the read buffer of the sate.
static void req_res_init_read_buf(req_res_t* rr) {
	if (rr->read != NULL) {
		DEBUG("req_res_init_read_buf error: read not NULL");
		return;
	}
	rr->read = malloc(sizeof(uv_buf_t));
	rr->read->base = malloc(READ_BUF_SIZE);
	rr->read->len = READ_BUF_SIZE;
}

// Initialize the headers buffer of the sate.
static void req_res_init_headers_buf(req_res_t* rr) {
	if (rr->headers != NULL) {
		DEBUG("req_res_init_headers_buf error: header not NULL");
		return;
	}
	rr->headers = malloc(sizeof(uv_buf_t));
	rr->headers->base = malloc(HEADERS_BUF_SIZE);
	rr->headers->len = HEADERS_BUF_SIZE;
}

// Free all memory of one request.
static void req_res_free(req_res_t* rr) {
	// Check for pending requests
	if (rr->pending_requests > 0) {
		DEBUG("req_res_free pending requests: %d\n", rr->pending_requests);
		// Close the stream if it is not already closed.
		if (!uv_is_closing((uv_handle_t *) rr->stream)) {
			uv_close((uv_handle_t *) rr->stream, on_close);
			rr->stream = NULL;
		}
		rr->dead = 1;
		return;
	} else if(rr->dead) {
		DEBUG("req_res_free free dead request\n");
	}
	// Free the `path` string.
	if (rr->path != NULL) free(rr->path);
	// Free the read buffer.
	if (rr->read != NULL) {
		free(rr->read->base);
		free(rr->read);
	}
	// Free the headers buffer.
	if (rr->headers != NULL) {
		free(rr->headers->base);
		free(rr->headers);
	}
	// Close the file descriptor.
	if (rr->file != 0) {
		uv_fs_t close_req;
		// `NULL` as callback function makes the call synchron.
		if (0 != uv_fs_close(loop, &close_req, rr->file, NULL)) {
			fprintf(stderr, "uv_fs_close error");
		}
		uv_fs_req_cleanup(&close_req);
	}
	// Close the stream if it is not already closed.
	if (rr->stream != NULL && !uv_is_closing((uv_handle_t *) rr->stream)) {
		uv_close((uv_handle_t *) rr->stream, on_close);
	}
	// Free the state struct.
	free(rr);
	DEBUG("concurrent_states: %zu\n", --concurrent_states);
}

// ## send_headers helper
static void send_headers(req_res_t* rr, const char* headers) {
	
	DEBUG("send_headers\n");
	
	// Initialize the headers buffer.
	req_res_init_headers_buf(rr);
	
	// Get the UTC `struct tm`.
	time_t now = time(NULL);
	struct tm* utc = gmtime(&now);
	if (utc == NULL) {
		fprintf(stderr, "gmtime error");
		return;
	}
	
	// Expand the headers.
	if (strftime(rr->headers->base, HEADERS_BUF_SIZE, headers, utc) == 0) {
		fprintf(stderr, "strftime error");
		return;
	}
	rr->headers->len = strlen(rr->headers->base);
	
	// Create an write request and dispatch it. 
	uv_write_t* wr = (uv_write_t*) malloc(sizeof(uv_write_t));
	wr->data = rr;
	rr->pending_requests++;
	if (uv_write(wr, rr->stream, rr->headers, 1, on_tcp_write)) {
		fprintf(stderr, "uv_write send_headers failed\n");
		rr->pending_requests--;
		req_res_free(rr);
		free(wr);
		return;
	}
}

// #### on close callback
//
// Free the stream.
static void on_close(uv_handle_t* peer) {
	free(peer);
}

// ## on tcp write callback
static void on_tcp_write(uv_write_t* req, int status) {
	req_res_t* rr = (req_res_t*) req->data;
	rr->pending_requests--;
	
	// Check if an error happend in an other callback.
	if (rr->dead) return req_res_free(rr);
	
	DEBUG("on_tcp_write\n");
	
	// Free the write request.
	free(req);
	
	// Check for write error.
	if (status != 0) {
		fprintf(stderr, "uv_write error: %s - %s\n", uv_err_name(status), uv_strerror(status));
		req_res_free(rr);
		return;
	}
	
	// Either close the connection or read more data of the file.
	if (rr->close_after_write) {
		DEBUG("on_tcp_write is closing the connection\n");
		req_res_free(rr);
	} else if (rr->read_on_write) {
		// Create the file read request and dispatch it. 
		// [See on_file_open](#on-file-open-callback).
		uv_fs_t* fs_req = malloc(sizeof(uv_fs_t));
		fs_req->data = rr;
		rr->pending_requests++;
		int r = uv_fs_read(loop, fs_req, rr->file, rr->read, 1, rr->file_pos, on_file_read);
		if  (r != 0) {
			fprintf(stderr, "read error: %d\n", r);
			rr->pending_requests--;
			req_res_free(rr);
			free(fs_req);
		}
	}
}

// ## on file read callback
static void on_file_read(uv_fs_t * req) {
	int status = req->result;
	req_res_t* rr = (req_res_t*) req->data;
	rr->pending_requests--;
	
	// Check if an error happend in an other callback.
	if (rr->dead) return req_res_free(rr);
	
	DEBUG("on_file_read %s\n", rr->path);
	
	// Free the read request.
	uv_fs_req_cleanup(req);
	free(req);
	
	// Check for read error.
	if(status < 0 && status != UV_EOF) {
		fprintf(stderr, "on_file_read error: %s\n", uv_strerror(status));
		req_res_free(rr);
		return;
	}
	// Check if the hole file is already read.
	if (status == UV_EOF || status == 0) {
		DEBUG("read reach end of file - closing connection\n");
		req_res_free(rr);
		return;
	}
	
	DEBUG("read %d bytes\n", status);
	
	// Create the tcp stream write request, associate it with the state, update the state
	// and dispatch the request. [on_tcp_write](#on-tcp-write-callback) gets called a soon
	// as the date is written to the stream.
	uv_write_t* wr = (uv_write_t*) malloc(sizeof(uv_write_t));
	wr->data = rr;
	rr->read_on_write = 1;
	rr->file_pos += status;
	rr->read->len = status;
	rr->pending_requests++;
	if (uv_write(wr, rr->stream, rr->read, 1, on_tcp_write)) {
		fprintf(stderr, "uv_write headers failed\n");
		rr->pending_requests--;
		req_res_free(rr);
		free(wr);
	}
}


// ## on file open callback
static void on_file_open(uv_fs_t* req) {
	int status = req->result;
	req_res_t* rr = (req_res_t*) req->data;
	rr->pending_requests--;
	
	// Check if an error happend in an other callback.
	if (rr->dead) return req_res_free(rr);
	
	DEBUG("on_file_open %s\n", rr->path);
	
	uv_fs_req_cleanup(req);
	free(req);
	
	// Check if `uv_fs_open` returned an error.
	if (status < 0) {
		// Send an `404` response only on `File not found` error. Note that it is 
		// important to close the file on `EMFILE` and other errors.
		if (status == UV_ENOENT) {
			// `rr->close_after_write = 1` makes `on_tcp_write` close the connection.
			rr->close_after_write = 1;
			send_headers(rr, "HTTP/1.1 404 Not Found\n"
							 "Date: %a, %d %b %Y %H:%M:%S %Z\n"
							 "Content-Type: text/html\n"
							 "\n"
							 "<h1>Not found</h1>");
			return;
		}
		fprintf(stderr, "async open error '%s': %s\n", rr->path, uv_strerror(status));
		// Close the file and free the memory.
		req_res_free(rr);
		return;
	}

	// Store the file handle.
	rr->file = status;
	// Send the `200` response headers.
	rr->read_on_write = 1;
	req_res_init_read_buf(rr);
	// Send the `200` response headers.
	send_headers(rr, "HTTP/1.0 200 OK\n"
					 "Date: %a, %d %b %Y %H:%M:%S %Z\n"
					 "Content-Type: text/html\n\n");
}

// ### Simple request header parser
static int parse(const uv_buf_t* req, req_res_t* rr) {
	
	if (rr == NULL || req == NULL) return -1;
	
	// Check if its an `GET` request.
	if (strncmp(req->base, "GET ", 4) != 0) {
		fprintf(stderr, "Not a GET request\n");
		return -2;
	}
	
	// Search for the end of the `path`.
	int path_start = 4;
	int path_len = 0;
	while (path_len+path_start < req->len) {
		if (req->base[path_len+path_start] == ' ') {
			break;
		}
		path_len++;
	}
	// Validate the `path` length.
	if (path_len == 0 || path_len > MAX_PATH_SIZE) {
		fprintf(stderr, "Path invalid\n");
		return -3;
	}
	// Check if its an `HTTP` version 1.* request.
	if (path_start+path_len+7 > req->len || 
		strncmp(req->base+path_start+path_len, " HTTP/1", 7) != 0)
	{
		fprintf(stderr, "Not a HTTP/1 request\n");
		return -4;
	}
	// Allocate memory for the `path`.
	rr->path = malloc(web_root_len+path_len+1);
	if (rr->path == NULL) {
		fprintf(stderr, "malloc error\n");
		return -5;
	}
	// Copy the path and terminate it with `\0`.
	strcpy(rr->path, web_root);
	memcpy(rr->path+web_root_len, req->base+path_start, path_len+1);
	rr->path[path_len+web_root_len] = '\0';
	rr->path_len = path_len+web_root_len;
	
	return 0;
}

// ## on tcp read callback
static void on_tcp_read(uv_stream_t* handle, ssize_t nread, const uv_buf_t* buf) {
	int r;
	req_res_t *rr = (req_res_t*) handle->data;
	uv_fs_t* file_open_req;
	
	DEBUG("on_tcp_read\n");
	
	// Check if an error happened in an other callback.
	if (rr->dead) return req_res_free(rr);
	
	// Check if `uv_read` returned an error.
	if (nread < 0) {
		// Print every error except end of file (UV_EOF).
		if (nread != UV_EOF) {
			fprintf(stderr, "on_tcp_read error: %s\n", uv_err_name(nread));
		}
		
		// Free the memory allocated by `request_alloc`.
		if (buf->base) 
			free(buf->base);
		
		// Close the stream.
		req_res_free(rr);
		return;
	}
	
	if (nread == 0) {
		// Everything OK, but nothing read.
		free(buf->base);
		return;
	}
	
	// Check if an previous `on_tcp_read` has parsed the headers.
	if (rr->path != NULL) {
		DEBUG("on_tcp_read on already parsed headers\n");
		free(buf->base);
		return;
	}
	
	// parse the request headers.
	r = parse(buf, rr);
	if (r != 0) {
		req_res_free(rr);
		return;
	}
	// The state object contains now all parsed headers, free the request buffer.
	if (buf->base) 
		free(buf->base);
	
	DEBUG("path: %s\n", rr->path);
	
	// Create an file open request, associate the sate and dispatch it on the loop. The
	// [on_file_open](#on-file-open-callback) is invoked once the file is opened.
	file_open_req = malloc(sizeof(uv_fs_t));
	file_open_req->data = rr;
	rr->pending_requests++;
	r = uv_fs_open(loop, file_open_req, rr->path, O_RDONLY, S_IFREG, on_file_open);
	if (r != 0) {
		fprintf(stderr, "uv_fs_open error: %d\n", r);
		rr->pending_requests--;
		req_res_free(rr);
		free(file_open_req);
		return;
	}
}

// #### request alloc

// Libuv does not allocate memory. Thus this function is provided to `uv_read_start` to
// allocate memory for the request buffer.
static void request_alloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	buf->base = malloc(suggested_size);
	buf->len = suggested_size;
}

// ## on connection callback
static void on_connection(uv_stream_t* server, int status) {
	int r;
	req_res_t *rr;
	uv_stream_t* stream;
	
	DEBUG("on_connection\n");
	
	if (status != 0) {
		fprintf(stderr, "Connect error %s\n", uv_err_name(status));
		return;
	}

	// Create a new stream.
	stream = malloc(sizeof(uv_tcp_t));
	if (stream == NULL) {
		fprintf(stderr, "malloc error\n");
		return;
	}
	r = uv_tcp_init(loop, (uv_tcp_t*)stream);
	if (0 != r) {
		fprintf(stderr, "uv_tcp_init error %s\n", uv_err_name(r));
		return;
	}
	
	DEBUG("creating new req_res\n");
	rr = req_res_init(stream);
	
	// Associate the state with stream.
	stream->data = rr;

	// Accept the connection
	r = uv_accept(server, stream);
	if (0 != r) {
		fprintf(stderr, "uv_accept error %s\n", uv_err_name(r));
		return;
	}
	
	// Start reading on the stream and register the [on_tcp_read](#on-tcp-read-callback)
	// callback. Libuv will call [request_alloc](#request-alloc) to allocate memory for 
	// the request buffer.
	r = uv_read_start(stream, request_alloc, on_tcp_read);
	if (0 != r) {
		fprintf(stderr, "uv_read_start error %s\n", uv_err_name(r));
	}

}

// #main
int main(int argc, char *argv[]) {
	struct sockaddr_in addr;
	int r, port;
	
	if (argc != 4) {
		fprintf(stderr, "usage: %s <ip> <port> <web-root>\n", argv[0]);
		return -1;
	}
	
	// Create the libuv event loop.
	loop = uv_default_loop();
	
	web_root = argv[3];
	web_root_len = strlen(web_root);
	port = (int)strtol(argv[2], NULL, 0);
	r = uv_ip4_addr(argv[1], port, &addr);
	if (0 != r) {
		fprintf(stderr, "Listen error %s\n", uv_err_name(r));
		return 2;
	}
	
	// Create an new TCP socket.
	r = uv_tcp_init(loop, &tcp_server);
	if (r) {
		fprintf(stderr, "Socket creation error %s\n", uv_err_name(r));
		return 3;
	}

	// Bind the socket to the IPv4 address and port.
	r = uv_tcp_bind(&tcp_server, (const struct sockaddr*) &addr, 0);
		if (r) {
		fprintf(stderr, "Bind error %s\n", uv_err_name(r));
		return 4;
	}

	// Register the [on_connection](#on-connection-callback) callback for connections on 
	// the socket. 
	r = uv_listen((uv_stream_t*)&tcp_server, SOMAXCONN, on_connection);
	if (r) {
		fprintf(stderr, "Listen error %s\n", uv_err_name(r));
		return 5;
	}
	
	DEBUG("Server listen on %s:%d serving %s\n", argv[1], port, web_root);
	
	// Start the event loop.
	uv_run(loop, UV_RUN_DEFAULT);

	return 0;
}
