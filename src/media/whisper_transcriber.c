// cppcheck-suppress-file redundantInitialization
#include "sc/media.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sc/api.h"
#include "sc/json.h"

#include "net/curl_global.h"

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

typedef struct whisper_transcriber {
    sc_allocator *alloc;
    sc_string endpoint_url;
    sc_string model;
    sc_string default_language;
    sc_string prompt;
    sc_string temperature;
    sc_string temperature_inc;
    sc_string response_format;
    uint32_t timeout_ms;
    size_t max_audio_bytes;
    size_t max_response_bytes;
    sc_transcriber_whisper_http_fn http_request;
    void *http_user;
} whisper_transcriber;

#ifdef SC_HAVE_LIBCURL
typedef struct whisper_curl_buffer {
    sc_string_builder builder;
    size_t max_bytes;
    bool too_large;
} whisper_curl_buffer;
#endif

static sc_status whisper_transcribe(void *impl,
                                    const sc_transcription_request *request,
                                    sc_allocator *alloc,
                                    sc_transcription_result *out);
static void whisper_destroy(void *impl);
static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out);
static bool whisper_endpoint_url_allowed(sc_str url);
static bool str_has_prefix(sc_str value, const char *prefix);
static bool loopback_host_allowed(sc_str host);
static sc_status validate_response_format(sc_str response_format);
static sc_status attachment_upload_size(const sc_media_attachment *attachment, size_t *out);
static sc_status parse_whisper_response(sc_allocator *alloc, sc_str body, sc_str response_format, sc_string *out);
static sc_status extract_json_text(sc_allocator *alloc, sc_str body, sc_string *out);
static sc_status whisper_default_http(void *user,
                                      const sc_transcriber_whisper_http_request *request,
                                      sc_allocator *alloc,
                                      sc_string *out);

#ifdef SC_HAVE_LIBCURL
static size_t whisper_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
static sc_status curl_add_mime_text(curl_mime *mime, const char *name, sc_str value);
#endif

static const sc_transcriber_vtab whisper_vtab = {
    .struct_size = sizeof(sc_transcriber_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "whisper_cpp",
    .display_name = "whisper.cpp ASR",
    .feature_flag = "SC_TRANSCRIBER_WHISPER_CPP",
    .capabilities = SC_CONTRACT_CAP_BINARY,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .transcribe = whisper_transcribe,
    .destroy = whisper_destroy,
};

sc_status sc_transcriber_whisper_new(sc_allocator *alloc,
                                     const sc_transcriber_whisper_options *options,
                                     sc_transcriber **out)
{
    whisper_transcriber *impl = nullptr;
    sc_status status = sc_status_ok();
    sc_str endpoint_url = {0};
    sc_str model = {0};
    sc_str response_format = {0};

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.transcriber.whisper.invalid_argument");
    }
    *out = nullptr;
    endpoint_url = options->endpoint_url.len == 0
                       ? sc_str_from_cstr("http://127.0.0.1:2022/v1/audio/transcriptions")
                       : options->endpoint_url;
    model = options->model.len == 0 ? sc_str_from_cstr("whisper-1") : options->model;
    response_format = options->response_format.len == 0 ? sc_str_from_cstr("json") : options->response_format;
    if (endpoint_url.ptr == nullptr || endpoint_url.len == 0 || model.ptr == nullptr || model.len == 0) {
        return sc_status_invalid_argument("sc.transcriber.whisper.invalid_argument");
    }
    if (!whisper_endpoint_url_allowed(endpoint_url)) {
        return sc_status_security_denied("sc.transcriber.whisper.endpoint_denied");
    }
    status = validate_response_format(response_format);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(whisper_transcriber));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (whisper_transcriber){
        .alloc = alloc,
        .timeout_ms = options->timeout_ms == 0 ? 30'000 : options->timeout_ms,
        .max_audio_bytes = options->max_audio_bytes == 0 ? 25U * 1024U * 1024U : options->max_audio_bytes,
        .max_response_bytes = options->max_response_bytes == 0 ? 64U * 1024U : options->max_response_bytes,
        .http_request = options->http_request == nullptr ? whisper_default_http : options->http_request,
        .http_user = options->http_user,
    };
    status = copy_string(alloc, endpoint_url, &impl->endpoint_url);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, model, &impl->model);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->default_language, &impl->default_language);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->prompt, &impl->prompt);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->temperature, &impl->temperature);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->temperature_inc, &impl->temperature_inc);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, response_format, &impl->response_format);
    }
    if (sc_status_is_ok(status)) {
        status = sc_transcriber_new(alloc, &whisper_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        whisper_destroy(impl);
    }
    return status;
}

