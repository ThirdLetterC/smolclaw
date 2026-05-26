// cppcheck-suppress-file redundantInitialization
#include "sc/media.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sc/api.h"

#include "net/curl_global.h"

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

typedef struct piper_tts {
    sc_allocator *alloc;
    sc_string base_url;
    sc_string default_voice;
    sc_string speaker;
    int64_t speaker_id;
    bool speaker_id_set;
    double length_scale;
    bool length_scale_set;
    double noise_scale;
    bool noise_scale_set;
    double noise_w_scale;
    bool noise_w_scale_set;
    uint32_t timeout_ms;
    size_t max_audio_bytes;
    sc_tts_piper_http_fn http_request;
    void *http_user;
} piper_tts;

#ifdef SC_HAVE_LIBCURL
typedef struct piper_curl_buffer {
    sc_bytes bytes;
    size_t max_bytes;
    bool too_large;
} piper_curl_buffer;
#endif

static sc_status piper_synthesize(void *impl,
                                  const sc_tts_request *request,
                                  sc_allocator *alloc,
                                  sc_tts_result *out);
static void piper_destroy(void *impl);
static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status build_piper_request_json(const piper_tts *piper,
                                          const sc_tts_request *request,
                                          sc_allocator *alloc,
                                          sc_string *out);
static sc_status append_json_string_field(sc_string_builder *builder, const char *name, sc_str value, bool *first);
static sc_status append_json_number_field(sc_string_builder *builder, const char *name, double value, bool *first);
static sc_status append_json_int_field(sc_string_builder *builder, const char *name, int64_t value, bool *first);
static sc_status append_json_escaped(sc_string_builder *builder, sc_str value);
static sc_status append_separator(sc_string_builder *builder, bool *first);
static bool piper_base_url_allowed(sc_str url);
static bool str_has_prefix(sc_str value, const char *prefix);
static bool loopback_host_allowed(sc_str host);
static sc_status piper_default_http(void *user,
                                    sc_str method,
                                    sc_str url,
                                    sc_str body,
                                    uint32_t timeout_ms,
                                    size_t max_bytes,
                                    sc_allocator *alloc,
                                    sc_bytes *out);

#ifdef SC_HAVE_LIBCURL
static size_t piper_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
#endif

static const sc_tts_vtab piper_vtab = {
    .struct_size = sizeof(sc_tts_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "piper",
    .display_name = "Piper TTS",
    .feature_flag = "SC_TTS_PIPER",
    .capabilities = SC_CONTRACT_CAP_BINARY,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .synthesize = piper_synthesize,
    .destroy = piper_destroy,
};

sc_status sc_tts_piper_new(sc_allocator *alloc, const sc_tts_piper_options *options, sc_tts **out)
{
    piper_tts *impl = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || options->base_url.ptr == nullptr || options->base_url.len == 0) {
        return sc_status_invalid_argument("sc.tts.piper.invalid_argument");
    }
    if (!piper_base_url_allowed(options->base_url)) {
        return sc_status_security_denied("sc.tts.piper.endpoint_denied");
    }

    *out = nullptr;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(piper_tts));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (piper_tts){
        .alloc = alloc,
        .speaker_id = options->speaker_id,
        .speaker_id_set = options->speaker_id_set,
        .length_scale = options->length_scale,
        .length_scale_set = options->length_scale_set,
        .noise_scale = options->noise_scale,
        .noise_scale_set = options->noise_scale_set,
        .noise_w_scale = options->noise_w_scale,
        .noise_w_scale_set = options->noise_w_scale_set,
        .timeout_ms = options->timeout_ms == 0 ? 30'000 : options->timeout_ms,
        .max_audio_bytes = options->max_audio_bytes == 0 ? 8U * 1024U * 1024U : options->max_audio_bytes,
        .http_request = options->http_request == nullptr ? piper_default_http : options->http_request,
        .http_user = options->http_user,
    };
    status = copy_string(alloc, options->base_url, &impl->base_url);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->default_voice, &impl->default_voice);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->speaker, &impl->speaker);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tts_new(alloc, &piper_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        piper_destroy(impl);
    }
    return status;
}

