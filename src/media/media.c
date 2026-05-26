#include "sc/media.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sc/api.h"
#include "sc/time.h"

typedef struct sc_contract_handle {
    sc_allocator *alloc;
    const void *vtab;
    void *impl;
} sc_contract_handle;

struct sc_transcriber {
    sc_contract_handle base;
};

struct sc_tts {
    sc_contract_handle base;
};

typedef struct mock_transcriber {
    sc_allocator *alloc;
    sc_string text;
    bool fail;
} mock_transcriber;

typedef struct mock_tts {
    sc_allocator *alloc;
    sc_bytes audio;
    sc_string content_type;
    bool fail;
} mock_tts;

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status strip_tts_markdown(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status append_tts_plain_char(sc_string_builder *builder, char ch, bool *last_space);
static bool markdown_list_marker_at(sc_str input, size_t offset);
static bool ascii_space(char ch);
static bool markdown_symbol(char ch);
static sc_str empty_if_null(sc_str value);
static sc_status ensure_not_cancelled_or_timed_out(bool cancelled, bool timed_out);
static bool content_type_allowed(sc_str content_type, const sc_media_limits *limits);
static size_t attachment_size(const sc_media_attachment *attachment);
static bool media_id_is_path_safe(sc_str id);
static sc_status build_temp_path(sc_allocator *alloc, sc_str temp_dir, sc_str id, sc_string *out);
static sc_status write_all_to_file(sc_str path, sc_buf bytes);
static sc_status make_prompt_context(sc_allocator *alloc, sc_str prefix, sc_str value, sc_string *out);
static bool vtab_common_valid(size_t struct_size, uint32_t abi_major, const char *name, bool required_present, bool destroy_present);
static sc_status mock_transcribe(void *impl,
                                 const sc_transcription_request *request,
                                 sc_allocator *alloc,
                                 sc_transcription_result *out);
static void mock_transcriber_destroy(void *impl);
static sc_status mock_synthesize(void *impl,
                                 const sc_tts_request *request,
                                 sc_allocator *alloc,
                                 sc_tts_result *out);
static void mock_tts_destroy(void *impl);

static const sc_transcriber_vtab mock_transcriber_vtab = {
    .struct_size = sizeof(sc_transcriber_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-transcriber",
    .display_name = "Mock transcriber",
    .feature_flag = "SC_TRANSCRIBER_MOCK",
    .capabilities = SC_CONTRACT_CAP_BINARY,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .transcribe = mock_transcribe,
    .destroy = mock_transcriber_destroy,
};

static const sc_tts_vtab mock_tts_vtab = {
    .struct_size = sizeof(sc_tts_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-tts",
    .display_name = "Mock TTS",
    .feature_flag = "SC_TTS_MOCK",
    .capabilities = SC_CONTRACT_CAP_BINARY,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .synthesize = mock_synthesize,
    .destroy = mock_tts_destroy,
};

sc_status sc_media_attachment_from_bytes(sc_allocator *alloc,
                                         const sc_media_attachment *input,
                                         const sc_media_limits *limits,
                                         sc_media_attachment *out)
{
    sc_media_attachment tmp = {0};
    sc_status status;

    if (input == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.media.attachment.invalid_argument");
    }
    if (input->bytes.len > 0 && input->bytes.ptr == nullptr) {
        return sc_status_invalid_argument("sc.media.attachment.invalid_bytes");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tmp = (sc_media_attachment){
        .struct_size = sizeof(tmp),
        .size_bytes = input->bytes.len,
        .storage_kind = SC_MEDIA_STORAGE_BYTES,
        .safety_flags = input->safety_flags,
        .redacted = input->redacted,
    };

    if (input->attachment_id.len == 0) {
        status = sc_uuid_v4(alloc, &tmp.owned_attachment_id);
    } else {
        status = copy_string(alloc, input->attachment_id, &tmp.owned_attachment_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, input->content_type, &tmp.owned_content_type);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, input->filename, &tmp.owned_filename);
    }
    if (sc_status_is_ok(status)) {
        status = sc_bytes_from_buf(alloc, input->bytes, &tmp.owned_bytes);
    }
    if (sc_status_is_ok(status)) {
        if (input->hash.len > 0) {
            status = copy_string(alloc, input->hash, &tmp.owned_hash);
        } else {
            status = sc_media_hash_bytes(alloc, input->bytes, &tmp.owned_hash);
        }
    }
    if (sc_status_is_ok(status)) {
        tmp.attachment_id = sc_string_as_str(&tmp.owned_attachment_id);
        tmp.content_type = sc_string_as_str(&tmp.owned_content_type);
        tmp.filename = sc_string_as_str(&tmp.owned_filename);
        tmp.bytes = sc_buf_from_parts(tmp.owned_bytes.ptr, tmp.owned_bytes.len);
        tmp.hash = sc_string_as_str(&tmp.owned_hash);
        status = sc_media_attachment_validate(&tmp, limits);
    }
    if (!sc_status_is_ok(status)) {
        sc_media_attachment_clear(&tmp);
        return status;
    }

    *out = tmp;
    return sc_status_ok();
}

sc_status sc_media_attachment_validate(const sc_media_attachment *attachment,
                                       const sc_media_limits *limits)
{
    size_t size = 0;
    sc_status status;

    if (attachment == nullptr) {
        return sc_status_invalid_argument("sc.media.attachment.invalid_argument");
    }
    status = ensure_not_cancelled_or_timed_out(limits != nullptr && limits->cancel_requested,
                                              limits != nullptr && limits->timeout_elapsed);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (attachment->content_type.len == 0 || attachment->content_type.ptr == nullptr) {
        return sc_status_unsupported("sc.media.content_type.missing");
    }
    if (!content_type_allowed(attachment->content_type, limits)) {
        return sc_status_unsupported("sc.media.content_type.unsupported");
    }

    size = attachment_size(attachment);
    if (size == 0) {
        return sc_status_invalid_argument("sc.media.attachment.empty");
    }
    if (limits != nullptr && limits->max_bytes > 0 && size > limits->max_bytes) {
        return sc_status_security_denied("sc.media.attachment.too_large");
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_BYTES && attachment->bytes.ptr == nullptr) {
        return sc_status_invalid_argument("sc.media.attachment.invalid_bytes");
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_PATH && attachment->storage_path.len == 0) {
        return sc_status_invalid_argument("sc.media.attachment.invalid_path");
    }
    return sc_status_ok();
}

sc_status sc_media_attachment_store_temp(sc_media_attachment *attachment, sc_str temp_dir)
{
    sc_string path = {0};
    sc_status status;

    if (attachment == nullptr || temp_dir.len == 0 || temp_dir.ptr == nullptr) {
        return sc_status_invalid_argument("sc.media.temp.invalid_argument");
    }
    if (attachment->storage_kind != SC_MEDIA_STORAGE_BYTES || attachment->bytes.ptr == nullptr || attachment->bytes.len == 0) {
        return sc_status_invalid_argument("sc.media.temp.invalid_bytes");
    }
    if (!media_id_is_path_safe(attachment->attachment_id)) {
        return sc_status_security_denied("sc.media.temp.unsafe_id");
    }

    status = build_temp_path(attachment->owned_attachment_id.alloc, temp_dir, attachment->attachment_id, &path);
    if (sc_status_is_ok(status)) {
        status = write_all_to_file(sc_string_as_str(&path), attachment->bytes);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&path);
        return status;
    }

    sc_string_clear(&attachment->owned_storage_path);
    attachment->owned_storage_path = path;
    attachment->storage_path = sc_string_as_str(&attachment->owned_storage_path);
    attachment->storage_kind = SC_MEDIA_STORAGE_PATH;
    attachment->temporary = true;
    return sc_status_ok();
}

sc_status sc_media_attachment_cleanup(sc_media_attachment *attachment)
{
    if (attachment == nullptr) {
        return sc_status_invalid_argument("sc.media.cleanup.invalid_argument");
    }
    if (!attachment->temporary || attachment->storage_path.len == 0 || attachment->storage_path.ptr == nullptr) {
        return sc_status_ok();
    }
    if (unlink(attachment->storage_path.ptr) != 0) {
        return sc_status_io("sc.media.cleanup.unlink_failed");
    }
    attachment->temporary = false;
    return sc_status_ok();
}

void sc_media_attachment_clear(sc_media_attachment *attachment)
{
    if (attachment == nullptr) {
        return;
    }
    if (attachment->temporary && attachment->storage_path.ptr != nullptr) {
        (void)unlink(attachment->storage_path.ptr);
    }
    sc_string_clear(&attachment->owned_attachment_id);
    sc_string_clear(&attachment->owned_content_type);
    sc_string_clear(&attachment->owned_filename);
    sc_string_clear(&attachment->owned_storage_path);
    sc_string_clear(&attachment->owned_hash);
    sc_bytes_clear(&attachment->owned_bytes);
    *attachment = (sc_media_attachment){0};
}

sc_status sc_media_hash_bytes(sc_allocator *alloc, sc_buf bytes, sc_string *out)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    char text[17] = {0};
    int written = 0;

    if (out == nullptr || (bytes.len > 0 && bytes.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.media.hash.invalid_argument");
    }
    for (size_t i = 0; i < bytes.len; i += 1) {
        hash ^= (uint64_t)bytes.ptr[i];
        hash *= UINT64_C(1099511628211);
    }
    written = snprintf(text, sizeof(text), "%016llx", (unsigned long long)hash);
    if (written != 16) {
        return sc_status_io("sc.media.hash.format_failed");
    }
    return sc_string_from_cstr(alloc, text, out);
}

bool sc_media_content_type_is_audio(sc_str content_type)
{
    return sc_str_equal(content_type, sc_str_from_cstr("audio/wav")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/x-wav")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/mpeg")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/ogg")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/opus")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/webm")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/mp4")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/flac")) ||
           sc_str_equal(content_type, sc_str_from_cstr("audio/aac"));
}

