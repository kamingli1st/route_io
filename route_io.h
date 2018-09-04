#ifndef ROUTE_IO_HANDLER_H
#define ROUTE_IO_HANDLER_H

#ifdef __cplusplus
extern "C" {
#endif
#if defined _WIN32 || _WIN64

#include <winsock2.h>
#include <mswsock.h>
#include <shlwapi.h>
#include "at_thpool.h"

#pragma comment(lib,"ws2_32")   // Standard socket API.
#pragma comment(lib,"mswsock")  // AcceptEx, TransmitFile, etc,.
#pragma comment(lib,"shlwapi")  // UrlUnescape.
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);
typedef enum { rio_false, rio_true } rio_bool_t;

#define rio_buf_size(b) (SIZE_T) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef enum {
  rio_READABLE   =   1,
  rio_WRITABLE  =   2,
  rio_DONE_WRITE = 3,
  rio_PEER_CLOSED = 4,
  rio_FORCE_CLOSE = 5,
  rio_IDLE = 6,
} rio_state;

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t capacity;
} rio_buf_t;

struct rio_request_s {
  OVERLAPPED ovlp;
  SOCKET listenfd;
  union {
    BYTE addr_block[ ((sizeof( struct sockaddr_in) + 16)) * 2 ];
    struct sockaddr_in client_addr;
  };

  union {
    SOCKET sock;
    int client_addr_len;
  };
  rio_state next_state;
  rio_buf_t *in_buff;
  rio_buf_t *out_buff;
 // HANDLE iocp;
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
  unsigned isudp: 1;
  void* ctx_val;
  SIZE_T sz_per_read;
  int force_close; // force close on host side
};

typedef struct {
  HANDLE iocp;
  rio_init_handler_pt init_handler;
  void* init_arg;
  at_thpool_t *thpool;
} rio_instance_t;

#elif !defined(__APPLE__) && !defined(_WIN32) && !defined(_WIN64)/*Linux*/

#include <stdlib.h>
#include <stdint.h>
#include <netinet/in.h>

typedef struct rio_instance_s rio_instance_t;
typedef struct rio_request_s rio_request_t;
typedef void (*rio_read_handler_pt)(rio_request_t *);
typedef void (*rio_on_conn_close_pt)(rio_request_t *);
typedef void (*rio_init_handler_pt)(void*);
typedef enum { rio_false, rio_true } rio_bool_t;

#define rio_buf_size(b) (size_t) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd

typedef struct rio_buf_s {
  unsigned char *start;
  unsigned char *end;
  size_t total_size;
} rio_buf_t;

struct rio_request_s {
  int sockfd;
  struct sockaddr_in client_addr;
  socklen_t client_addr_len;
  unsigned isudp: 1;
  rio_buf_t *in_buff;
  rio_buf_t *out_buff;
  void* ctx_val;
  rio_instance_t *instance;
  rio_read_handler_pt read_handler;
  rio_on_conn_close_pt on_conn_close_handler;
  struct epoll_event *epev;
};

struct rio_instance_s {
  int epfd;
  int nevents;
  // rio_event_t *evts;
  struct epoll_event *ep_events;
  size_t ep_events_sz;
  rio_init_handler_pt init_handler;
  void* init_arg;
};
    
#elif __APPLE__
    
#include <stdlib.h>
#include <stdint.h>
#include <sys/event.h>
#include <netinet/in.h>
    
    typedef struct rio_instance_s rio_instance_t;
    typedef struct rio_request_s rio_request_t;
    typedef void (*rio_read_handler_pt)(rio_request_t *);
    typedef void (*rio_on_conn_close_pt)(rio_request_t *);
    typedef void (*rio_init_handler_pt)(void*);
    typedef enum { rio_false, rio_true } rio_bool_t;
    
#define rio_buf_size(b) (size_t) (b->end - b->start)
#define rio_add_http_fd rio_add_tcp_fd
    
    typedef struct rio_buf_s {
        unsigned char *start;
        unsigned char *end;
        size_t total_size;
    } rio_buf_t;
    
    struct rio_request_s {
        int sockfd;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len;
        unsigned isudp: 1;
        rio_buf_t *in_buff;
        rio_buf_t *out_buff;
        void* ctx_val;
        rio_instance_t *instance;
        rio_read_handler_pt read_handler;
        rio_on_conn_close_pt on_conn_close_handler;
        struct epoll_event *epev;
    };
    
    struct rio_instance_s {
        int kqfd;
        int nevents;
        struct kevent *kevents;
        rio_init_handler_pt init_handler;
        rio_request_t **__int_req;
        pid_t parent_proc_id;
        void* init_arg;
    };
    

#endif

extern void rio_write_output_buffer(rio_request_t *req, unsigned char* output);
extern void rio_write_output_buffer_l(rio_request_t *req, unsigned char* output, size_t len);
extern rio_instance_t* rio_create_routing_instance(rio_init_handler_pt init_handler, void *arg );
extern int rio_start(rio_instance_t *instance, unsigned int n_concurrent_threads);
extern int rio_add_udp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler,
                          rio_on_conn_close_pt on_conn_close_handler);
extern int rio_add_tcp_fd(rio_instance_t *instance, int port, rio_read_handler_pt read_handler, int backlog,
                          rio_on_conn_close_pt on_conn_close_handler);
extern void rio_set_no_fork(void);
extern void rio_set_max_polling_event(int opt);
extern void rio_set_def_sz_per_read(int opt);
extern void rio_set_curr_req_read_sz(rio_request_t* req, int opt);

/** For HTTP OUTPUT **/
#define rio_http_xform_header "Content-Type: application/x-www-form-urlencoded"
#define rio_http_jsonp_header "Content-Type: application/javascript"
#define rio_http_json_header "Content-Type: application/json"
#define rio_http_textplain_header "Content-Type: text/plain"

extern unsigned char* rio_memstr(unsigned char * start, unsigned char *end, char *pattern);

extern rio_bool_t rio_write_http_status(rio_request_t * request, int statuscode);

extern rio_bool_t rio_write_http_header(rio_request_t * request, char* key, char *val);

extern rio_bool_t rio_write_http_header_2(rio_request_t * request, char* keyval);

extern rio_bool_t rio_write_http_header_3(rio_request_t * request, char* keyval, size_t len);

extern rio_bool_t rio_write_http_content(rio_request_t * request,  char* content);

extern rio_bool_t rio_write_http_content_2(rio_request_t * request, char* content, size_t len);

extern rio_bool_t rio_http_getpath(rio_request_t *req, rio_buf_t *buf);

extern rio_bool_t rio_http_getbody(rio_request_t *req, rio_buf_t *buf);

extern rio_bool_t rio_http_get_queryparam(rio_request_t *req, char *key, rio_buf_t *buf);

#ifdef __cplusplus
}
#endif

#endif

