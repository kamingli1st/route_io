#include <iostream>
#include "route_io.h"
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

    if (strncmp( (char*)req->in_buff->start, "log ", 4) == 0) {
        file_logger->info("%.*s", (int) (req->in_buff->end - req->in_buff->start), req->in_buff->start);
        // file_logger->flush();
    }
    rio_write_output_buffer_l(req, req->in_buff->start, (req->in_buff->end - req->in_buff->start));
}

void on_conn_close_handler(rio_request_t *req) {
	
}

int main(int, char* []) {
    try {
        rio_instance_t * instance = rio_create_routing_instance(init_logger_in_instance, NULL);
#if defined _WIN32 || _WIN64 /*Windows*/
        rio_add_udp_fd(instance, 12345, read_handler, 64, 1024, on_conn_close_handler);
        rio_add_tcp_fd(instance, 8080, read_handler, 64, 1024, on_conn_close_handler);
        rio_start(instance);
#else
        rio_add_udp_fd(instance, 12345, read_handler, on_conn_close_handler);
        rio_add_tcp_fd(instance, 3232, read_handler, 64, on_conn_close_handler);
#endif
        rio_start(instance);
    }
    catch (const spdlog::spdlog_ex& ex)
    {
        std::cout << "Log initialization failed: " << ex.what() << std::endl;
    }


    spdlog::drop_all();
}