bool sc_media_audio_wav_is_well_formed(sc_buf bytes)
{
    return bytes.len >= 12 &&
           bytes.ptr != nullptr &&
           memcmp(bytes.ptr, "RIFF", 4) == 0 &&
           memcmp(bytes.ptr + 8, "WAVE", 4) == 0;
}

bool sc_media_audio_has_speech(sc_buf bytes)
{
    if (bytes.ptr == nullptr || bytes.len == 0) {
        return false;
    }
    for (size_t i = 12; i < bytes.len; i += 1) {
        if (bytes.ptr[i] != 0) {
            return true;
        }
    }
    return false;
}

void sc_media_attachment_filename_log_field(const sc_media_attachment *attachment, sc_log_field *out)
{
    if (out == nullptr) {
        return;
    }
    *out = (sc_log_field){.key = "filename"};
    if (attachment == nullptr) {
        return;
    }
    out->value = attachment->filename;
    out->secret = attachment->redacted ||
                  (attachment->safety_flags & (SC_MEDIA_SAFETY_REDACTED | SC_MEDIA_SAFETY_PERSONAL_DATA | SC_MEDIA_SAFETY_SECRET)) != 0;
}

bool sc_transcriber_vtab_valid(const sc_transcriber_vtab *vtab)
{
    return vtab != nullptr &&
           vtab->struct_size >= sizeof(*vtab) &&
           vtab_common_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->transcribe != nullptr, vtab->destroy != nullptr);
}