static sc_status whisper_transcribe(void *impl,
                                    const sc_transcription_request *request,
                                    sc_allocator *alloc,
                                    sc_transcription_result *out)
{
    whisper_transcriber *whisper = impl;
    sc_string body = {0};
    sc_status status = sc_status_ok();
    sc_str language = {0};
    size_t upload_size = 0;

    if (whisper == nullptr || request == nullptr || request->attachment == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.transcriber.whisper.invalid_argument");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.transcriber.whisper.cancelled");
    }
    status = validate_response_format(sc_string_as_str(&whisper->response_format));
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = attachment_upload_size(request->attachment, &upload_size);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (upload_size == 0) {
        return sc_status_invalid_argument("sc.transcriber.whisper.audio_missing");
    }
    if (upload_size > whisper->max_audio_bytes) {
        return sc_status_security_denied("sc.transcriber.whisper.audio_too_large");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_transcription_result){.struct_size = sizeof(*out)};
    language = request->language.len > 0 ? request->language : sc_string_as_str(&whisper->default_language);
    sc_transcriber_whisper_http_request http_request = {
        .struct_size = sizeof(http_request),
        .method = sc_str_from_cstr("POST"),
        .url = sc_string_as_str(&whisper->endpoint_url),
        .attachment = request->attachment,
        .model = sc_string_as_str(&whisper->model),
        .language = language,
        .prompt = sc_string_as_str(&whisper->prompt),
        .temperature = sc_string_as_str(&whisper->temperature),
        .temperature_inc = sc_string_as_str(&whisper->temperature_inc),
        .response_format = sc_string_as_str(&whisper->response_format),
        .timeout_ms = request->timeout_ms == 0 ? whisper->timeout_ms : request->timeout_ms,
        .max_audio_bytes = whisper->max_audio_bytes,
        .max_response_bytes = whisper->max_response_bytes,
    };
    status = whisper->http_request(whisper->http_user, &http_request, alloc, &body);
    if (sc_status_is_ok(status)) {
        status = parse_whisper_response(alloc, sc_string_as_str(&body), sc_string_as_str(&whisper->response_format), &out->text);
    }
    if (!sc_status_is_ok(status)) {
        sc_transcription_result_clear(out);
    }
    sc_string_clear(&body);
    return status;
}

static void whisper_destroy(void *impl)
{
    whisper_transcriber *whisper = impl;
    if (whisper == nullptr) {
        return;
    }
    sc_string_clear(&whisper->endpoint_url);
    sc_string_clear(&whisper->model);
    sc_string_clear(&whisper->default_language);
    sc_string_clear(&whisper->prompt);
    sc_string_clear(&whisper->temperature);
    sc_string_clear(&whisper->temperature_inc);
    sc_string_clear(&whisper->response_format);
    sc_free(whisper->alloc, whisper, sizeof(*whisper), _Alignof(whisper_transcriber));
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out)
{
    return sc_string_from_str(alloc, input.ptr == nullptr ? sc_str_from_cstr("") : input, out);
}

static bool whisper_endpoint_url_allowed(sc_str url)
{
    sc_str rest = {0};
    size_t host_len = 0;
    size_t scheme_len = 0;
    if (str_has_prefix(url, "https://")) {
        return true;
    }
    if (!str_has_prefix(url, "http://")) {
        return false;
    }
    scheme_len = strlen("http://");
    rest = sc_str_from_parts(url.ptr + scheme_len, url.len - scheme_len);
    while (host_len < rest.len && rest.ptr[host_len] != '/' && rest.ptr[host_len] != ':') {
        host_len += 1;
    }
    return loopback_host_allowed(sc_str_from_parts(rest.ptr, host_len));
}

static bool str_has_prefix(sc_str value, const char *prefix)
{
    size_t prefix_len = prefix == nullptr ? 0 : strlen(prefix);
    return value.ptr != nullptr && value.len >= prefix_len && memcmp(value.ptr, prefix, prefix_len) == 0;
}

