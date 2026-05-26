#include "sc/media.h"
#include "sc/runtime.h"
#include "sc/version.h"
#include "test_helpers.h"

#include "media/media_encoder_internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef struct piper_stub {
    sc_string method;
    sc_string url;
    sc_string body;
    uint32_t timeout_ms;
    size_t max_bytes;
    sc_buf audio;
    sc_status status;
} piper_stub;

typedef struct whisper_stub {
    sc_transcriber_whisper_http_request request;
    sc_string method;
    sc_string url;
    sc_string model;
    sc_string language;
    sc_string prompt;
    sc_string temperature;
    sc_string temperature_inc;
    sc_string response_format;
    sc_string response_body;
    sc_status status;
} whisper_stub;

static sc_media_attachment input_attachment(const char *id,
                                            const char *content_type,
                                            const char *filename,
                                            const unsigned char *bytes,
                                            size_t len);
static sc_media_limits limits(size_t max_bytes);
static int test_attachment_validation_and_cleanup(void);
static int test_audio_bounds_cancellation_and_vad(void);
static int test_transcription_tts_and_turn_attachment(void);
static int test_piper_tts(void);
static int test_whisper_transcriber(void);
static int test_wav_to_ogg_opus(void);
static bool bytes_contain(sc_buf haystack, sc_buf needle);
static sc_status piper_stub_http(void *user,
                                 sc_str method,
                                 sc_str url,
                                 sc_str body,
                                 uint32_t timeout_ms,
                                 size_t max_bytes,
                                 sc_allocator *alloc,
                                 sc_bytes *out);
static void piper_stub_clear(piper_stub *stub);
static sc_status whisper_stub_http(void *user,
                                   const sc_transcriber_whisper_http_request *request,
                                   sc_allocator *alloc,
                                   sc_string *out);
static void whisper_stub_clear(whisper_stub *stub);

int main(void)
{
    int failures = 0;

    failures += test_attachment_validation_and_cleanup();
    failures += test_audio_bounds_cancellation_and_vad();
    failures += test_transcription_tts_and_turn_attachment();
    failures += test_piper_tts();
    failures += test_whisper_transcriber();
    failures += test_wav_to_ogg_opus();

    return failures == 0 ? 0 : 1;
}