sc_status sc_transcriber_new(sc_allocator *alloc, const sc_transcriber_vtab *vtab, void *impl, sc_transcriber **out)
{
    sc_transcriber *transcriber = nullptr;
    if (out == nullptr || !sc_transcriber_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.transcriber.invalid_vtab");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    transcriber = sc_alloc(alloc, sizeof(*transcriber), _Alignof(sc_transcriber));
    if (transcriber == nullptr) {
        return sc_status_no_memory();
    }
    *transcriber = (sc_transcriber){.base = {.alloc = alloc, .vtab = vtab, .impl = impl}};
    *out = transcriber;
    return sc_status_ok();
}

sc_status sc_transcriber_transcribe(sc_transcriber *transcriber,
                                    const sc_transcription_request *request,
                                    sc_allocator *alloc,
                                    sc_transcription_result *out)
{
    const sc_transcriber_vtab *vtab = transcriber == nullptr ? nullptr : transcriber->base.vtab;
    if (transcriber == nullptr || request == nullptr || out == nullptr || vtab == nullptr || vtab->transcribe == nullptr) {
        return sc_status_invalid_argument("sc.transcriber.invalid_argument");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.transcriber.cancelled");
    }
    return vtab->transcribe(transcriber->base.impl, request, alloc, out);
}

const sc_transcriber_vtab *sc_transcriber_vtab_of(const sc_transcriber *transcriber)
{
    return transcriber == nullptr ? nullptr : transcriber->base.vtab;
}

void sc_transcriber_destroy(sc_transcriber *transcriber)
{
    const sc_transcriber_vtab *vtab = transcriber == nullptr ? nullptr : transcriber->base.vtab;
    if (transcriber == nullptr) {
        return;
    }
    if (vtab != nullptr && vtab->destroy != nullptr) {
        vtab->destroy(transcriber->base.impl);
    }
    sc_free(transcriber->base.alloc, transcriber, sizeof(*transcriber), _Alignof(sc_transcriber));
}

void sc_transcription_result_clear(sc_transcription_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->text);
    *result = (sc_transcription_result){0};
}

