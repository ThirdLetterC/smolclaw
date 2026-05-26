#include "app/app_onboard_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

#include "net/curl_global.h"

#ifdef SC_HAVE_LIBCURL
typedef struct curl_buffer {
    sc_allocator *alloc;
    sc_string body;
} curl_buffer;

static size_t curl_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_buffer *buffer = userdata;
    size_t total = size * nmemb;
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, buffer->alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(&buffer->body));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_str_from_parts(ptr, total));
    }
    sc_string_clear(&buffer->body);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &buffer->body);
    } else {
        sc_string_builder_clear(&builder);
    }
    return sc_status_is_ok(status) ? total : 0;
}

static sc_status curl_post_form(sc_allocator *alloc, const char *url, const char *body, sc_string *out)
{
    CURL *curl = nullptr;
    CURLcode code;
    curl_buffer buffer = {.alloc = alloc};
    struct curl_slist *headers = nullptr;

    sc_status status = sc_curl_global_init("sc.onboard.curl_init_failed");
    if (!sc_status_is_ok(status)) {
        return status;
    }
    curl = curl_easy_init();
    if (curl == nullptr) {
        return sc_status_io("sc.onboard.curl_init_failed");
    }
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");
    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 onboard");
    code = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (code != CURLE_OK) {
        sc_string_clear(&buffer.body);
        return sc_status_http("sc.onboard.oauth_request_failed");
    }
    *out = buffer.body;
    return sc_status_ok();
}

static bool json_field(const sc_string *json, const char *field, sc_allocator *alloc, sc_string *out)
{
    char pattern[128] = {0};
    char *start = nullptr;
    const char *end = nullptr;
    int written = snprintf(pattern, sizeof(pattern), "\"%s\":\"", field);

    if (written < 0 || (size_t)written >= sizeof(pattern) || json->ptr == nullptr) {
        return false;
    }
    start = strstr(json->ptr, pattern);
    if (start == nullptr) {
        return false;
    }
    start += strlen(pattern);
    end = strchr(start, '"');
    if (end == nullptr) {
        return false;
    }
    return sc_status_is_ok(sc_string_from_str(alloc, sc_str_from_parts(start, (size_t)(end - start)), out));
}
#endif

sc_status sc_app_onboard_copilot_device_flow(prompt_context *ctx, sc_allocator *alloc, sc_string *out)
{
#ifndef SC_HAVE_LIBCURL
    (void)ctx;
    (void)alloc;
    (void)out;
    return sc_status_unsupported("sc.onboard.oauth_unsupported");
#else
    const char *client_id = getenv("SMOLCLAW_GITHUB_OAUTH_CLIENT_ID");
    sc_string response = {0};
    sc_string device_code = {0};
    sc_string user_code = {0};
    sc_string verification_uri = {0};
    sc_string access_token = {0};
    sc_status status;
    char body[1024] = {0};

    if (client_id == nullptr || client_id[0] == '\0') {
        return sc_status_unsupported("sc.onboard.copilot_client_id_missing");
    }
    (void)snprintf(body, sizeof(body), "client_id=%s&scope=read:user", client_id);
    status = curl_post_form(alloc, "https://github.com/login/device/code", body, &response);
    if (sc_status_is_ok(status) &&
        (!json_field(&response, "device_code", alloc, &device_code) ||
         !json_field(&response, "user_code", alloc, &user_code) ||
         !json_field(&response, "verification_uri", alloc, &verification_uri))) {
        status = sc_status_parse("sc.onboard.oauth_parse_failed");
    }
    if (sc_status_is_ok(status)) {
        (void)fprintf(ctx->out,
                      "Open %s and enter code %s.\nPress Enter after approving in GitHub.",
                      verification_uri.ptr,
                      user_code.ptr);
        (void)fflush(ctx->out);
        char ignored[8] = {0};
        if (fgets(ignored, sizeof(ignored), ctx->in) == nullptr) {
            status = sc_status_io("sc.onboard.input_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        (void)snprintf(body,
                       sizeof(body),
                       "client_id=%s&device_code=%s&grant_type=urn:ietf:params:oauth:grant-type:device_code",
                       client_id,
                       device_code.ptr);
        sc_string_clear(&response);
        status = curl_post_form(alloc, "https://github.com/login/oauth/access_token", body, &response);
    }
    if (sc_status_is_ok(status) && !json_field(&response, "access_token", alloc, &access_token)) {
        status = sc_status_security_denied("sc.onboard.oauth_authorization_pending");
    }
    if (sc_status_is_ok(status)) {
        *out = access_token;
        access_token = (sc_string){0};
    }
    sc_string_secure_clear(&access_token);
    sc_string_clear(&verification_uri);
    sc_string_clear(&user_code);
    sc_string_secure_clear(&device_code);
    sc_string_clear(&response);
    return status;
#endif
}
