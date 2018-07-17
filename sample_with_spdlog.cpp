#include <iostream>
#include <route_io.h>
/*https://github.com/gabime/spdlog*/
#include "spdlog/spdlog.h"


/***Compile by ***/
/** cd build **/
/** g++ -DSPDLOG_FMT_PRINTF -std=c++11 ../sample_with_spdlog.cpp  -lsrh -pthread **/

static std::shared_ptr<spdlog::logger> file_logger = 0;
void read_handler(rio_request_t *req);
void init_logger_in_instance(void *arg);

void init_logger_in_instance(void *arg) {
    fprintf(stderr, "%s\n", "start instance");
    fprintf(stderr, "%s\n", "init logging");
    auto rotating = std::make_shared<spdlog::sinks::rotating_file_sink_mt> ("mylog.log", 1024 * 1024, 5);
    file_logger = spdlog::create_async("my_logger", rotating, 8192,
        spdlog::async_overflow_policy::block_retry, nullptr, std::chrono::milliseconds{10}/*flush interval*/, nullptr);
}

void read_handler(rio_request_t *req) {
    char *a = "CAUSE ERROR FREE INVALID";

    if (strncmp( (char*)req->in_buff->start, "ERROR", 5) == 0) {
        free(a);
    }

    if (strncmp( (char*)req->in_buff->start, "log ", 4) == 0) {
        file_logger->info("%.*s", (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);
        // file_logger->flush();
    }
    rio_write_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
}

int main(int, char* []) {
    try {
        rio_instance_t * instance = rio_create_routing_instance(24, init_logger_in_instance, NULL);

        rio_add_udp_fd(instance, 12345, read_handler, 1024, NULL);
        rio_add_tcp_fd(instance, 3232, read_handler, 64, NULL);

        rio_start(instance, 1);
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }


    spdlog::drop_all();
}