static int test_wav_to_ogg_opus(void)
{
    int failures = 0;
    const unsigned char wav[] = {
        'R', 'I', 'F', 'F', 68, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        1, 0, 1, 0,
        0x40, 0x1F, 0, 0,
        0x80, 0x3E, 0, 0,
        2, 0, 16, 0,
        'd', 'a', 't', 'a', 32, 0, 0, 0,
        0, 0, 0x00, 0x10, 0x00, 0x20, 0x00, 0x10,
        0, 0, 0x00, 0xF0, 0x00, 0xE0, 0x00, 0xF0,
        0, 0, 0x00, 0x10, 0x00, 0x20, 0x00, 0x10,
        0, 0, 0x00, 0xF0, 0x00, 0xE0, 0x00, 0xF0,
    };
    const unsigned char malformed[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    const unsigned char unsupported[] = {
        'R', 'I', 'F', 'F', 48, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0,
        3, 0, 1, 0,
        0x40, 0x1F, 0, 0,
        0x00, 0x7D, 0, 0,
        4, 0, 32, 0,
        'd', 'a', 't', 'a', 12, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    sc_bytes ogg = {0};
    sc_status status = sc_media_wav_to_ogg_opus(sc_allocator_heap(), sc_buf_from_parts(wav, sizeof(wav)), 32'768, &ogg);

    if (status.code == SC_ERR_UNSUPPORTED) {
        failures += sc_test_expect_true("opus unavailable key", strcmp(status.error_key, "sc.media.opus.unavailable") == 0);
        sc_status_clear(&status);
        return failures;
    }

    failures += sc_test_expect_status("wav to ogg opus", status, SC_OK);
    failures += sc_test_expect_true("ogg magic", ogg.len > 4 && memcmp(ogg.ptr, "OggS", 4) == 0);
    failures += sc_test_expect_true("opus head", bytes_contain(sc_buf_from_parts(ogg.ptr, ogg.len), sc_buf_from_parts("OpusHead", 8)));
    failures += sc_test_expect_true("opus tags", bytes_contain(sc_buf_from_parts(ogg.ptr, ogg.len), sc_buf_from_parts("OpusTags", 8)));
    sc_bytes_clear(&ogg);

    failures += sc_test_expect_status("wav malformed",
                              sc_media_wav_to_ogg_opus(sc_allocator_heap(), sc_buf_from_parts(malformed, sizeof(malformed)), 32'768, &ogg),
                              SC_ERR_PARSE);
    sc_bytes_clear(&ogg);
    failures += sc_test_expect_status("wav unsupported",
                              sc_media_wav_to_ogg_opus(sc_allocator_heap(), sc_buf_from_parts(unsupported, sizeof(unsupported)), 32'768, &ogg),
                              SC_ERR_UNSUPPORTED);
    sc_bytes_clear(&ogg);
    failures += sc_test_expect_status("opus output cap",
                              sc_media_wav_to_ogg_opus(sc_allocator_heap(), sc_buf_from_parts(wav, sizeof(wav)), 32, &ogg),
                              SC_ERR_SECURITY_DENIED);
    sc_bytes_clear(&ogg);
    return failures;
}

static bool bytes_contain(sc_buf haystack, sc_buf needle)
{
    if (needle.len == 0) {
        return true;
    }
    if (haystack.ptr == nullptr || needle.ptr == nullptr || haystack.len < needle.len) {
        return false;
    }
    for (size_t i = 0; i + needle.len <= haystack.len; i += 1) {
        if (memcmp(haystack.ptr + i, needle.ptr, needle.len) == 0) {
            return true;
        }
    }
    return false;
}

static int test_whisper_transcriber(void)
{
    int failures = 0;
    const unsigned char audio[] = "audio bytes";
    sc_media_attachment attachment = input_attachment("whisper-audio", "audio/ogg", "voice.ogg", audio, sizeof(audio) - 1);
    whisper_stub stub = {.status = sc_status_ok()};
    sc_transcriber *transcriber = nullptr;
    sc_transcriber *denied = nullptr;
    sc_transcriber *srt = nullptr;
    sc_transcription_result result = {0};

    failures += sc_test_expect_status("whisper deny remote",
                              sc_transcriber_whisper_new(sc_allocator_heap(),
                                                         &(sc_transcriber_whisper_options){
                                                             .struct_size = sizeof(sc_transcriber_whisper_options),
                                                             .endpoint_url = sc_str_from_cstr("http://example.com:2022/v1/audio/transcriptions"),
                                                             .http_request = whisper_stub_http,
                                                         },
                                                         &denied),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("whisper reject subtitle",
                              sc_transcriber_whisper_new(sc_allocator_heap(),
                                                         &(sc_transcriber_whisper_options){
                                                             .struct_size = sizeof(sc_transcriber_whisper_options),
                                                             .endpoint_url = sc_str_from_cstr("http://127.0.0.1:2022/v1/audio/transcriptions"),
                                                             .response_format = sc_str_from_cstr("srt"),
                                                             .http_request = whisper_stub_http,
                                                         },
                                                         &srt),
                              SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_status("whisper response body",
                              sc_string_from_cstr(sc_allocator_heap(), "{\"text\":\"transcribed words\"}", &stub.response_body),
                              SC_OK);
    failures += sc_test_expect_status("whisper new",
                              sc_transcriber_whisper_new(sc_allocator_heap(),
                                                         &(sc_transcriber_whisper_options){
                                                             .struct_size = sizeof(sc_transcriber_whisper_options),
                                                             .endpoint_url = sc_str_from_cstr("http://localhost:2022/v1/audio/transcriptions"),
                                                             .model = sc_str_from_cstr("whisper-1"),
                                                             .default_language = sc_str_from_cstr("en"),
                                                             .prompt = sc_str_from_cstr("technical terms"),
                                                             .temperature = sc_str_from_cstr("0.0"),
                                                             .temperature_inc = sc_str_from_cstr("0.2"),
                                                             .response_format = sc_str_from_cstr("json"),
                                                             .timeout_ms = 2345,
                                                             .max_audio_bytes = 64,
                                                             .max_response_bytes = 128,
                                                             .http_request = whisper_stub_http,
                                                             .http_user = &stub,
                                                         },
                                                         &transcriber),
                              SC_OK);
    failures += sc_test_expect_status("whisper transcribe",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                            .language = sc_str_from_cstr("uk"),
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_OK);
    failures += sc_test_expect_true("whisper text", strcmp(result.text.ptr, "transcribed words") == 0);
    failures += sc_test_expect_true("whisper method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("whisper url", strcmp(stub.url.ptr, "http://localhost:2022/v1/audio/transcriptions") == 0);
    failures += sc_test_expect_true("whisper fields", strcmp(stub.model.ptr, "whisper-1") == 0 &&
                                                strcmp(stub.language.ptr, "uk") == 0 &&
                                                strcmp(stub.prompt.ptr, "technical terms") == 0 &&
                                                strcmp(stub.temperature.ptr, "0.0") == 0 &&
                                                strcmp(stub.temperature_inc.ptr, "0.2") == 0 &&
                                                strcmp(stub.response_format.ptr, "json") == 0);
    failures += sc_test_expect_true("whisper bounds", stub.request.timeout_ms == 2345 &&
                                               stub.request.max_audio_bytes == 64 &&
                                               stub.request.max_response_bytes == 128);
    sc_transcription_result_clear(&result);
    failures += sc_test_expect_status("whisper cancelled",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                            .cancel_requested = true,
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_ERR_CANCELLED);
    stub.status = sc_status_timeout("sc.test.whisper.timeout");
    failures += sc_test_expect_status("whisper timeout",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_ERR_TIMEOUT);
    attachment.size_bytes = 65;
    failures += sc_test_expect_status("whisper upload too large",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_ERR_SECURITY_DENIED);
    attachment.size_bytes = sizeof(audio) - 1;
    sc_transcription_result_clear(&result);
    sc_string_clear(&stub.response_body);
    // cppcheck-suppress redundantAssignment
    stub.status = sc_status_ok();
    failures += sc_test_expect_status("whisper text response body",
                              sc_string_from_cstr(sc_allocator_heap(), "plain transcript", &stub.response_body),
                              SC_OK);
    sc_transcriber_destroy(transcriber);
    transcriber = nullptr;
    failures += sc_test_expect_status("whisper text new",
                              sc_transcriber_whisper_new(sc_allocator_heap(),
                                                         &(sc_transcriber_whisper_options){
                                                             .struct_size = sizeof(sc_transcriber_whisper_options),
                                                             .endpoint_url = sc_str_from_cstr("http://127.0.0.1:2022/v1/audio/transcriptions"),
                                                             .response_format = sc_str_from_cstr("text"),
                                                             .http_request = whisper_stub_http,
                                                             .http_user = &stub,
                                                         },
                                                         &transcriber),
                              SC_OK);
    failures += sc_test_expect_status("whisper text transcribe",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_OK);
    failures += sc_test_expect_true("whisper raw text", strcmp(result.text.ptr, "plain transcript") == 0);
    sc_transcription_result_clear(&result);
    sc_string_clear(&stub.response_body);
    sc_transcriber_destroy(transcriber);
    transcriber = nullptr;
    failures += sc_test_expect_status("whisper json new",
                              sc_transcriber_whisper_new(sc_allocator_heap(),
                                                         &(sc_transcriber_whisper_options){
                                                             .struct_size = sizeof(sc_transcriber_whisper_options),
                                                             .endpoint_url = sc_str_from_cstr("http://127.0.0.1:2022/v1/audio/transcriptions"),
                                                             .response_format = sc_str_from_cstr("json"),
                                                             .http_request = whisper_stub_http,
                                                             .http_user = &stub,
                                                         },
                                                         &transcriber),
                              SC_OK);
    failures += sc_test_expect_status("whisper malformed body",
                              sc_string_from_cstr(sc_allocator_heap(), "{\"missing\":\"text\"}", &stub.response_body),
                              SC_OK);
    failures += sc_test_expect_status("whisper missing text",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_ERR_PARSE);
    sc_string_clear(&stub.response_body);
    failures += sc_test_expect_status("whisper malformed response",
                              sc_string_from_cstr(sc_allocator_heap(), "{", &stub.response_body),
                              SC_OK);
    failures += sc_test_expect_status("whisper malformed json",
                              sc_transcriber_transcribe(transcriber,
                                                        &(sc_transcription_request){
                                                            .struct_size = sizeof(sc_transcription_request),
                                                            .attachment = &attachment,
                                                        },
                                                        sc_allocator_heap(),
                                                        &result),
                              SC_ERR_PARSE);

    sc_transcription_result_clear(&result);
    sc_transcriber_destroy(transcriber);
    whisper_stub_clear(&stub);
    return failures;
}

static int test_piper_tts(void)
{
    int failures = 0;
    const unsigned char wav[] = {'R', 'I', 'F', 'F', 16, 0, 0, 0, 'W', 'A', 'V', 'E', 3, 0, 0, 0};
    const unsigned char bad[] = "not wav";
    piper_stub stub = {.audio = sc_buf_from_parts(wav, sizeof(wav)), .status = sc_status_ok()};
    sc_tts *tts = nullptr;
    sc_tts *denied = nullptr;
    sc_tts_result result = {0};

    failures += sc_test_expect_status("piper denies remote http",
                              sc_tts_piper_new(sc_allocator_heap(),
                                               &(sc_tts_piper_options){
                                                   .struct_size = sizeof(sc_tts_piper_options),
                                                   .base_url = sc_str_from_cstr("http://example.com:5000"),
                                                   .http_request = piper_stub_http,
                                               },
                                               &denied),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("piper new",
                              sc_tts_piper_new(sc_allocator_heap(),
                                               &(sc_tts_piper_options){
                                                   .struct_size = sizeof(sc_tts_piper_options),
                                                   .base_url = sc_str_from_cstr("http://127.0.0.1:5000"),
                                                   .default_voice = sc_str_from_cstr("default-voice"),
                                                   .speaker = sc_str_from_cstr("alice"),
                                                   .speaker_id = 2,
                                                   .speaker_id_set = true,
                                                   .length_scale = 1.25,
                                                   .length_scale_set = true,
                                                   .noise_scale = 0.5,
                                                   .noise_scale_set = true,
                                                   .noise_w_scale = 0.75,
                                                   .noise_w_scale_set = true,
                                                   .timeout_ms = 1234,
                                                   .max_audio_bytes = sizeof(wav),
                                                   .http_request = piper_stub_http,
                                                   .http_user = &stub,
                                               },
                                               &tts),
                              SC_OK);
    failures += sc_test_expect_status("piper synth",
                              sc_tts_synthesize(tts,
                                                &(sc_tts_request){
                                                    .struct_size = sizeof(sc_tts_request),
                                                    .text = sc_str_from_cstr("hello \"audio\"\nreply"),
                                                    .voice = sc_str_from_cstr("override-voice"),
                                                },
                                                sc_allocator_heap(),
                                                &result),
                              SC_OK);
    failures += sc_test_expect_true("piper audio", result.audio.len == sizeof(wav) && strcmp(result.content_type.ptr, "audio/wav") == 0);
    failures += sc_test_expect_true("piper method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("piper url", strcmp(stub.url.ptr, "http://127.0.0.1:5000") == 0);
    failures += sc_test_expect_true("piper timeout", stub.timeout_ms == 1234 && stub.max_bytes == sizeof(wav));
    failures += sc_test_expect_true("piper text escaped", strstr(stub.body.ptr, "hello \\\"audio\\\" reply") != nullptr);
    failures += sc_test_expect_true("piper voice override", strstr(stub.body.ptr, "\"voice\":\"override-voice\"") != nullptr);
    failures += sc_test_expect_true("piper speaker", strstr(stub.body.ptr, "\"speaker\":\"alice\"") != nullptr);
    failures += sc_test_expect_true("piper speaker id", strstr(stub.body.ptr, "\"speaker_id\":2") != nullptr);
    failures += sc_test_expect_true("piper scales", strstr(stub.body.ptr, "\"length_scale\":1.25") != nullptr &&
                                          strstr(stub.body.ptr, "\"noise_scale\":0.5") != nullptr &&
                                          strstr(stub.body.ptr, "\"noise_w_scale\":0.75") != nullptr);
    sc_tts_result_clear(&result);
    failures += sc_test_expect_status("piper markdown stripped",
                              sc_tts_synthesize(tts,
                                                &(sc_tts_request){
                                                    .struct_size = sizeof(sc_tts_request),
                                                    .text = sc_str_from_cstr("## Hello **audio** [docs](https://example.test) `code` _now_!\n- item"),
                                                },
                                                sc_allocator_heap(),
                                                &result),
                              SC_OK);
    failures += sc_test_expect_true("piper markdown-free text", strstr(stub.body.ptr, "\"text\":\"Hello audio docs code now item\"") != nullptr);
    sc_tts_result_clear(&result);
    stub.audio = sc_buf_from_parts(bad, sizeof(bad) - 1);
    failures += sc_test_expect_status("piper malformed",
                              sc_tts_synthesize(tts,
                                                &(sc_tts_request){
                                                    .struct_size = sizeof(sc_tts_request),
                                                    .text = sc_str_from_cstr("bad"),
                                                },
                                                sc_allocator_heap(),
                                                &result),
                              SC_ERR_PARSE);
    failures += sc_test_expect_status("piper cancelled",
                              sc_tts_synthesize(tts,
                                                &(sc_tts_request){
                                                    .struct_size = sizeof(sc_tts_request),
                                                    .text = sc_str_from_cstr("cancel"),
                                                    .cancel_requested = true,
                                                },
                                                sc_allocator_heap(),
                                                &result),
                              SC_ERR_CANCELLED);

    sc_tts_result_clear(&result);
    sc_tts_destroy(tts);
    piper_stub_clear(&stub);
    return failures;
}

static sc_status piper_stub_http(void *user,
                                 sc_str method,
                                 sc_str url,
                                 sc_str body,
                                 uint32_t timeout_ms,
                                 size_t max_bytes,
                                 sc_allocator *alloc,
                                 sc_bytes *out)
{
    piper_stub *stub = user;
    sc_status status;
    if (stub == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test.piper_stub.invalid_argument");
    }
    sc_string_clear(&stub->method);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->body);
    stub->timeout_ms = timeout_ms;
    stub->max_bytes = max_bytes;
    status = sc_string_from_str(alloc, method, &stub->method);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, url, &stub->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, body, &stub->body);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (!sc_status_is_ok(stub->status)) {
        return stub->status;
    }
    if (stub->audio.len > max_bytes) {
        return sc_status_security_denied("sc.test.piper_stub.too_large");
    }
    return sc_bytes_from_buf(alloc, stub->audio, out);
}

static void piper_stub_clear(piper_stub *stub)
{
    if (stub == nullptr) {
        return;
    }
    sc_string_clear(&stub->method);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->body);
}

static sc_status whisper_stub_http(void *user,
                                   const sc_transcriber_whisper_http_request *request,
                                   sc_allocator *alloc,
                                   sc_string *out)
{
    whisper_stub *stub = user;
    sc_status status;
    if (stub == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test.whisper_stub.invalid_argument");
    }
    sc_string_clear(&stub->method);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->model);
    sc_string_clear(&stub->language);
    sc_string_clear(&stub->prompt);
    sc_string_clear(&stub->temperature);
    sc_string_clear(&stub->temperature_inc);
    sc_string_clear(&stub->response_format);
    stub->request = *request;
    status = sc_string_from_str(alloc, request->method, &stub->method);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->url, &stub->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->model, &stub->model);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->language, &stub->language);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->prompt, &stub->prompt);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->temperature, &stub->temperature);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->temperature_inc, &stub->temperature_inc);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, request->response_format, &stub->response_format);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (!sc_status_is_ok(stub->status)) {
        return stub->status;
    }
    if (request->attachment == nullptr || request->attachment->size_bytes > request->max_audio_bytes) {
        return sc_status_security_denied("sc.test.whisper_stub.audio_too_large");
    }
    if (stub->response_body.len > request->max_response_bytes) {
        return sc_status_security_denied("sc.test.whisper_stub.response_too_large");
    }
    return sc_string_from_str(alloc, sc_string_as_str(&stub->response_body), out);
}