static bool loopback_host_allowed(sc_str host)
{
    if (sc_str_equal(host, sc_str_from_cstr("localhost")) ||
        sc_str_equal(host, sc_str_from_cstr("127.0.0.1")) ||
        sc_str_equal(host, sc_str_from_cstr("::1")) ||
        sc_str_equal(host, sc_str_from_cstr("[::1]"))) {
        return true;
    }
    if (host.len >= 4 && memcmp(host.ptr, "127.", 4) == 0) {
        for (size_t i = 4; i < host.len; i += 1) {
            if (!(isdigit((unsigned char)host.ptr[i]) || host.ptr[i] == '.')) {
                return false;
            }
        }
        return true;
    }
    return false;
}

static sc_status validate_response_format(sc_str response_format)
{
    if (sc_str_equal(response_format, sc_str_from_cstr("json")) ||
        sc_str_equal(response_format, sc_str_from_cstr("verbose_json")) ||
        sc_str_equal(response_format, sc_str_from_cstr("text"))) {
        return sc_status_ok();
    }
    if (sc_str_equal(response_format, sc_str_from_cstr("srt")) ||
        sc_str_equal(response_format, sc_str_from_cstr("vtt"))) {
        return sc_status_unsupported("sc.transcriber.whisper.subtitle_format_unsupported");
    }
    return sc_status_invalid_argument("sc.transcriber.whisper.response_format_invalid");
}

static sc_status attachment_upload_size(const sc_media_attachment *attachment, size_t *out)
{
    struct stat st = {0};
    if (attachment == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.transcriber.whisper.attachment_invalid");
    }
    if (attachment->size_bytes > 0) {
        *out = attachment->size_bytes;
        return sc_status_ok();
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_BYTES) {
        *out = attachment->bytes.len;
        return sc_status_ok();
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_PATH && attachment->storage_path.ptr != nullptr) {
        sc_string path = {0};
        sc_status status = sc_string_from_str(sc_allocator_heap(), attachment->storage_path, &path);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        if (stat(path.ptr, &st) != 0 || st.st_size < 0) {
            sc_string_clear(&path);
            return sc_status_io("sc.transcriber.whisper.audio_stat_failed");
        }
        sc_string_clear(&path);
        *out = (size_t)st.st_size;
        return sc_status_ok();
    }
    return sc_status_invalid_argument("sc.transcriber.whisper.attachment_storage_invalid");
}

static sc_status parse_whisper_response(sc_allocator *alloc, sc_str body, sc_str response_format, sc_string *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.transcriber.whisper.response_invalid_argument");
    }
    if (sc_str_equal(response_format, sc_str_from_cstr("text"))) {
        return sc_string_from_str(alloc, body, out);
    }
    return extract_json_text(alloc, body, out);
}