sc_status sc_transcriber_mock_new(sc_allocator *alloc, sc_str text, bool fail, sc_transcriber **out)
{
    mock_transcriber *impl = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.transcriber_mock.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(mock_transcriber));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (mock_transcriber){.alloc = alloc, .fail = fail};
    status = copy_string(alloc, text.len == 0 ? sc_str_from_cstr("mock transcript") : text, &impl->text);
    if (sc_status_is_ok(status)) {
        status = sc_transcriber_new(alloc, &mock_transcriber_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        mock_transcriber_destroy(impl);
    }
    return status;
}

bool sc_tts_vtab_valid(const sc_tts_vtab *vtab)
{
    return vtab != nullptr &&
           vtab->struct_size >= sizeof(*vtab) &&
           vtab_common_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->synthesize != nullptr, vtab->destroy != nullptr);
}

sc_status sc_tts_new(sc_allocator *alloc, const sc_tts_vtab *vtab, void *impl, sc_tts **out)
{
    sc_tts *tts = nullptr;
    if (out == nullptr || !sc_tts_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.tts.invalid_vtab");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tts = sc_alloc(alloc, sizeof(*tts), _Alignof(sc_tts));
    if (tts == nullptr) {
        return sc_status_no_memory();
    }
    *tts = (sc_tts){.base = {.alloc = alloc, .vtab = vtab, .impl = impl}};
    *out = tts;
    return sc_status_ok();
}

sc_status sc_tts_synthesize(sc_tts *tts,
                            const sc_tts_request *request,
                            sc_allocator *alloc,
                            sc_tts_result *out)
{
    const sc_tts_vtab *vtab = tts == nullptr ? nullptr : tts->base.vtab;
    sc_tts_request plain_request = {0};
    sc_string plain_text = {0};
    sc_status status;

    if (tts == nullptr || request == nullptr || out == nullptr || vtab == nullptr || vtab->synthesize == nullptr) {
        return sc_status_invalid_argument("sc.tts.invalid_argument");
    }
    if (request->text.len > 0 && request->text.ptr == nullptr) {
        return sc_status_invalid_argument("sc.tts.invalid_argument");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.tts.cancelled");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = strip_tts_markdown(alloc, request->text, &plain_text);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    plain_request = *request;
    plain_request.text = sc_string_as_str(&plain_text);
    status = vtab->synthesize(tts->base.impl, &plain_request, alloc, out);
    sc_string_clear(&plain_text);
    return status;
}

const sc_tts_vtab *sc_tts_vtab_of(const sc_tts *tts)
{
    return tts == nullptr ? nullptr : tts->base.vtab;
}

void sc_tts_destroy(sc_tts *tts)
{
    const sc_tts_vtab *vtab = tts == nullptr ? nullptr : tts->base.vtab;
    if (tts == nullptr) {
        return;
    }
    if (vtab != nullptr && vtab->destroy != nullptr) {
        vtab->destroy(tts->base.impl);
    }
    sc_free(tts->base.alloc, tts, sizeof(*tts), _Alignof(sc_tts));
}

void sc_tts_result_clear(sc_tts_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->content_type);
    sc_bytes_clear(&result->audio);
    *result = (sc_tts_result){0};
}

sc_status sc_tts_mock_new(sc_allocator *alloc, sc_buf audio, sc_str content_type, bool fail, sc_tts **out)
{
    mock_tts *impl = nullptr;
    sc_status status;

    if (out == nullptr || (audio.len > 0 && audio.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.tts_mock.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(mock_tts));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (mock_tts){.alloc = alloc, .fail = fail};
    status = sc_bytes_from_buf(alloc, audio, &impl->audio);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, content_type.len == 0 ? sc_str_from_cstr("audio/wav") : content_type, &impl->content_type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tts_new(alloc, &mock_tts_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        mock_tts_destroy(impl);
    }
    return status;
}

sc_status sc_media_pipeline_run(sc_allocator *alloc,
                                const sc_media_pipeline_options *options,
                                const sc_media_attachment *attachment,
                                sc_str response_text,
                                sc_media_pipeline_result *out)
{
    sc_transcription_result transcription = {0};
    sc_tts_result speech = {0};
    sc_status status;
    bool cancelled = false;
    bool timed_out = false;
    uint32_t timeout_ms = 0;

    if (options == nullptr || attachment == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.media.pipeline.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_media_pipeline_result){.struct_size = sizeof(*out)};
    cancelled = options->cancel_requested || options->limits.cancel_requested;
    timed_out = options->timeout_elapsed || options->limits.timeout_elapsed;
    timeout_ms = options->timeout_ms == 0 ? options->limits.timeout_ms : options->timeout_ms;

    status = ensure_not_cancelled_or_timed_out(cancelled, timed_out);
    if (sc_status_is_ok(status)) {
        status = sc_media_attachment_validate(attachment, &options->limits);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }

    if (!sc_media_content_type_is_audio(attachment->content_type)) {
        return make_prompt_context(alloc, sc_str_from_cstr("Attached media: "), attachment->filename, &out->prompt_context);
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_BYTES &&
        (sc_str_equal(attachment->content_type, sc_str_from_cstr("audio/wav")) ||
         sc_str_equal(attachment->content_type, sc_str_from_cstr("audio/x-wav")))) {
        if (!sc_media_audio_wav_is_well_formed(attachment->bytes)) {
            return sc_status_parse("sc.media.audio.malformed");
        }
    }
    if (options->vad_enabled && attachment->storage_kind == SC_MEDIA_STORAGE_BYTES && !sc_media_audio_has_speech(attachment->bytes)) {
        out->no_speech = true;
        return sc_string_from_cstr(alloc, "Media audio contained no detected speech.", &out->prompt_context);
    }

    status = ensure_not_cancelled_or_timed_out(cancelled, timed_out);
    if (sc_status_is_ok(status)) {
        if (options->transcriber == nullptr) {
            status = sc_status_unsupported("sc.media.transcriber.missing");
        } else {
            sc_transcription_request request = {
                .struct_size = sizeof(request),
                .attachment = attachment,
                .timeout_ms = timeout_ms,
                .cancel_requested = cancelled,
            };
            status = sc_transcriber_transcribe(options->transcriber, &request, alloc, &transcription);
        }
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&transcription.text), &out->transcript);
    }
    if (sc_status_is_ok(status)) {
        status = make_prompt_context(alloc, sc_str_from_cstr("Media transcript: "), sc_string_as_str(&transcription.text), &out->prompt_context);
    }
    if (sc_status_is_ok(status) && options->synthesize_response && options->tts != nullptr && response_text.len > 0) {
        sc_tts_request request = {
            .struct_size = sizeof(request),
            .text = response_text,
            .timeout_ms = timeout_ms,
            .cancel_requested = cancelled,
        };
        status = ensure_not_cancelled_or_timed_out(cancelled, timed_out);
        if (sc_status_is_ok(status)) {
            status = sc_tts_synthesize(options->tts, &request, alloc, &speech);
        }
        if (sc_status_is_ok(status)) {
            status = copy_string(alloc, sc_string_as_str(&speech.content_type), &out->speech_content_type);
        }
        if (sc_status_is_ok(status)) {
            status = sc_bytes_from_buf(alloc, sc_buf_from_parts(speech.audio.ptr, speech.audio.len), &out->speech_audio);
        }
    }

    sc_transcription_result_clear(&transcription);
    sc_tts_result_clear(&speech);
    if (!sc_status_is_ok(status)) {
        sc_media_pipeline_result_clear(out);
    }
    return status;
}