static sc_status piper_synthesize(void *impl,
                                  const sc_tts_request *request,
                                  sc_allocator *alloc,
                                  sc_tts_result *out)
{
    piper_tts *piper = impl;
    sc_string body = {0};
    sc_bytes audio = {0};
    sc_status status = sc_status_ok();

    if (piper == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.tts.piper.invalid_argument");
    }
    if (request->text.ptr == nullptr || request->text.len == 0) {
        return sc_status_invalid_argument("sc.tts.piper.text_missing");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.tts.piper.cancelled");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_tts_result){.struct_size = sizeof(*out)};

    status = build_piper_request_json(piper, request, alloc, &body);
    if (sc_status_is_ok(status)) {
        uint32_t timeout_ms = request->timeout_ms == 0 ? piper->timeout_ms : request->timeout_ms;
        status = piper->http_request(piper->http_user,
                                     sc_str_from_cstr("POST"),
                                     sc_string_as_str(&piper->base_url),
                                     sc_string_as_str(&body),
                                     timeout_ms,
                                     piper->max_audio_bytes,
                                     alloc,
                                     &audio);
    }
    if (sc_status_is_ok(status) && !sc_media_audio_wav_is_well_formed(sc_buf_from_parts(audio.ptr, audio.len))) {
        status = sc_status_parse("sc.tts.piper.malformed_wav");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "audio/wav", &out->content_type);
    }
    if (sc_status_is_ok(status)) {
        out->audio = audio;
        audio = (sc_bytes){0};
    }
    if (!sc_status_is_ok(status)) {
        sc_tts_result_clear(out);
    }
    sc_bytes_clear(&audio);
    sc_string_clear(&body);
    return status;
}

static void piper_destroy(void *impl)
{
    piper_tts *piper = impl;
    if (piper == nullptr) {
        return;
    }
    sc_string_clear(&piper->base_url);
    sc_string_clear(&piper->default_voice);
    sc_string_clear(&piper->speaker);
    sc_free(piper->alloc, piper, sizeof(*piper), _Alignof(piper_tts));
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out)
{
    return sc_string_from_str(alloc, input.ptr == nullptr ? sc_str_from_cstr("") : input, out);
}

static sc_status build_piper_request_json(const piper_tts *piper,
                                          const sc_tts_request *request,
                                          sc_allocator *alloc,
                                          sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    bool first = true;
    sc_str voice = request->voice.len > 0 ? request->voice : sc_string_as_str(&piper->default_voice);

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{");
    if (sc_status_is_ok(status)) {
        status = append_json_string_field(&builder, "text", request->text, &first);
    }
    if (sc_status_is_ok(status) && voice.len > 0) {
        status = append_json_string_field(&builder, "voice", voice, &first);
    }
    if (sc_status_is_ok(status) && piper->speaker.len > 0) {
        status = append_json_string_field(&builder, "speaker", sc_string_as_str(&piper->speaker), &first);
    }
    if (sc_status_is_ok(status) && piper->speaker_id_set) {
        status = append_json_int_field(&builder, "speaker_id", piper->speaker_id, &first);
    }
    if (sc_status_is_ok(status) && piper->length_scale_set) {
        status = append_json_number_field(&builder, "length_scale", piper->length_scale, &first);
    }
    if (sc_status_is_ok(status) && piper->noise_scale_set) {
        status = append_json_number_field(&builder, "noise_scale", piper->noise_scale, &first);
    }
    if (sc_status_is_ok(status) && piper->noise_w_scale_set) {
        status = append_json_number_field(&builder, "noise_w_scale", piper->noise_w_scale, &first);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_json_string_field(sc_string_builder *builder, const char *name, sc_str value, bool *first)
{
    sc_status status = append_separator(builder, first);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\":\"");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_escaped(builder, value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

static sc_status append_json_number_field(sc_string_builder *builder, const char *name, double value, bool *first)
{
    char buffer[64] = {0};
    int written = snprintf(buffer, sizeof(buffer), "%.17g", value);
    sc_status status = sc_status_ok();
    if (written <= 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_invalid_argument("sc.tts.piper.number_invalid");
    }
    status = append_separator(builder, first);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, buffer);
    }
    return status;
}

static sc_status append_json_int_field(sc_string_builder *builder, const char *name, int64_t value, bool *first)
{
    char buffer[32] = {0};
    int written = snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    sc_status status = sc_status_ok();
    if (written <= 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_invalid_argument("sc.tts.piper.integer_invalid");
    }
    status = append_separator(builder, first);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, buffer);
    }
    return status;
}