static sc_status extract_json_text(sc_allocator *alloc, sc_str body, sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *text_value = nullptr;
    sc_json_parse_error error = {0};
    sc_str text = {0};
    sc_status status = sc_json_parse(alloc, body, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    text_value = sc_json_object_get(root, sc_str_from_cstr("text"));
    if (!sc_json_as_str(text_value, &text)) {
        sc_json_destroy(root);
        return sc_status_parse("sc.transcriber.whisper.text_missing");
    }
    status = sc_string_from_str(alloc, text, out);
    sc_json_destroy(root);
    return status;
}

static sc_status whisper_default_http(void *user,
                                      const sc_transcriber_whisper_http_request *request,
                                      sc_allocator *alloc,
                                      sc_string *out)
{
    (void)user;
    (void)request;
    (void)alloc;
    (void)out;
#ifndef SC_HAVE_LIBCURL
    return sc_status_unsupported("sc.transcriber.whisper.libcurl_unavailable");
#else
    CURL *curl = nullptr;
    CURLcode code;
    curl_mime *mime = nullptr;
    curl_mimepart *part = nullptr;
    whisper_curl_buffer response = {0};
    sc_string filename = {0};
    sc_string path = {0};
    long response_code = 0;
    sc_status status = sc_status_ok();

    if (request == nullptr || request->attachment == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.transcriber.whisper.http_invalid_argument");
    }
    status = sc_curl_global_init("sc.transcriber.whisper.curl_init_failed");
    if (!sc_status_is_ok(status)) {
        return sc_status_http("sc.transcriber.whisper.curl_init_failed");
    }
    curl = curl_easy_init();
    if (curl == nullptr) {
        return sc_status_http("sc.transcriber.whisper.curl_init_failed");
    }
    sc_string_builder_init(&response.builder, alloc);
    response.max_bytes = request->max_response_bytes == 0 ? 64U * 1024U : request->max_response_bytes;
    mime = curl_mime_init(curl);
    if (mime == nullptr) {
        status = sc_status_no_memory();
    }
    if (sc_status_is_ok(status)) {
        part = curl_mime_addpart(mime);
        if (request->attachment->filename.len > 0) {
            status = copy_string(alloc, request->attachment->filename, &filename);
        }
        if (sc_status_is_ok(status)) {
            if (part == nullptr ||
                curl_mime_name(part, "file") != CURLE_OK ||
                (filename.len > 0 && curl_mime_filename(part, filename.ptr) != CURLE_OK)) {
                status = sc_status_no_memory();
            } else if (request->attachment->storage_kind == SC_MEDIA_STORAGE_BYTES) {
                if (curl_mime_data(part, (const char *)request->attachment->bytes.ptr, request->attachment->bytes.len) != CURLE_OK) {
                    status = sc_status_http("sc.transcriber.whisper.mime_failed");
                }
            } else if (request->attachment->storage_kind == SC_MEDIA_STORAGE_PATH) {
                status = copy_string(alloc, request->attachment->storage_path, &path);
                if (sc_status_is_ok(status) && curl_mime_filedata(part, path.ptr) != CURLE_OK) {
                    status = sc_status_http("sc.transcriber.whisper.mime_failed");
                }
            } else {
                status = sc_status_invalid_argument("sc.transcriber.whisper.attachment_storage_invalid");
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = curl_add_mime_text(mime, "model", request->model);
    }
    if (sc_status_is_ok(status) && request->language.len > 0) {
        status = curl_add_mime_text(mime, "language", request->language);
    }
    if (sc_status_is_ok(status) && request->prompt.len > 0) {
        status = curl_add_mime_text(mime, "prompt", request->prompt);
    }
    if (sc_status_is_ok(status) && request->temperature.len > 0) {
        status = curl_add_mime_text(mime, "temperature", request->temperature);
    }
    if (sc_status_is_ok(status) && request->temperature_inc.len > 0) {
        status = curl_add_mime_text(mime, "temperature_inc", request->temperature_inc);
    }
    if (sc_status_is_ok(status) && request->response_format.len > 0) {
        status = curl_add_mime_text(mime, "response_format", request->response_format);
    }
    if (sc_status_is_ok(status)) {
        (void)curl_easy_setopt(curl, CURLOPT_URL, request->url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, whisper_curl_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 whisper-cpp-asr");
        (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, request->timeout_ms < 10'000 ? (long)request->timeout_ms : 10'000L);
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, request->timeout_ms == 0 ? 30'000L : (long)request->timeout_ms);
        (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        code = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response.too_large) {
            status = sc_status_security_denied("sc.transcriber.whisper.response_too_large");
        } else if (code == CURLE_OPERATION_TIMEDOUT) {
            status = sc_status_timeout("sc.transcriber.whisper.timeout");
        } else if (code != CURLE_OK) {
            status = sc_status_http("sc.transcriber.whisper.http_failed");
        } else if (response_code < 200 || response_code >= 300) {
            status = sc_status_http("sc.transcriber.whisper.http_status_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&response.builder, out);
    }
    sc_string_clear(&path);
    sc_string_clear(&filename);
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&response.builder);
    }
    curl_mime_free(mime);
    curl_easy_cleanup(curl);
    return status;
#endif
}

#ifdef SC_HAVE_LIBCURL
static size_t whisper_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    whisper_curl_buffer *response = userdata;
    size_t bytes = size * nmemb;
    if (response == nullptr || ptr == nullptr) {
        return 0;
    }
    if (response->builder.bytes.len + bytes > response->max_bytes) {
        response->too_large = true;
        return 0;
    }
    if (!sc_status_is_ok(sc_string_builder_append(&response->builder, sc_str_from_parts(ptr, bytes)))) {
        return 0;
    }
    return bytes;
}

static sc_status curl_add_mime_text(curl_mime *mime, const char *name, sc_str value)
{
    curl_mimepart *part = curl_mime_addpart(mime);
    if (part == nullptr || name == nullptr || value.ptr == nullptr) {
        return sc_status_no_memory();
    }
    if (curl_mime_name(part, name) != CURLE_OK ||
        curl_mime_data(part, value.ptr, value.len) != CURLE_OK) {
        return sc_status_http("sc.transcriber.whisper.mime_failed");
    }
    return sc_status_ok();
}
#endif
