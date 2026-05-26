#define _POSIX_C_SOURCE 200809L

#include "net/http_client.h"
#include "test_helpers.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int start_server(const char *response, int *out_port, pid_t *out_pid);
static int run_request(const char *response, size_t max_response_bytes, sc_status_code expected, sc_string *body);
static int test_get_success(void);
static int test_http_status_error(void);
static int test_response_too_large(void);

int main(void)
{
    int failures = 0;

    failures += test_get_success();
    failures += test_http_status_error();
    failures += test_response_too_large();

    return failures == 0 ? 0 : 1;
}

static int start_server(const char *response, int *out_port, pid_t *out_pid)
{
    int listener = -1;
    struct sockaddr_in addr = {0};
    socklen_t addr_len = sizeof(addr);
    pid_t pid = -1;
    int one = 1;

    if (response == nullptr || out_port == nullptr || out_pid == nullptr) {
        return -1;
    }
    listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) {
        return -1;
    }
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        (void)close(listener);
        return -1;
    }
    if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
        (void)close(listener);
        return -1;
    }
    pid = fork();
    if (pid < 0) {
        (void)close(listener);
        return -1;
    }
    if (pid == 0) {
        int client = accept(listener, nullptr, nullptr);
        if (client >= 0) {
            char request[1024] = {0};
            ssize_t bytes_read = read(client, request, sizeof(request));
            ssize_t bytes_written = bytes_read < 0 ? -1 : write(client, response, strlen(response));
            if (bytes_written < 0) {
                (void)close(client);
                (void)close(listener);
                _exit(1);
            }
            (void)close(client);
        }
        (void)close(listener);
        _exit(0);
    }
    *out_port = ntohs(addr.sin_port);
    *out_pid = pid;
    (void)close(listener);
    return 0;
}

static int run_request(const char *response, size_t max_response_bytes, sc_status_code expected, sc_string *body)
{
    int failures = 0;
    int port = 0;
    pid_t pid = 0;
    char url[128] = {0};
    sc_http_response http = {0};
    sc_status status;

    failures += sc_test_expect_true("start server", start_server(response, &port, &pid) == 0);
    if (failures != 0) {
        return failures;
    }
    (void)snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    status = sc_http_client_perform_sync(sc_allocator_heap(),
                                         &(sc_http_request){
                                             .struct_size = sizeof(sc_http_request),
                                             .method = sc_str_from_cstr("GET"),
                                             .url = sc_str_from_cstr(url),
                                             .user_agent = sc_str_from_cstr("smolclaw-test-http-client"),
                                             .max_response_bytes = max_response_bytes,
                                             .timeout_ms = 5000,
                                             .connect_timeout_ms = 1000,
                                         },
                                         &http);
    failures += sc_test_expect_status("request status", status, expected);
    if (expected == SC_OK && body != nullptr) {
        failures += sc_test_expect_status("copy body",
                                  sc_string_from_str(sc_allocator_heap(),
                                                     sc_str_from_parts((const char *)http.body.ptr, http.body.len),
                                                     body),
                                  SC_OK);
    }
    sc_http_response_clear(&http);
    (void)waitpid(pid, nullptr, 0);
    return failures;
}

static int test_get_success(void)
{
    int failures = 0;
    sc_string body = {0};

    failures += run_request("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", 128, SC_OK, &body);
    failures += sc_test_expect_true("body ok", sc_str_equal(sc_string_as_str(&body), sc_str_from_cstr("ok")));
    sc_string_clear(&body);
    return failures;
}

static int test_http_status_error(void)
{
    return run_request("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 5\r\n\r\nerror", 128, SC_ERR_HTTP, nullptr);
}

static int test_response_too_large(void)
{
    return run_request("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nlarge", 2, SC_ERR_HTTP, nullptr);
}