static sc_status append_json_escaped(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_status_ok();
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        unsigned char ch = (unsigned char)value.ptr[i];
        switch (ch) {
        case '\"':
            status = sc_string_builder_append_cstr(builder, "\\\"");
            break;
        case '\\':
            status = sc_string_builder_append_cstr(builder, "\\\\");
            break;
        case '\b':
            status = sc_string_builder_append_cstr(builder, "\\b");
            break;
        case '\f':
            status = sc_string_builder_append_cstr(builder, "\\f");
            break;
        case '\n':
            status = sc_string_builder_append_cstr(builder, "\\n");
            break;
        case '\r':
            status = sc_string_builder_append_cstr(builder, "\\r");
            break;
        case '\t':
            status = sc_string_builder_append_cstr(builder, "\\t");
            break;
        default:
            if (ch < 0x20u) {
                char escape[7] = {0};
                int written = snprintf(escape, sizeof(escape), "\\u%04x", (unsigned)ch);
                if (written != 6) {
                    status = sc_status_io("sc.tts.piper.escape_failed");
                } else {
                    status = sc_string_builder_append_cstr(builder, escape);
                }
            } else {
                status = sc_string_builder_append(builder, sc_str_from_parts((const char *)&value.ptr[i], 1));
            }
            break;
        }
    }
    return status;
}

static sc_status append_separator(sc_string_builder *builder, bool *first)
{
    if (first == nullptr) {
        return sc_status_invalid_argument("sc.tts.piper.builder_invalid");
    }
    if (*first) {
        *first = false;
        return sc_status_ok();
    }
    return sc_string_builder_append_cstr(builder, ",");
}

static bool piper_base_url_allowed(sc_str url)
{
    sc_str rest = {0};
    size_t host_len = 0;
    size_t scheme_len = 0;
    if (str_has_prefix(url, "https://")) {
        scheme_len = strlen("https://");
    } else if (str_has_prefix(url, "http://")) {
        scheme_len = strlen("http://");
    } else {
        return false;
    }
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

static sc_status piper_default_http(void *user,
                                    sc_str method,
                                    sc_str url,
                                    sc_str body,
                                    uint32_t timeout_ms,
                                    size_t max_bytes,
                                    sc_allocator *alloc,
                                    sc_bytes *out)
{
    (void)user;
    (void)method;
    (void)url;
    (void)body;
    (void)timeout_ms;
    (void)max_bytes;
    (void)alloc;
    (void)out;
#ifndef SC_HAVE_LIBCURL
    return sc_status_unsupported("sc.tts.piper.libcurl_unavailable");
#else
    CURL *curl = nullptr;
    CURLcode code;
    struct curl_slist *headers = nullptr;
    piper_curl_buffer response = {0};
    long response_code = 0;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.tts.piper.http_invalid_argument");
    }
    status = sc_curl_global_init("sc.tts.piper.curl_init_failed");
    if (!sc_status_is_ok(status)) {
        return sc_status_http("sc.tts.piper.curl_init_failed");
    }
    curl = curl_easy_init();
    if (curl == nullptr) {
        return sc_status_http("sc.tts.piper.curl_init_failed");
    }
    sc_bytes_init(&response.bytes, alloc);
    response.max_bytes = max_bytes == 0 ? 8U * 1024U * 1024U : max_bytes;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: audio/wav, application/octet-stream");
    if (headers == nullptr) {
        status = sc_status_no_memory();
    }
    if (sc_status_is_ok(status)) {
        (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.ptr == nullptr ? "" : body.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.len);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, piper_curl_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 piper-tts");
        (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 10'000 ? (long)timeout_ms : 10'000L);
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms == 0 ? 30'000L : (long)timeout_ms);
        (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        code = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response.too_large) {
            status = sc_status_security_denied("sc.tts.piper.audio_too_large");
        } else if (code == CURLE_OPERATION_TIMEDOUT) {
            status = sc_status_timeout("sc.tts.piper.timeout");
        } else if (code != CURLE_OK) {
            status = sc_status_http("sc.tts.piper.http_failed");
        } else if (response_code < 200 || response_code >= 300) {
            status = sc_status_http("sc.tts.piper.http_status_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        *out = response.bytes;
        response.bytes = (sc_bytes){0};
    }
    sc_bytes_clear(&response.bytes);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return status;
#endif
}

#ifdef SC_HAVE_LIBCURL
static size_t piper_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    piper_curl_buffer *response = userdata;
    size_t bytes = size * nmemb;
    if (response == nullptr || ptr == nullptr) {
        return 0;
    }
    if (response->bytes.len + bytes > response->max_bytes) {
        response->too_large = true;
        return 0;
    }
    if (!sc_status_is_ok(sc_bytes_append(&response->bytes, sc_buf_from_parts(ptr, bytes)))) {
        return 0;
    }
    return bytes;
}
#endif
