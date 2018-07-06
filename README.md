# stream_route_handler
stream_route_handler is tcp and udp handler, one instance handler different protocol and port at one time

## Prerequisition Installed

lock-free-queue-and-stack -- https://github.com/Taymindis/lock-free-stack-and-queue.git


## Installation

```bash
mkdir build

cd build

cmake ..

make

sudo make install

```



## Uninstallation

```bash
cd build

sudo make uninstall

```


## Example to run
```c

void read_data(srh_request_t *req);
void read_data(srh_request_t *req) {
  char *a = "CAUSE ERROR FREE INVALID";

  if (strncmp( (char*)req->in_buff->start, "ERROR", 3) == 0) {
    free(a);
  }
  // printf("%d,  %.*s\n", i++, (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);
  srh_set_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
  // printf("%d,  %.*s\n", i++, (int) (req->out_buff->end - req->out_buff->start), req->out_buff->start);
}

int main(void) {

  srh_instance_t * instance = srh_create_routing_instance(24);
  srh_add_udp_fd(instance, 12345, read_data, 1024);
  srh_add_tcp_fd(instance, 3232, read_data, 64);

  srh_start(instance);

  return 0;
}

```