void sc_media_pipeline_result_clear(sc_media_pipeline_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->prompt_context);
    sc_string_clear(&result->transcript);
    sc_string_clear(&result->speech_content_type);
    sc_bytes_clear(&result->speech_audio);
    *result = (sc_media_pipeline_result){0};
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out)
{
    return sc_string_from_str(alloc, empty_if_null(input), out);
}

static sc_status strip_tts_markdown(sc_allocator *alloc, sc_str input, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    bool last_space = true;

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.tts.markdown.invalid_argument");
    }

    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < input.len; i += 1) {
        char ch = input.ptr[i];

        if (ch == '\\' && i + 1U < input.len) {
            i += 1;
            status = append_tts_plain_char(&builder, input.ptr[i], &last_space);
        } else if (ch == ']' && i + 1U < input.len && input.ptr[i + 1U] == '(') {
            i += 2;
            while (i < input.len && input.ptr[i] != ')') {
                i += 1;
            }
        } else if (markdown_list_marker_at(input, i)) {
            status = append_tts_plain_char(&builder, ' ', &last_space);
            i += 1;
        } else if (markdown_symbol(ch)) {
            continue;
        } else {
            status = append_tts_plain_char(&builder, ch, &last_space);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_tts_plain_char(sc_string_builder *builder, char ch, bool *last_space)
{
    if (ascii_space(ch)) {
        if (*last_space) {
            return sc_status_ok();
        }
        ch = ' ';
        *last_space = true;
    } else {
        *last_space = false;
    }
    return sc_string_builder_append(builder, sc_str_from_parts(&ch, 1));
}

static bool markdown_list_marker_at(sc_str input, size_t offset)
{
    if (input.ptr == nullptr || offset >= input.len || (input.ptr[offset] != '-' && input.ptr[offset] != '+')) {
        return false;
    }
    if (offset + 1U >= input.len || !ascii_space(input.ptr[offset + 1U])) {
        return false;
    }
    for (size_t i = offset; i > 0; i -= 1) {
        char previous = input.ptr[i - 1U];
        if (previous == '\n' || previous == '\r') {
            return true;
        }
        if (!ascii_space(previous)) {
            return false;
        }
    }
    return true;
}

static bool ascii_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v';
}

static bool markdown_symbol(char ch)
{
    switch (ch) {
    case '`':
    case '*':
    case '_':
    case '{':
    case '}':
    case '[':
    case ']':
    case '(':
    case ')':
    case '#':
    case '>':
    case '~':
    case '|':
    case '!':
        return true;
    default:
        return false;
    }
}

static sc_str empty_if_null(sc_str value)
{
    return value.ptr == nullptr ? sc_str_from_cstr("") : value;
}

static sc_status ensure_not_cancelled_or_timed_out(bool cancelled, bool timed_out)
{
    if (cancelled) {
        return sc_status_cancelled("sc.media.cancelled");
    }
    if (timed_out) {
        return sc_status_timeout("sc.media.timeout");
    }
    return sc_status_ok();
}

static bool content_type_allowed(sc_str content_type, const sc_media_limits *limits)
{
    static const sc_str defaults[] = {
        {.ptr = "text/plain", .len = 10},
        {.ptr = "audio/wav", .len = 9},
        {.ptr = "audio/x-wav", .len = 11},
        {.ptr = "audio/mpeg", .len = 10},
        {.ptr = "audio/ogg", .len = 9},
        {.ptr = "audio/opus", .len = 10},
        {.ptr = "audio/webm", .len = 10},
        {.ptr = "audio/mp4", .len = 9},
        {.ptr = "audio/flac", .len = 10},
        {.ptr = "audio/aac", .len = 9},
        {.ptr = "image/png", .len = 9},
        {.ptr = "image/jpeg", .len = 10},
    };
    const sc_str *allowed = defaults;
    size_t count = sizeof(defaults) / sizeof(defaults[0]);

    if (limits != nullptr && limits->allowed_content_type_count > 0 && limits->allowed_content_types != nullptr) {
        allowed = limits->allowed_content_types;
        count = limits->allowed_content_type_count;
    }
    for (size_t i = 0; i < count; i += 1) {
        if (sc_str_equal(content_type, allowed[i])) {
            return true;
        }
    }
    return false;
}

static size_t attachment_size(const sc_media_attachment *attachment)
{
    if (attachment == nullptr) {
        return 0;
    }
    if (attachment->size_bytes > 0) {
        return attachment->size_bytes;
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_BYTES) {
        return attachment->bytes.len;
    }
    return 0;
}

static bool media_id_is_path_safe(sc_str id)
{
    if (id.len == 0 || id.ptr == nullptr) {
        return false;
    }
    for (size_t i = 0; i < id.len; i += 1) {
        unsigned char ch = (unsigned char)id.ptr[i];
        if (!((ch >= 'a' && ch <= 'z') ||
              (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') ||
              ch == '-' ||
              ch == '_')) {
            return false;
        }
    }
    return true;
}

static sc_status build_temp_path(sc_allocator *alloc, sc_str temp_dir, sc_str id, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, temp_dir);
    if (sc_status_is_ok(status) && (temp_dir.len == 0 || temp_dir.ptr[temp_dir.len - 1] != '/')) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "sc-media-");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".bin");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status write_all_to_file(sc_str path, sc_buf bytes)
{
    int fd = -1;
    size_t offset = 0;

    if (path.ptr == nullptr || bytes.ptr == nullptr) {
        return sc_status_invalid_argument("sc.media.temp.invalid_argument");
    }
    fd = open(path.ptr, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        return sc_status_io("sc.media.temp.open_failed");
    }
    while (offset < bytes.len) {
        ssize_t written = write(fd, bytes.ptr + offset, bytes.len - offset);
        if (written <= 0) {
            (void)close(fd);
            return sc_status_io("sc.media.temp.write_failed");
        }
        offset += (size_t)written;
    }
    if (close(fd) != 0) {
        return sc_status_io("sc.media.temp.close_failed");
    }
    return sc_status_ok();
}

