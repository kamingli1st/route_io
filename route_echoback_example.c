
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "route_io.h"

void read_handler(rio_request_t *req);
void on_conn_close_handler(rio_request_t *req);
void init_instance(void *arg);

void init_instance(void *arg) {
  fprintf(stderr, "%s\n", "start instance");
}

void read_handler(rio_request_t *req) {
  /** To varable context reference until the connection closed, it will trigger free context handler **/
  if (req->ctx_val == NULL) {
    req->ctx_val = malloc(1024 * sizeof(char));
  }
  // printf("%d,  %.*s\n", i++, (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);
  rio_write_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
  rio_write_output_buffer(req, (unsigned char*)"\n");
}

void on_conn_close_handler(rio_request_t *req) {
  if (req->ctx_val) {
    free(req->ctx_val);
  }
  fprintf(stderr, "%s\n", "Connection closing");
}

int main(void) {

  rio_instance_t * instance = rio_create_routing_instance(init_instance, NULL);
#if defined _WIN32 || _WIN64 /*Windows*/
  rio_add_udp_fd(instance, 12345, read_handler, 64, 1024, on_conn_close_handler);
  rio_add_tcp_fd(instance, 3232, read_handler, 64, 1024, on_conn_close_handler);
#else
  rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
  rio_add_tcp_fd(instance, 3232, read_handler, 64, on_conn_close_handler);
#endif
  rio_start(instance);

  return 0;
}

