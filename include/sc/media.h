#pragma once

#include "sc/allocator.h"
#include "sc/buffer.h"
#include "sc/contract.h"
#include "sc/log.h"
#include "sc/result.h"
#include "sc/string.h"

SC_BEGIN_DECLS

typedef struct sc_transcriber sc_transcriber;
typedef struct sc_tts sc_tts;

typedef struct sc_transcriber_whisper_http_request {
    size_t struct_size;
    sc_str method;
    sc_str url;
    const struct sc_media_attachment *attachment;
    sc_str model;
    sc_str language;
    sc_str prompt;
    sc_str temperature;
    sc_str temperature_inc;
    sc_str response_format;
    uint32_t timeout_ms;
    size_t max_audio_bytes;
    size_t max_response_bytes;
} sc_transcriber_whisper_http_request;

typedef sc_status (*sc_transcriber_whisper_http_fn)(void *user,
                                                    const sc_transcriber_whisper_http_request *request,
                                                    sc_allocator *alloc,
                                                    sc_string *out);

typedef sc_status (*sc_tts_piper_http_fn)(void *user,
                                          sc_str method,
                                          sc_str url,
                                          sc_str body,
                                          uint32_t timeout_ms,
                                          size_t max_bytes,
                                          sc_allocator *alloc,
                                          sc_bytes *out);

enum {
    SC_MEDIA_SAFETY_NONE = 0u,
    SC_MEDIA_SAFETY_REDACTED = 1u << 0u,
    SC_MEDIA_SAFETY_PERSONAL_DATA = 1u << 1u,
    SC_MEDIA_SAFETY_SECRET = 1u << 2u,
    SC_MEDIA_SAFETY_UNTRUSTED = 1u << 3u
};

typedef enum sc_media_storage_kind {
    SC_MEDIA_STORAGE_NONE = 0,
    SC_MEDIA_STORAGE_BYTES,
    SC_MEDIA_STORAGE_PATH
} sc_media_storage_kind;

typedef struct sc_media_attachment {
    size_t struct_size;
    sc_str attachment_id;
    sc_str content_type;
    sc_str filename;
    size_t size_bytes;
    sc_media_storage_kind storage_kind;
    sc_str storage_path;
    sc_buf bytes;
    sc_str hash;
    uint32_t safety_flags;
    bool redacted;
    bool temporary;
    sc_string owned_attachment_id;
    sc_string owned_content_type;
    sc_string owned_filename;
    sc_string owned_storage_path;
    sc_string owned_hash;
    sc_bytes owned_bytes;
} sc_media_attachment;

typedef struct sc_media_limits {
    size_t struct_size;
    size_t max_bytes;
    uint32_t timeout_ms;
    bool cancel_requested;
    bool timeout_elapsed;
    const sc_str *allowed_content_types;
    size_t allowed_content_type_count;
} sc_media_limits;

typedef struct sc_transcription_request {
    size_t struct_size;
    const sc_media_attachment *attachment;
    sc_str language;
    uint32_t timeout_ms;
    bool cancel_requested;
} sc_transcription_request;

typedef struct sc_transcription_result {
    size_t struct_size;
    sc_string text;
} sc_transcription_result;

typedef struct sc_tts_request {
    size_t struct_size;
    sc_str text;
    sc_str voice;
    uint32_t timeout_ms;
    bool cancel_requested;
} sc_tts_request;

typedef struct sc_tts_result {
    size_t struct_size;
    sc_string content_type;
    sc_bytes audio;
} sc_tts_result;

typedef struct sc_tts_piper_options {
    size_t struct_size;
    sc_str base_url;
    sc_str default_voice;
    sc_str speaker;
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
} sc_tts_piper_options;

typedef struct sc_transcriber_whisper_options {
    size_t struct_size;
    sc_str endpoint_url;
    sc_str model;
    sc_str default_language;
    sc_str prompt;
    sc_str temperature;
    sc_str temperature_inc;
    sc_str response_format;
    uint32_t timeout_ms;
    size_t max_audio_bytes;
    size_t max_response_bytes;
    sc_transcriber_whisper_http_fn http_request;
    void *http_user;
} sc_transcriber_whisper_options;

