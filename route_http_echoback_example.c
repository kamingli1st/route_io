#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "route_io.h"

/***Compile instruction ***/
/** cd build **/
/** gcc -std=c11 ../route_http_echoback_example.c -lrouteio **/

void read_handler(rio_request_t *req);
void on_conn_close_handler(rio_request_t *req);
void init_instance(void *arg);

void init_instance(void *arg) {
  fprintf(stderr, "%s\n", "start instance");
}

void read_handler(rio_request_t *req) {
  ssize_t i, curr_size = rio_buf_size(req->in_buff);
  unsigned char* curr_buf = req->in_buff->start;

  rio_buf_t path;
  rio_buf_t body;
  
  rio_http_getpath(req, &path);  
  rio_http_getbody(req, &body);  
  
  printf("%.*s\n", (int)path.total_size, (char*) path.start );
  printf("%.*s\n", (int)body.total_size, (char*) body.start );

  rio_write_http_status(req, 200);
  rio_write_http_header_2(req, rio_http_textplain_header);
  rio_write_http_content_2(req, (char*) path.start, path.total_size);
  /** For debug **/
//   printf("%.*s\n", (int)rio_buf_size(req->out_buff), req->out_buff->start );
}

void on_conn_close_handler(rio_request_t *req) {
  fprintf(stderr, "%s\n", "Connection closing");
}

/*
* Recommend to use 3rd party http proxy for routing if you are planning for HTTP. Such as Nginx
*/

int main(void) {

  rio_instance_t * instance = rio_create_routing_instance(init_instance, NULL);
#if defined _WIN32 || _WIN64 /*Windows*/
  rio_add_udp_fd(instance, 12345, read_handler, 64, 1024, on_conn_close_handler);
  rio_add_http_fd(instance, 3232, read_handler, 64, 1024, on_conn_close_handler);
#else
  rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
  rio_add_http_fd(instance, 3232, read_handler, 64, on_conn_close_handler);
#endif
  rio_start(instance);

  return 0;
}

