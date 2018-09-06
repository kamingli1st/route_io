#ifndef ROUTE_IO_HANDLER_H
#define ROUTE_IO_HANDLER_H
// #define RIO_HTTP_UTILITY
#ifdef __cplusplus
extern "C" {
#endif
#if defined _WIN32 || _WIN64

#include <winsock2.h>
#include <mswsock.h>
#include <shlwapi.h>
#include "at_thpool.h"
#include <Windows.h>

#pragma comment(lib,"ws2_32")   // Standard socket API.
#pragma comment(lib,"mswsock")  // AcceptEx, TransmitFile, etc,.
#pragma comment(lib,"shlwapi")  // UrlUnescape.
typedef struct rio_request_s rio_request_t;
typedef void(*rio_read_handler_pt)(rio_request_t *);
typedef void(*rio_on_conn_close_pt)(rio_request_t *);
typedef void(*rio_init_handler_pt)(void*);

#define rio_buf_size(b) (int) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef enum {
	rio_SUCCESS = 0,
	rio_SOCK_TIMEOUT = 1,
	rio_ERROR = -1,
} rio_state;

typedef struct rio_buf_s {
	unsigned char *start;
	unsigned char *end;
	int capacity;
} rio_buf_t;

struct rio_request_s {
	SOCKET sockfd;
	rio_buf_t *udp_outbuf;
	struct sockaddr_in client_addr;
	int client_addr_len;
	unsigned isudp : 1;
	rio_buf_t *inbuf;
	void* ctx_val;
	rio_read_handler_pt read_handler;
	rio_on_conn_close_pt on_conn_close_handler;
	int force_close; // force close on host side
	int sz_per_read;
};

typedef struct rio_poll_fd_s {
	WSAPOLLFD pfd;
	rio_request_t *req;
} rio_poll_fd_t;

typedef struct {
	rio_init_handler_pt init_handler;
	void* init_arg;
	at_thpool_t *thpool;
	rio_poll_fd_t *fdarr;
	unsigned int nevents;
} rio_instance_t;

#endif

extern rio_state rio_write_output_buffer(rio_request_t *req, unsigned char* output);
extern rio_state rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t len);
extern rio_instance_t* rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg);
extern int rio_start(rio_instance_t *instance, unsigned int n_concurrent_threads);
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
                          rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          rio_on_conn_close_pt on_conn_close_handler);
extern void rio_set_no_fork(void);
extern void rio_set_max_polling_event(int opt);
extern void rio_set_def_sz_per_read(int opt);
extern void rio_set_rw_timeout(int read_time_ms, int write_time_ms);
extern void rio_set_curr_req_read_sz(rio_request_t *req, int opt);

#ifdef RIO_HTTP_UTILITY
// For HTTP OUTPUT
#define rio_http_xform_header "Content-Type: application/x-www-form-urlencoded"
#define rio_http_jsonp_header "Content-Type: application/javascript"
#define rio_http_json_header "Content-Type: application/json"
#define rio_http_textplain_header "Content-Type: text/plain"

extern unsigned char* rio_memstr(unsigned char * start, unsigned char *end, char *pattern);

extern rio_state rio_write_http_status(rio_request_t * request, int statuscode);

extern rio_state rio_write_http_header(rio_request_t * request, char* key, char *val);

extern rio_state rio_write_http_header_2(rio_request_t * request, char* keyval);

extern rio_state rio_write_http_header_3(rio_request_t * request, char* keyval, size_t len);

extern rio_state rio_write_http_content(rio_request_t * request, char* content);

extern rio_state rio_write_http_content_2(rio_request_t * request, char* content, size_t len);

extern rio_state rio_http_getpath(rio_request_t *req, rio_buf_t *buf);

extern rio_state rio_http_getbody(rio_request_t *req, rio_buf_t *buf);

extern rio_state rio_http_get_queryparam(rio_request_t *req, char *key, rio_buf_t *buf);
#endif
#ifdef __cplusplus
}
#endif

#endif