typedef struct sc_media_pipeline_options {
    size_t struct_size;
    sc_media_limits limits;
    sc_transcriber *transcriber;
    sc_tts *tts;
    bool audio_preprocessing_enabled;
    bool vad_enabled;
    bool synthesize_response;
    bool cancel_requested;
    bool timeout_elapsed;
    uint32_t timeout_ms;
} sc_media_pipeline_options;

typedef struct sc_media_pipeline_result {
    size_t struct_size;
    sc_string prompt_context;
    sc_string transcript;
    sc_string speech_content_type;
    sc_bytes speech_audio;
    bool no_speech;
    bool cleanup_done;
} sc_media_pipeline_result;

typedef struct sc_transcriber_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*transcribe)(void *impl,
                            const sc_transcription_request *request,
                            sc_allocator *alloc,
                            sc_transcription_result *out);
    void (*destroy)(void *impl);
} sc_transcriber_vtab;

typedef struct sc_tts_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*synthesize)(void *impl,
                            const sc_tts_request *request,
                            sc_allocator *alloc,
                            sc_tts_result *out);
    void (*destroy)(void *impl);
} sc_tts_vtab;

sc_status sc_media_attachment_from_bytes(sc_allocator *alloc,
                                         const sc_media_attachment *input,
                                         const sc_media_limits *limits,
                                         sc_media_attachment *out);
sc_status sc_media_attachment_validate(const sc_media_attachment *attachment,
                                       const sc_media_limits *limits);
sc_status sc_media_attachment_store_temp(sc_media_attachment *attachment, sc_str temp_dir);
sc_status sc_media_attachment_cleanup(sc_media_attachment *attachment);
void sc_media_attachment_clear(sc_media_attachment *attachment);
sc_status sc_media_hash_bytes(sc_allocator *alloc, sc_buf bytes, sc_string *out);
bool sc_media_content_type_is_audio(sc_str content_type);
bool sc_media_audio_wav_is_well_formed(sc_buf bytes);
bool sc_media_audio_has_speech(sc_buf bytes);
void sc_media_attachment_filename_log_field(const sc_media_attachment *attachment, sc_log_field *out);

bool sc_transcriber_vtab_valid(const sc_transcriber_vtab *vtab);
sc_status sc_transcriber_new(sc_allocator *alloc, const sc_transcriber_vtab *vtab, void *impl, sc_transcriber **out);
sc_status sc_transcriber_transcribe(sc_transcriber *transcriber,
                                    const sc_transcription_request *request,
                                    sc_allocator *alloc,
                                    sc_transcription_result *out);
const sc_transcriber_vtab *sc_transcriber_vtab_of(const sc_transcriber *transcriber);
void sc_transcriber_destroy(sc_transcriber *transcriber);
void sc_transcription_result_clear(sc_transcription_result *result);
sc_status sc_transcriber_mock_new(sc_allocator *alloc, sc_str text, bool fail, sc_transcriber **out);
sc_status sc_transcriber_whisper_new(sc_allocator *alloc,
                                     const sc_transcriber_whisper_options *options,
                                     sc_transcriber **out);

bool sc_tts_vtab_valid(const sc_tts_vtab *vtab);
sc_status sc_tts_new(sc_allocator *alloc, const sc_tts_vtab *vtab, void *impl, sc_tts **out);
sc_status sc_tts_synthesize(sc_tts *tts,
                            const sc_tts_request *request,
                            sc_allocator *alloc,
                            sc_tts_result *out);
const sc_tts_vtab *sc_tts_vtab_of(const sc_tts *tts);
void sc_tts_destroy(sc_tts *tts);
void sc_tts_result_clear(sc_tts_result *result);
sc_status sc_tts_mock_new(sc_allocator *alloc, sc_buf audio, sc_str content_type, bool fail, sc_tts **out);
sc_status sc_tts_piper_new(sc_allocator *alloc, const sc_tts_piper_options *options, sc_tts **out);

sc_status sc_media_pipeline_run(sc_allocator *alloc,
                                const sc_media_pipeline_options *options,
                                const sc_media_attachment *attachment,
                                sc_str response_text,
                                sc_media_pipeline_result *out);
void sc_media_pipeline_result_clear(sc_media_pipeline_result *result);

SC_END_DECLS