static sc_status make_prompt_context(sc_allocator *alloc, sc_str prefix, sc_str value, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, prefix);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, value.len == 0 ? sc_str_from_cstr("attachment") : value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool vtab_common_valid(size_t struct_size, uint32_t abi_major, const char *name, bool required_present, bool destroy_present)
{
    (void)struct_size;
    return abi_major == SC_ABI_VERSION_MAJOR &&
           required_present &&
           destroy_present &&
           name != nullptr &&
           sc_contract_name_is_valid(sc_str_from_cstr(name));
}

static sc_status mock_transcribe(void *impl,
                                 const sc_transcription_request *request,
                                 sc_allocator *alloc,
                                 sc_transcription_result *out)
{
    mock_transcriber *transcriber = impl;
    if (transcriber == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.transcriber_mock.invalid_argument");
    }
    if (transcriber->fail) {
        return sc_status_http("sc.transcriber_mock.error");
    }
    *out = (sc_transcription_result){.struct_size = sizeof(*out)};
    return copy_string(alloc, sc_string_as_str(&transcriber->text), &out->text);
}

static void mock_transcriber_destroy(void *impl)
{
    mock_transcriber *transcriber = impl;
    if (transcriber == nullptr) {
        return;
    }
    sc_string_clear(&transcriber->text);
    sc_free(transcriber->alloc, transcriber, sizeof(*transcriber), _Alignof(mock_transcriber));
}

static sc_status mock_synthesize(void *impl,
                                 const sc_tts_request *request,
                                 sc_allocator *alloc,
                                 sc_tts_result *out)
{
    mock_tts *tts = impl;
    if (tts == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.tts_mock.invalid_argument");
    }
    if (tts->fail) {
        return sc_status_http("sc.tts_mock.error");
    }
    *out = (sc_tts_result){.struct_size = sizeof(*out)};
    sc_status status = copy_string(alloc, sc_string_as_str(&tts->content_type), &out->content_type);
    if (sc_status_is_ok(status)) {
        status = sc_bytes_from_buf(alloc, sc_buf_from_parts(tts->audio.ptr, tts->audio.len), &out->audio);
    }
    if (!sc_status_is_ok(status)) {
        sc_tts_result_clear(out);
    }
    return status;
}

static void mock_tts_destroy(void *impl)
{
    mock_tts *tts = impl;
    if (tts == nullptr) {
        return;
    }
    sc_bytes_clear(&tts->audio);
    sc_string_clear(&tts->content_type);
    sc_free(tts->alloc, tts, sizeof(*tts), _Alignof(mock_tts));
}
