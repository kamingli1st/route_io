
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
	printf("%s\n", "start instance");
}

void read_handler(rio_request_t *req) {
	int nbytes = rio_buf_size(req->inbuf);
	fprintf(stderr, "Readed bytes is %d\n", nbytes);

	rio_write_output_buffer_l(req, req->inbuf->start, nbytes);
	rio_write_output_buffer(req, (unsigned char*) "\n");
	// req->force_close = 1;

	// If found the content length, set it to the current request
	// rio_set_curr_req_read_sz(req, {{Content-Length}});
}

void on_conn_close_handler(rio_request_t *req) {
	if (req->ctx_val) {
		free(req->ctx_val);
		req->ctx_val = NULL;
	}
// fprintf(stderr, "%s\n", "Connection closing");
}

int main(void) {
	// rio_set_no_fork();
	rio_set_max_polling_event(64);
	rio_set_def_sz_per_read(1024);

	// one second timeout for read write
	rio_set_rw_timeout(1000, 1000);

	rio_instance_t * instance = rio_create_routing_instance(init_instance, NULL);
	rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
	rio_add_tcp_fd(instance, 3232, read_handler, 128, on_conn_close_handler);

	rio_start(instance, 8);
	
	return 0;
}