static void whisper_stub_clear(whisper_stub *stub)
{
    if (stub == nullptr) {
        return;
    }
    sc_string_clear(&stub->method);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->model);
    sc_string_clear(&stub->language);
    sc_string_clear(&stub->prompt);
    sc_string_clear(&stub->temperature);
    sc_string_clear(&stub->temperature_inc);
    sc_string_clear(&stub->response_format);
    sc_string_clear(&stub->response_body);
}

static sc_media_attachment input_attachment(const char *id,
                                            const char *content_type,
                                            const char *filename,
                                            const unsigned char *bytes,
                                            size_t len)
{
    return (sc_media_attachment){
        .struct_size = sizeof(sc_media_attachment),
        .attachment_id = sc_str_from_cstr(id),
        .content_type = sc_str_from_cstr(content_type),
        .filename = sc_str_from_cstr(filename),
        .size_bytes = len,
        .storage_kind = SC_MEDIA_STORAGE_BYTES,
        .bytes = sc_buf_from_parts(bytes, len),
    };
}

static sc_media_limits limits(size_t max_bytes)
{
    return (sc_media_limits){
        .struct_size = sizeof(sc_media_limits),
        .max_bytes = max_bytes,
        .timeout_ms = 1000,
    };
}

static int test_attachment_validation_and_cleanup(void)
{
    int failures = 0;
    const unsigned char text[] = "hello media";
    sc_media_attachment attachment = {0};
    sc_media_attachment unsafe = input_attachment("bad/type", "text/plain", "unsafe.txt", text, sizeof(text) - 1);
    sc_media_attachment unsupported = input_attachment("bad-content", "application/x-bad", "bad.bin", text, 1);
    sc_media_attachment too_large = input_attachment("too-large", "text/plain", "large.txt", text, sizeof(text));
    sc_media_attachment input = {0};
    sc_media_limits standard_limits = limits(64);
    sc_media_limits small_limits = limits(2);
    sc_media_limits type_limits = limits(32);
    sc_log_field field = {0};
    sc_str redacted = {0};
    char id[64] = {0};
    int written = snprintf(id, sizeof(id), "media-%ld", (long)getpid());

    failures += sc_test_expect_true("id formatted", written > 0 && (size_t)written < sizeof(id));
    failures += sc_test_expect_status("unsupported type",
                              sc_media_attachment_validate(&unsupported, &type_limits),
                              SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_status("too large",
                              sc_media_attachment_validate(&too_large, &small_limits),
                              SC_ERR_SECURITY_DENIED);
    input = input_attachment(id, "text/plain", "note.txt", text, sizeof(text) - 1);
    failures += sc_test_expect_status("from bytes",
                              sc_media_attachment_from_bytes(sc_allocator_heap(),
                                                             &input,
                                                             &standard_limits,
                                                             &attachment),
                              SC_OK);
    failures += sc_test_expect_true("hash created", attachment.hash.len == 16);
    failures += sc_test_expect_status("unsafe id rejected", sc_media_attachment_store_temp(&unsafe, sc_str_from_cstr("/tmp")), SC_ERR_SECURITY_DENIED);
    attachment.safety_flags = SC_MEDIA_SAFETY_PERSONAL_DATA;
    sc_media_attachment_filename_log_field(&attachment, &field);
    redacted = sc_log_redact_field(&field);
    failures += sc_test_expect_true("filename redacted", sc_str_equal(redacted, sc_str_from_cstr("[REDACTED]")));
    failures += sc_test_expect_status("store temp", sc_media_attachment_store_temp(&attachment, sc_str_from_cstr("/tmp")), SC_OK);
    failures += sc_test_expect_true("temp exists", attachment.storage_path.ptr != nullptr && access(attachment.storage_path.ptr, F_OK) == 0);
    failures += sc_test_expect_status("cleanup temp", sc_media_attachment_cleanup(&attachment), SC_OK);
    failures += sc_test_expect_true("temp removed", attachment.storage_path.ptr != nullptr && access(attachment.storage_path.ptr, F_OK) != 0);

    sc_media_attachment_clear(&attachment);
    return failures;
}

static int test_audio_bounds_cancellation_and_vad(void)
{
    int failures = 0;
    const unsigned char malformed[] = "not wave";
    const unsigned char silence[] = {'R', 'I', 'F', 'F', 16, 0, 0, 0, 'W', 'A', 'V', 'E', 0, 0, 0, 0};
    sc_media_attachment cancel_input = input_attachment("cancelled", "text/plain", "cancel.txt", malformed, 1);
    sc_media_attachment malformed_input = input_attachment("audio-bad", "audio/wav", "bad.wav", malformed, sizeof(malformed) - 1);
    sc_media_attachment silent_input = input_attachment("audio-silent", "audio/wav", "silent.wav", silence, sizeof(silence));
    sc_media_attachment malformed_audio = {0};
    sc_media_attachment silent_audio = {0};
    sc_media_pipeline_result result = {0};
    sc_media_limits standard_limits = limits(64);
    sc_media_limits cancelled_limits = limits(64);
    sc_media_pipeline_options vad_options = {0};

    cancelled_limits.cancel_requested = true;
    failures += sc_test_expect_status("cancelled validate",
                              sc_media_attachment_validate(&cancel_input, &cancelled_limits),
                              SC_ERR_CANCELLED);
    failures += sc_test_expect_status("malformed from bytes",
                              sc_media_attachment_from_bytes(sc_allocator_heap(),
                                                             &malformed_input,
                                                             &standard_limits,
                                                             &malformed_audio),
                              SC_OK);
    failures += sc_test_expect_status("malformed pipeline",
                              sc_media_pipeline_run(sc_allocator_heap(),
                                                    &(sc_media_pipeline_options){
                                                        .struct_size = sizeof(sc_media_pipeline_options),
                                                        .limits = limits(64),
                                                    },
                                                    &malformed_audio,
                                                    sc_str_from_cstr(""),
                                                    &result),
                              SC_ERR_PARSE);
    failures += sc_test_expect_status("silent from bytes",
                              sc_media_attachment_from_bytes(sc_allocator_heap(),
                                                             &silent_input,
                                                             &standard_limits,
                                                             &silent_audio),
                              SC_OK);
    vad_options = (sc_media_pipeline_options){
        .struct_size = sizeof(vad_options),
        .limits = limits(64),
        .vad_enabled = true,
    };
    failures += sc_test_expect_status("vad no speech",
                              sc_media_pipeline_run(sc_allocator_heap(), &vad_options, &silent_audio, sc_str_from_cstr(""), &result),
                              SC_OK);
    failures += sc_test_expect_true("no speech result", result.no_speech && strstr(result.prompt_context.ptr, "no detected speech") != nullptr);
    sc_media_pipeline_result_clear(&result);
    vad_options.timeout_elapsed = true;
    failures += sc_test_expect_status("timeout pipeline",
                              sc_media_pipeline_run(sc_allocator_heap(), &vad_options, &silent_audio, sc_str_from_cstr(""), &result),
                              SC_ERR_TIMEOUT);

    sc_media_attachment_clear(&silent_audio);
    sc_media_attachment_clear(&malformed_audio);
    return failures;
}

static int test_transcription_tts_and_turn_attachment(void)
{
    int failures = 0;
    const unsigned char speech[] = {'R', 'I', 'F', 'F', 16, 0, 0, 0, 'W', 'A', 'V', 'E', 1, 0, 0, 0};
    const unsigned char tts_bytes[] = {'R', 'I', 'F', 'F', 16, 0, 0, 0, 'W', 'A', 'V', 'E', 2, 0, 0, 0};
    sc_media_attachment speech_input = input_attachment("audio-speech", "audio/wav", "speech.wav", speech, sizeof(speech));
    sc_media_limits standard_limits = limits(64);
    sc_media_attachment audio = {0};
    sc_transcriber *transcriber = nullptr;
    sc_transcriber *failing_transcriber = nullptr;
    sc_tts *tts = nullptr;
    sc_media_pipeline_result result = {0};
    sc_turn turn = {0};

    failures += sc_test_expect_true("voice off default", SC_ENABLE_VOICE == 0);
    failures += sc_test_expect_true("audio preprocessing off default", SC_ENABLE_AUDIO_PREPROCESSING == 0);
    failures += sc_test_expect_status("audio from bytes",
                              sc_media_attachment_from_bytes(sc_allocator_heap(),
                                                             &speech_input,
                                                             &standard_limits,
                                                             &audio),
                              SC_OK);
    failures += sc_test_expect_status("transcriber",
                              sc_transcriber_mock_new(sc_allocator_heap(), sc_str_from_cstr("hello from audio"), false, &transcriber),
                              SC_OK);
    failures += sc_test_expect_status("failing transcriber",
                              sc_transcriber_mock_new(sc_allocator_heap(), sc_str_from_cstr("unused"), true, &failing_transcriber),
                              SC_OK);
    failures += sc_test_expect_status("tts",
                              sc_tts_mock_new(sc_allocator_heap(), sc_buf_from_parts(tts_bytes, sizeof(tts_bytes)), sc_str_from_cstr("audio/wav"), false, &tts),
                              SC_OK);
    failures += sc_test_expect_status("pipeline",
                              sc_media_pipeline_run(sc_allocator_heap(),
                                                    &(sc_media_pipeline_options){
                                                        .struct_size = sizeof(sc_media_pipeline_options),
                                                        .limits = limits(64),
                                                        .transcriber = transcriber,
                                                        .tts = tts,
                                                        .audio_preprocessing_enabled = false,
                                                        .vad_enabled = true,
                                                        .synthesize_response = true,
                                                    },
                                                    &audio,
                                                    sc_str_from_cstr("spoken reply"),
                                                    &result),
                              SC_OK);
    failures += sc_test_expect_true("transcript copied", strcmp(result.transcript.ptr, "hello from audio") == 0);
    failures += sc_test_expect_true("prompt context", strstr(result.prompt_context.ptr, "Media transcript: hello from audio") != nullptr);
    failures += sc_test_expect_true("tts audio copied", result.speech_audio.len == sizeof(tts_bytes) && strcmp(result.speech_content_type.ptr, "audio/wav") == 0);
    turn = (sc_turn){
        .struct_size = sizeof(turn),
        .input = sc_str_from_cstr("text input"),
        .session_id = sc_str_from_cstr("session"),
        .media = &audio,
        .media_count = 1,
        .media_context = sc_string_as_str(&result.prompt_context),
    };
    failures += sc_test_expect_true("turn carries media", turn.media_count == 1 && turn.media == &audio && turn.media_context.len > 0);
    sc_media_pipeline_result_clear(&result);
    failures += sc_test_expect_status("transcription error normalized",
                              sc_media_pipeline_run(sc_allocator_heap(),
                                                    &(sc_media_pipeline_options){
                                                        .struct_size = sizeof(sc_media_pipeline_options),
                                                        .limits = limits(64),
                                                        .transcriber = failing_transcriber,
                                                    },
                                                    &audio,
                                                    sc_str_from_cstr(""),
                                                    &result),
                              SC_ERR_HTTP);

    sc_media_pipeline_result_clear(&result);
    sc_tts_destroy(tts);
    sc_transcriber_destroy(failing_transcriber);
    sc_transcriber_destroy(transcriber);
    sc_media_attachment_clear(&audio);
    return failures;
}
