
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
  char *a = "CAUSE ERROR FREE INVALID TEST";
  ssize_t i, curr_size = rio_buf_size(req->in_buff);
  unsigned char* curr_buf = req->in_buff->start;
  if (strncmp( (char*)req->in_buff->start, "ERROR", 5) == 0) {
    free(a);
  }

  /** To varable context reference until the connection closed, it will trigger free context handler **/
  if (req->ctx_val == NULL) {
    req->ctx_val = malloc(1024 * sizeof(char));
  }

  // printf("%d,  %.*s\n", i++, (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);

  for (i = 0; i < curr_size; i++) {
    fprintf(stderr, "%c", curr_buf[i]);
  }
  // sleep(3);
  rio_write_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
  rio_write_output_buffer(req, (unsigned char*)"\n");

  // printf("%d,  %.*s\n", i++, (int) (req->out_buff->end - req->out_buff->start), req->out_buff->start);
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
  rio_add_tcp_fd(instance, 8080, read_handler, 64, 1024, on_conn_close_handler);
  rio_start(instance);
#else
  rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
  rio_add_tcp_fd(instance, 3232, read_handler, 64, on_conn_close_handler);
#endif
  rio_start(instance);

  return 0;
}

