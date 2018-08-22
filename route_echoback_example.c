
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
	int nbytes =(int) rio_buf_size(req->in_buff);
	char terminator_char = *(req->in_buff->end - 1);

	if (terminator_char == '\n') {
		rio_write_output_buffer_l(req, req->in_buff->start, nbytes);
		rio_write_output_buffer(req, (unsigned char*) "\n");
	}

	// If you found the total size needed to read, exchange the read size
	rio_set_curr_req_read_sz(req, 1024);

	// Force close are not recommended in case client has not close the connection.
	//req->force_close = 1;

}

void on_conn_close_handler(rio_request_t *req) {
	if (req->ctx_val) {
		free(req->ctx_val);
		req->ctx_val = NULL;
	}
// fprintf(stderr, "%s\n", "Connection closing");
}

int main(void) {
	rio_set_max_polling_event(1024);
	//rio_set_no_fork();
	rio_set_def_sz_per_read(512);

	rio_instance_t * instance = rio_create_routing_instance(init_instance, NULL);
	rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
	rio_add_tcp_fd(instance, 3232, read_handler, 128, on_conn_close_handler);

	rio_start(instance);

	return 0;
}

