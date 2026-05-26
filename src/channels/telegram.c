// cppcheck-suppress-file redundantInitialization
#include "sc/channel.h"

#include "sc/api.h"
#include "sc/log.h"
#include "sc/media.h"

#include "media/media_encoder_internal.h"
#include "net/curl_global.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SC_HAVE_LIBCMARK
#include <cmark.h>
#endif

#ifdef SC_HAVE_PARSON
#include "parson/parson.h"
#endif

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

#ifdef SC_HAVE_PARSON
enum {
    telegram_text_safe_limit_bytes = 3900,
};

typedef struct telegram_channel {
    sc_allocator *alloc;
    sc_string bot_token;
    sc_string api_base_url;
    sc_string bot_username;
    sc_string parse_mode;
    sc_string stream_mode;
    sc_string media_temp_dir;
    sc_string last_draft_chat_id;
    sc_string last_draft_thread_id;
    sc_string last_draft_message_id;
    int64_t next_update_offset;
    long next_draft_id;
    long active_draft_id;
    time_t rate_window_start;
    long poll_timeout_seconds;
    long approval_timeout_seconds;
    long draft_update_interval_ms;
    size_t max_response_bytes;
    size_t max_media_bytes;
    size_t message_split_bytes;
    uint32_t max_retries;
    uint32_t retry_backoff_ms;
    uint32_t rate_limit_per_minute;
    uint32_t rate_window_count;
    uint64_t next_approval_id;
    bool mention_only;
    bool interrupt_on_new_message;
    bool ack_reactions;
    bool post_reactions;
    sc_telegram_http_request_fn http_request;
    void *http_user;
#ifdef SC_HAVE_LIBCURL
    CURL *curl;
#endif
} telegram_channel;

typedef struct telegram_update {
    int64_t update_id;
    int64_t message_id;
    int64_t chat_id;
    int64_t sender_id;
    int64_t thread_id;
    int64_t reply_to_message_id;
    bool has_thread_id;
    bool has_reply_to_message_id;
    const char *text;
    const char *chat_type;
    const char *username;
    const char *reply_to_text;
    const char *media_type;
    const char *file_id;
    const char *file_name;
    size_t file_size;
    int64_t timestamp_unix_secs;
    bool has_timestamp;
    bool media;
    bool post;
    bool callback;
    const char *callback_data;
    const char *callback_query_id;
} telegram_update;

static sc_status telegram_send(void *impl, const sc_channel_message *message);
static sc_status telegram_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status telegram_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status telegram_request_approval(void *impl,
                                           const sc_channel_approval_request *request,
                                           sc_allocator *alloc,
                                           sc_channel_approval_response *out);
static void telegram_destroy(void *impl);
static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status copy_i64_string(sc_allocator *alloc, int64_t value, sc_string *out);
static sc_status append_str(sc_string_builder *builder, sc_str input);
static sc_status append_cstr(sc_string_builder *builder, const char *input);
static sc_status append_i64(sc_string_builder *builder, int64_t value);
static sc_status append_long(sc_string_builder *builder, long value);
static sc_status append_urlencoded(sc_string_builder *builder, sc_str input);
static bool is_url_unreserved(unsigned char ch);
static sc_status telegram_build_url(const telegram_channel *telegram, const char *method, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_get_updates_url(const telegram_channel *telegram, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_get_file_url(const telegram_channel *telegram, sc_str file_id, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_download_file_url(const telegram_channel *telegram, sc_str file_path, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_send_body(const telegram_channel *telegram,
                                          const sc_channel_message *message,
                                          sc_str text,
                                          sc_allocator *alloc,
                                          sc_string *out);
static sc_status telegram_send_approval_message(telegram_channel *telegram,
                                                const sc_channel_approval_request *request,
                                                sc_str approve_data,
                                                sc_str deny_data,
                                                sc_allocator *alloc,
                                                sc_string *out_message_id);
static sc_status telegram_build_approval_body(const telegram_channel *telegram,
                                              const sc_channel_approval_request *request,
                                              sc_str approve_data,
                                              sc_str deny_data,
                                              sc_allocator *alloc,
                                              sc_string *out);
static sc_status telegram_delete_message(telegram_channel *telegram, sc_str chat_id, sc_str message_id);
static sc_status telegram_build_delete_message_body(sc_str chat_id,
                                                    sc_str message_id,
                                                    sc_allocator *alloc,
                                                    sc_string *out);
static sc_status telegram_build_message_draft_body(const telegram_channel *telegram,
                                                   const sc_channel_message *message,
                                                   sc_str text,
                                                   long draft_id,
                                                   sc_allocator *alloc,
                                                   sc_string *out);
static sc_status telegram_build_edit_body(const telegram_channel *telegram,
                                          const sc_channel_message *message,
                                          sc_str text,
                                          sc_allocator *alloc,
                                          sc_string *out);
static sc_status telegram_build_chat_action_body(const sc_channel_message *message,
                                                 sc_str action,
                                                 sc_allocator *alloc,
                                                 sc_string *out);
static sc_status telegram_build_reaction_body(const telegram_update *update, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_audio_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_voice_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_photo_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out);
static sc_status telegram_build_document_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out);
static sc_status telegram_send_attachment(telegram_channel *telegram, const sc_channel_message *message);
static sc_status telegram_send_audio(telegram_channel *telegram, const sc_channel_message *message);
static sc_status telegram_send_photo(telegram_channel *telegram, const sc_channel_message *message);
static sc_status telegram_send_document(telegram_channel *telegram, const sc_channel_message *message);
static sc_status telegram_send_voice_from_wav(telegram_channel *telegram, const sc_channel_message *message);
static sc_status telegram_send_voice(telegram_channel *telegram, const sc_channel_message *message);
static sc_status telegram_send_chat_action(telegram_channel *telegram,
                                           const sc_channel_message *message,
                                           sc_str action);
static sc_status telegram_send_reaction(telegram_channel *telegram, const telegram_update *update);
static void telegram_log_reaction_failure(sc_status status, const telegram_update *update);
static sc_status telegram_format_outbound_text(const telegram_channel *telegram,
                                               sc_str text,
                                               sc_allocator *alloc,
                                               sc_string *formatted,
                                               sc_str *parse_mode);
static sc_status telegram_next_text_chunk(const telegram_channel *telegram,
                                          sc_str text,
                                          size_t offset,
                                          sc_allocator *alloc,
                                          sc_str *out);
static size_t telegram_utf8_chunk_boundary(sc_str text, size_t offset, size_t len);
static bool telegram_should_render_markdown(const telegram_channel *telegram);
static bool sc_str_case_equal_cstr(sc_str left, const char *right);
#ifdef SC_HAVE_LIBCMARK
static sc_status telegram_markdown_to_html(sc_str markdown, sc_allocator *alloc, sc_string *out);
static sc_status telegram_filter_cmark_html(sc_str html, sc_allocator *alloc, sc_string *out);
static bool telegram_html_tag_allowed(sc_str tag);
#endif
static sc_status telegram_http_request(telegram_channel *telegram,
                                       sc_str method,
                                       sc_str url,
                                       sc_str body,
                                       sc_allocator *alloc,
                                       sc_string *out);
static sc_status telegram_redact_url_for_log(sc_allocator *alloc, sc_str url, sc_string *out);
static sc_str telegram_api_method_from_url(sc_str url);
static sc_status telegram_parse_ok_response(sc_str json);
static sc_status telegram_parse_message_id(sc_str json, sc_allocator *alloc, sc_string *out);
static sc_status telegram_parse_file_path(sc_str json, sc_allocator *alloc, sc_string *out);
static sc_status telegram_parse_first_update(telegram_channel *telegram,
                                             sc_str json,
                                             sc_allocator *alloc,
                                             sc_channel_inbound *out,
                                             int64_t *next_offset);
static bool telegram_extract_update(const telegram_channel *telegram, JSON_Object *update, telegram_update *out);
static sc_status telegram_update_to_inbound(telegram_channel *telegram,
                                            const telegram_update *update,
                                            sc_allocator *alloc,
                                            sc_channel_inbound *out);
static sc_status telegram_download_media(telegram_channel *telegram,
                                         const telegram_update *update,
                                         sc_allocator *alloc,
                                         sc_str *content_type,
                                         sc_string *storage_path,
                                         size_t *size_bytes);
static sc_status telegram_store_media_bytes(telegram_channel *telegram,
                                            const telegram_update *update,
                                            sc_str bytes,
                                            sc_allocator *alloc,
                                            sc_str *content_type,
                                            sc_string *storage_path,
                                            size_t *size_bytes);
static sc_status telegram_screen_media_attachment(sc_allocator *alloc,
                                                  const sc_media_attachment *attachment,
                                                  const sc_media_limits *limits);
static sc_str telegram_declared_content_type(const telegram_update *update);
static sc_str telegram_detect_media_type(sc_str bytes);
static sc_str telegram_resolved_content_type(const telegram_update *update,
                                             sc_str declared_type,
                                             sc_str detected_type);
static bool telegram_media_type_matches(const telegram_update *update, sc_str declared_type, sc_str detected_type);
static bool telegram_content_type_is_image(sc_str content_type);
static bool telegram_status_consumes_update(sc_status status);
static sc_status telegram_check_rate_limit(telegram_channel *telegram);
static bool telegram_update_visible(const telegram_channel *telegram, const telegram_update *update);
static bool telegram_update_mentions_bot(const telegram_channel *telegram, const telegram_update *update);
static bool telegram_text_mentions_bot(const telegram_channel *telegram, const char *text);
static sc_status telegram_build_reply_context_text(const telegram_update *update, sc_allocator *alloc, sc_string *out);
static bool str_contains_cstr(sc_str text, const char *needle);
static bool telegram_is_placeholder_draft(const sc_channel_message *message);
static bool stream_mode_is_off(const telegram_channel *telegram);
static bool same_message_target(const telegram_channel *telegram, const sc_channel_message *message);
static bool telegram_attachment_is_wav(const sc_channel_message *message);
static bool telegram_attachment_is_image(const sc_channel_message *message);
static bool telegram_photo_status_allows_document_fallback(sc_status status);
static bool telegram_http_status_retryable(long response_code);
#ifdef SC_HAVE_LIBCURL
static bool telegram_curl_code_retryable(CURLcode code);
static long telegram_curl_http_version(void);
#endif
static void sleep_ms(uint32_t delay_ms);
static bool json_i64(JSON_Object *object, const char *name, int64_t *out);

#ifdef SC_HAVE_LIBCURL
enum {
    telegram_low_memory_upload_buffer_size = 16 * 1024,
};

static sc_status telegram_curl_global_init(void);
static void telegram_curl_apply_common_options(const telegram_channel *telegram, CURL *curl);
static void telegram_curl_release_if_low_memory(telegram_channel *telegram);
static bool telegram_low_memory_transport_enabled(void);
static const char *telegram_ca_bundle_path(void);
typedef struct curl_response {
    sc_bytes bytes;
    size_t max_bytes;
    bool too_large;
} curl_response;

static sc_status telegram_curl_request(telegram_channel *telegram,
                                       sc_str method,
                                       sc_str url,
                                       sc_str body,
                                       sc_allocator *alloc,
                                       sc_string *out);
static sc_status telegram_curl_send_audio_multipart(telegram_channel *telegram,
                                                    sc_str url,
                                                    const sc_channel_message *message,
                                                    sc_str file_field,
                                                    sc_str default_filename,
                                                    sc_str default_content_type,
                                                    sc_allocator *alloc,
                                                    sc_string *out);
static void telegram_curl_response_excerpt(const curl_response *response, char *out, size_t out_size);
static size_t telegram_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
#endif

static const char *const telegram_required_secret_keys[] = {"bot_token"};

static const sc_channel_vtab telegram_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "telegram",
    .display_name = "Telegram bot channel",
    .feature_flag = "SC_CHANNEL_TELEGRAM",
    .description_key = "channel.telegram.description",
    .config_schema_ref = "sc.schema.channels.telegram",
    .required_secret_keys = telegram_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(telegram_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA | SC_CHANNEL_INBOUND_EDIT |
                           SC_CHANNEL_INBOUND_CALLBACK,
    .media_capabilities = SC_CHANNEL_MEDIA_IMAGE | SC_CHANNEL_MEDIA_AUDIO | SC_CHANNEL_MEDIA_VIDEO | SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_DRAFT_UPDATES | SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM |
                    SC_CHANNEL_CAP_REACTIONS | SC_CHANNEL_CAP_ATTACHMENTS | SC_CHANNEL_CAP_APPROVAL_PROMPTS |
                    SC_CHANNEL_CAP_INLINE_APPROVAL_BUTTONS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = telegram_send,
    .listen = telegram_listen,
    .health = telegram_health,
    .request_approval = telegram_request_approval,
    .destroy = telegram_destroy,
};
#endif

sc_status sc_channel_telegram_new(sc_allocator *alloc, const sc_telegram_channel_options *options, sc_channel **out)
{
#ifdef SC_HAVE_PARSON
    telegram_channel *telegram = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.invalid_argument");
    }
    if (options->bot_token.ptr == nullptr || options->bot_token.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.token_missing");
    }
#ifndef SC_HAVE_LIBCURL
    if (options->http_request == nullptr) {
        return sc_status_unsupported("sc.channel_telegram.libcurl_unavailable");
    }
#endif

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    telegram = sc_alloc(alloc, sizeof(*telegram), _Alignof(telegram_channel));
    if (telegram == nullptr) {
        return sc_status_no_memory();
    }
    *telegram = (telegram_channel){
        .alloc = alloc,
        .poll_timeout_seconds = options->poll_timeout_seconds <= 0 ? 30 : options->poll_timeout_seconds,
        .approval_timeout_seconds = options->approval_timeout_seconds <= 0 ? 120 : options->approval_timeout_seconds,
        .draft_update_interval_ms = options->draft_update_interval_ms <= 0 ? 1000 : options->draft_update_interval_ms,
        .max_response_bytes = options->max_response_bytes == 0 ? 1024U * 1024U : options->max_response_bytes,
        .max_media_bytes = options->max_media_bytes == 0 ? 1024U * 1024U : options->max_media_bytes,
        .message_split_bytes = options->message_split_bytes == 0 ? 3900U : options->message_split_bytes,
        .next_draft_id = 1,
        .max_retries = options->max_retries,
        .retry_backoff_ms = options->retry_backoff_ms == 0 ? 250U : options->retry_backoff_ms,
        .rate_limit_per_minute = options->rate_limit_per_minute,
        .mention_only = options->mention_only,
        .interrupt_on_new_message = options->interrupt_on_new_message,
        .ack_reactions = options->ack_reactions,
        .post_reactions = options->struct_size >= offsetof(sc_telegram_channel_options, post_reactions) +
                                                sizeof(options->post_reactions) &&
                          options->post_reactions,
        .http_request = options->http_request,
        .http_user = options->http_user,
    };

    status = copy_string(alloc, options->bot_token, &telegram->bot_token);
    if (sc_status_is_ok(status)) {
        sc_str base = options->api_base_url.ptr == nullptr || options->api_base_url.len == 0
                          ? sc_str_from_cstr("https://api.telegram.org")
                          : options->api_base_url;
        status = copy_string(alloc, base, &telegram->api_base_url);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->bot_username, &telegram->bot_username);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->parse_mode, &telegram->parse_mode);
    }
    if (sc_status_is_ok(status)) {
        sc_str mode = options->stream_mode.ptr == nullptr || options->stream_mode.len == 0 ? sc_str_from_cstr("off")
                                                                                        : options->stream_mode;
        status = copy_string(alloc, mode, &telegram->stream_mode);
    }
    if (sc_status_is_ok(status)) {
        sc_str media_temp_dir = options->media_temp_dir.len == 0 ? sc_str_from_cstr("/tmp") : options->media_temp_dir;
        status = copy_string(alloc, media_temp_dir, &telegram->media_temp_dir);
    }
    if (sc_status_is_ok(status)) {
        status = sc_channel_new(alloc, &telegram_vtab, telegram, out);
    }
    if (!sc_status_is_ok(status)) {
        telegram_destroy(telegram);
    }
    return status;
#else
    (void)alloc;
    (void)options;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.channel_telegram.parson_unavailable");
#endif
}

#ifdef SC_HAVE_PARSON
static sc_status telegram_send(void *impl, const sc_channel_message *message)
{
    telegram_channel *telegram = impl;
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_string message_id = {0};
    sc_status status = sc_status_ok();
    size_t sent = 0;

    if (telegram == nullptr || message == nullptr || message->conversation_id.ptr == nullptr || message->conversation_id.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.send_invalid_argument");
    }
    if (message->draft && (stream_mode_is_off(telegram) || telegram_is_placeholder_draft(message))) {
        (void)telegram_send_chat_action(telegram, message, sc_str_from_cstr("typing"));
        return sc_status_ok();
    }
    if (!message->draft && message->attachment_bytes.len > 0) {
        status = telegram_send_attachment(telegram, message);
        if (!sc_status_is_ok(status) || message->text.len == 0) {
            return status;
        }
    }

    status = sc_status_ok();
    while (sc_status_is_ok(status) && sent < message->text.len) {
        sc_str chunk = {0};
        size_t chunk_len = 0;
        bool used_message_draft = false;
        const char *method = (!message->draft && same_message_target(telegram, message) && telegram->last_draft_message_id.len > 0)
                                 ? "editMessageText"
                                 : "sendMessage";
        bool used_edit = strcmp(method, "editMessageText") == 0;

        status = telegram_next_text_chunk(telegram, message->text, sent, telegram->alloc, &chunk);
        if (!sc_status_is_ok(status)) {
            break;
        }
        chunk_len = chunk.len;
        if (message->draft) {
            if (!same_message_target(telegram, message) || telegram->active_draft_id == 0) {
                telegram->active_draft_id = telegram->next_draft_id;
                telegram->next_draft_id = telegram->next_draft_id == LONG_MAX ? 1 : telegram->next_draft_id + 1;
            }
            method = "sendMessageDraft";
            used_message_draft = true;
            used_edit = false;
        }

        sc_string_clear(&url);
        sc_string_clear(&body);
        sc_string_clear(&response);
        status = telegram_build_url(telegram, method, telegram->alloc, &url);
        if (sc_status_is_ok(status)) {
            status = used_message_draft
                         ? telegram_build_message_draft_body(telegram, message, chunk, telegram->active_draft_id, telegram->alloc, &body)
                     : used_edit
                         ? telegram_build_edit_body(telegram, message, chunk, telegram->alloc, &body)
                         : telegram_build_send_body(telegram, message, chunk, telegram->alloc, &body);
        }
        if (sc_status_is_ok(status)) {
            status = telegram_http_request(telegram,
                                           sc_str_from_cstr("POST"),
                                           sc_string_as_str(&url),
                                           sc_string_as_str(&body),
                                           telegram->alloc,
                                           &response);
        }
        if (sc_status_is_ok(status)) {
            status = telegram_parse_ok_response(sc_string_as_str(&response));
        }
        if (!sc_status_is_ok(status) && used_message_draft) {
            sc_status_clear(&status);
            telegram->active_draft_id = 0;
            sc_string_clear(&telegram->last_draft_chat_id);
            sc_string_clear(&telegram->last_draft_thread_id);
            sc_string_clear(&telegram->last_draft_message_id);
            sc_string_clear(&url);
            sc_string_clear(&body);
            sc_string_clear(&response);

            method = "sendMessage";
            used_message_draft = false;
            status = telegram_build_url(telegram, method, telegram->alloc, &url);
            if (sc_status_is_ok(status)) {
                status = telegram_build_send_body(telegram, message, chunk, telegram->alloc, &body);
            }
            if (sc_status_is_ok(status)) {
                status = telegram_http_request(telegram,
                                               sc_str_from_cstr("POST"),
                                               sc_string_as_str(&url),
                                               sc_string_as_str(&body),
                                               telegram->alloc,
                                               &response);
            }
            if (sc_status_is_ok(status)) {
                status = telegram_parse_ok_response(sc_string_as_str(&response));
            }
        }
        if (!sc_status_is_ok(status) && used_edit) {
            sc_status_clear(&status);
            telegram->active_draft_id = 0;
            sc_string_clear(&telegram->last_draft_chat_id);
            sc_string_clear(&telegram->last_draft_thread_id);
            sc_string_clear(&telegram->last_draft_message_id);
            sc_string_clear(&url);
            sc_string_clear(&body);
            sc_string_clear(&response);

            status = telegram_build_url(telegram, "sendMessage", telegram->alloc, &url);
            if (sc_status_is_ok(status)) {
                status = telegram_build_send_body(telegram, message, chunk, telegram->alloc, &body);
            }
            if (sc_status_is_ok(status)) {
                status = telegram_http_request(telegram,
                                               sc_str_from_cstr("POST"),
                                               sc_string_as_str(&url),
                                               sc_string_as_str(&body),
                                               telegram->alloc,
                                               &response);
            }
            if (sc_status_is_ok(status)) {
                status = telegram_parse_ok_response(sc_string_as_str(&response));
            }
        }
        if (sc_status_is_ok(status) && message->draft && used_message_draft) {
            sc_string_clear(&telegram->last_draft_chat_id);
            sc_string_clear(&telegram->last_draft_thread_id);
            sc_string_clear(&telegram->last_draft_message_id);
            status = copy_string(telegram->alloc, message->conversation_id, &telegram->last_draft_chat_id);
            if (sc_status_is_ok(status)) {
                status = copy_string(telegram->alloc, message->thread_id, &telegram->last_draft_thread_id);
            }
        } else if (sc_status_is_ok(status) && message->draft) {
            sc_string_clear(&message_id);
            status = telegram_parse_message_id(sc_string_as_str(&response), telegram->alloc, &message_id);
            if (sc_status_is_ok(status)) {
                sc_string_clear(&telegram->last_draft_chat_id);
                sc_string_clear(&telegram->last_draft_thread_id);
                sc_string_clear(&telegram->last_draft_message_id);
                status = copy_string(telegram->alloc, message->conversation_id, &telegram->last_draft_chat_id);
            }
            if (sc_status_is_ok(status)) {
                status = copy_string(telegram->alloc, message->thread_id, &telegram->last_draft_thread_id);
            }
            if (sc_status_is_ok(status)) {
                telegram->last_draft_message_id = message_id;
                message_id = (sc_string){0};
            }
        } else if (sc_status_is_ok(status) && !message->draft) {
            telegram->active_draft_id = 0;
            sc_string_clear(&telegram->last_draft_chat_id);
            sc_string_clear(&telegram->last_draft_thread_id);
            sc_string_clear(&telegram->last_draft_message_id);
        }
        sent += chunk_len == 0 ? 1 : chunk_len;
    }
    if (sc_status_is_ok(status) && message->text.len == 0) {
        status = telegram_build_url(telegram, "sendMessage", telegram->alloc, &url);
        if (sc_status_is_ok(status)) {
            status = telegram_build_send_body(telegram, message, sc_str_from_cstr(""), telegram->alloc, &body);
        }
        if (sc_status_is_ok(status)) {
            status = telegram_http_request(telegram,
                                           sc_str_from_cstr("POST"),
                                           sc_string_as_str(&url),
                                           sc_string_as_str(&body),
                                           telegram->alloc,
                                           &response);
        }
        if (sc_status_is_ok(status)) {
            status = telegram_parse_ok_response(sc_string_as_str(&response));
        }
    }

    sc_string_clear(&message_id);
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    telegram_channel *telegram = impl;
    sc_string url = {0};
    sc_string response = {0};
    int64_t next_offset = telegram == nullptr ? 0 : telegram->next_update_offset;
    sc_status status = sc_status_ok();

    if (telegram == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.listen_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    status = telegram_build_get_updates_url(telegram, telegram->alloc, &url);
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("GET"),
                                       sc_string_as_str(&url),
                                       sc_str_from_cstr(""),
                                       alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_first_update(telegram, sc_string_as_str(&response), alloc, out, &next_offset);
    }
    if (sc_status_is_ok(status) ||
        status.code == SC_ERR_TIMEOUT ||
        telegram_status_consumes_update(status)) {
        telegram->next_update_offset = next_offset;
    }

    sc_string_clear(&response);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    const telegram_channel *telegram = impl;
    if (telegram == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.health_invalid_argument");
    }
    *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
    return sc_string_from_cstr(alloc, "telegram configured", &out->message);
}

static sc_status telegram_request_approval(void *impl,
                                           const sc_channel_approval_request *request,
                                           sc_allocator *alloc,
                                           sc_channel_approval_response *out)
{
    telegram_channel *telegram = impl;
    sc_channel_inbound inbound = {0};
    sc_string approval_message_id = {0};
    time_t deadline = 0;
    char approve_data[64] = {0};
    char deny_data[64] = {0};
    uint64_t approval_id = 0;
    sc_status status = sc_status_ok();
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.approval_invalid_argument");
    }
    if (telegram == nullptr || request == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.approval_invalid_argument");
    }
    approval_id = telegram->next_approval_id == UINT64_MAX ? 1 : telegram->next_approval_id + 1;
    telegram->next_approval_id = approval_id;
    (void)snprintf(approve_data, sizeof(approve_data), "sc:approve:%" PRIu64, approval_id);
    (void)snprintf(deny_data, sizeof(deny_data), "sc:deny:%" PRIu64, approval_id);
    status = telegram_send_approval_message(telegram,
                                            request,
                                            sc_str_from_cstr(approve_data),
                                            sc_str_from_cstr(deny_data),
                                            alloc,
                                            &approval_message_id);
    deadline = time(nullptr) + telegram->approval_timeout_seconds;
    while (sc_status_is_ok(status) && time(nullptr) <= deadline) {
        status = telegram_listen(telegram, alloc, &inbound);
        if (status.code == SC_ERR_TIMEOUT) {
            sc_status_clear(&status);
            status = sc_status_ok();
            continue;
        }
        if (!sc_status_is_ok(status)) {
            break;
        }
        if (sc_str_equal(inbound.sender_id, request->sender_id)) {
            if (sc_str_equal(inbound.text, sc_str_from_cstr(approve_data)) ||
                str_contains_cstr(inbound.text, "approve") || str_contains_cstr(inbound.text, "yes")) {
                *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_APPROVED};
                sc_channel_inbound_clear(&inbound);
                if (approval_message_id.len > 0) {
                    (void)telegram_delete_message(telegram, request->conversation_id, sc_string_as_str(&approval_message_id));
                }
                sc_string_clear(&approval_message_id);
                return sc_string_from_cstr(alloc, "approved by telegram", &out->reason);
            }
            if (sc_str_equal(inbound.text, sc_str_from_cstr(deny_data)) ||
                str_contains_cstr(inbound.text, "deny") || str_contains_cstr(inbound.text, "no")) {
                *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
                sc_channel_inbound_clear(&inbound);
                if (approval_message_id.len > 0) {
                    (void)telegram_delete_message(telegram, request->conversation_id, sc_string_as_str(&approval_message_id));
                }
                sc_string_clear(&approval_message_id);
                return sc_string_from_cstr(alloc, "denied by telegram", &out->reason);
            }
        }
        sc_channel_inbound_clear(&inbound);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&approval_message_id);
        return status;
    }
    sc_string_clear(&approval_message_id);
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
    return sc_string_from_cstr(alloc, "telegram approval timed out", &out->reason);
}

static void telegram_destroy(void *impl)
{
    telegram_channel *telegram = impl;
    if (telegram == nullptr) {
        return;
    }
    sc_string_secure_clear(&telegram->bot_token);
    sc_string_clear(&telegram->api_base_url);
    sc_string_clear(&telegram->bot_username);
    sc_string_clear(&telegram->parse_mode);
    sc_string_clear(&telegram->stream_mode);
    sc_string_clear(&telegram->media_temp_dir);
    sc_string_clear(&telegram->last_draft_chat_id);
    sc_string_clear(&telegram->last_draft_thread_id);
    sc_string_clear(&telegram->last_draft_message_id);
#ifdef SC_HAVE_LIBCURL
    if (telegram->curl != nullptr) {
        curl_easy_cleanup(telegram->curl);
    }
#endif
    sc_free(telegram->alloc, telegram, sizeof(*telegram), _Alignof(telegram_channel));
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out)
{
    return sc_string_from_str(alloc, input.ptr == nullptr ? sc_str_from_cstr("") : input, out);
}

static sc_status copy_i64_string(sc_allocator *alloc, int64_t value, sc_string *out)
{
    char buffer[32] = {0};
    int written = snprintf(buffer, sizeof(buffer), "%" PRId64, value);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_no_memory();
    }
    return sc_string_from_str(alloc, sc_str_from_parts(buffer, (size_t)written), out);
}

static sc_status append_str(sc_string_builder *builder, sc_str input)
{
    return sc_string_builder_append(builder, input);
}

static sc_status append_cstr(sc_string_builder *builder, const char *input)
{
    return sc_string_builder_append(builder, sc_str_from_cstr(input));
}

static sc_status append_i64(sc_string_builder *builder, int64_t value)
{
    char buffer[32] = {0};
    int written = snprintf(buffer, sizeof(buffer), "%" PRId64, value);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_no_memory();
    }
    return sc_string_builder_append(builder, sc_str_from_parts(buffer, (size_t)written));
}

static sc_status append_long(sc_string_builder *builder, long value)
{
    char buffer[32] = {0};
    int written = snprintf(buffer, sizeof(buffer), "%ld", value);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_no_memory();
    }
    return sc_string_builder_append(builder, sc_str_from_parts(buffer, (size_t)written));
}

static sc_status append_urlencoded(sc_string_builder *builder, sc_str input)
{
    static const char hex[] = "0123456789ABCDEF";
    sc_status status = sc_status_ok();

    for (size_t i = 0; sc_status_is_ok(status) && i < input.len; i += 1) {
        unsigned char ch = (unsigned char)input.ptr[i];
        char encoded[3] = {'%', hex[(ch >> 4u) & 0x0Fu], hex[ch & 0x0Fu]};
        if (is_url_unreserved(ch)) {
            char literal = (char)ch;
            status = append_str(builder, sc_str_from_parts(&literal, 1));
        } else if (ch == ' ') {
            status = append_cstr(builder, "%20");
        } else {
            status = append_str(builder, sc_str_from_parts(encoded, sizeof(encoded)));
        }
    }
    return status;
}

static bool is_url_unreserved(unsigned char ch)
{
    return isalnum(ch) != 0 || ch == '-' || ch == '_' || ch == '.' || ch == '~';
}

static sc_status telegram_build_url(const telegram_channel *telegram, const char *method, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_str base = sc_string_as_str(&telegram->api_base_url);

    while (base.len > 0 && base.ptr[base.len - 1] == '/') {
        base.len -= 1;
    }

    sc_string_builder_init(&builder, alloc);
    status = append_str(&builder, base);
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "/bot");
    }
    if (sc_status_is_ok(status)) {
        status = append_str(&builder, sc_string_as_str(&telegram->bot_token));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, method);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_get_updates_url(const telegram_channel *telegram, sc_allocator *alloc, sc_string *out)
{
    sc_string base = {0};
    sc_string_builder builder = {0};
    sc_status status = telegram_build_url(telegram, "getUpdates", alloc, &base);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_string_builder_init(&builder, alloc);
    status = append_str(&builder, sc_string_as_str(&base));
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "?timeout=");
    }
    if (sc_status_is_ok(status)) {
        status = append_long(&builder, telegram->poll_timeout_seconds);
    }
    if (sc_status_is_ok(status) && telegram->next_update_offset > 0) {
        status = append_cstr(&builder, "&offset=");
    }
    if (sc_status_is_ok(status) && telegram->next_update_offset > 0) {
        status = append_i64(&builder, telegram->next_update_offset);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&base);
    return status;
}

static sc_status telegram_build_get_file_url(const telegram_channel *telegram, sc_str file_id, sc_allocator *alloc, sc_string *out)
{
    sc_string base = {0};
    sc_string_builder builder = {0};
    sc_status status = telegram_build_url(telegram, "getFile", alloc, &base);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_string_builder_init(&builder, alloc);
    status = append_str(&builder, sc_string_as_str(&base));
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "?file_id=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, file_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&base);
    return status;
}

static sc_status telegram_build_download_file_url(const telegram_channel *telegram, sc_str file_path, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_str base = telegram == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&telegram->api_base_url);
    sc_status status = sc_status_ok();

    while (base.len > 0 && base.ptr[base.len - 1] == '/') {
        base.len -= 1;
    }

    sc_string_builder_init(&builder, alloc);
    status = append_str(&builder, base);
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "/file/bot");
    }
    if (sc_status_is_ok(status)) {
        status = append_str(&builder, sc_string_as_str(&telegram->bot_token));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = append_str(&builder, file_path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_send_body(const telegram_channel *telegram,
                                          const sc_channel_message *message,
                                          sc_str text,
                                          sc_allocator *alloc,
                                          sc_string *out)
{
    sc_string_builder builder = {0};
    sc_string formatted = {0};
    sc_str parse_mode = sc_str_from_cstr("");
    sc_status status = sc_status_ok();

    status = telegram_format_outbound_text(telegram, text, alloc, &formatted, &parse_mode);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&text=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_string_as_str(&formatted));
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_cstr(&builder, "&parse_mode=");
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_urlencoded(&builder, parse_mode);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_cstr(&builder, "&reply_parameters=");
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("{\"message_id\":"));
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, message->reply_to_message_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("}"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&formatted);
    return status;
}

static sc_status telegram_send_approval_message(telegram_channel *telegram,
                                                const sc_channel_approval_request *request,
                                                sc_str approve_data,
                                                sc_str deny_data,
                                                sc_allocator *alloc,
                                                sc_string *out_message_id)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || request == nullptr || out_message_id == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.approval_invalid_argument");
    }
    status = telegram_build_url(telegram, "sendMessage", telegram->alloc, &url);
    if (sc_status_is_ok(status)) {
        status = telegram_build_approval_body(telegram, request, approve_data, deny_data, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    if (sc_status_is_ok(status)) {
        sc_status message_status = telegram_parse_message_id(sc_string_as_str(&response), alloc, out_message_id);
        if (!sc_status_is_ok(message_status)) {
            sc_status_clear(&message_status);
        }
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_build_approval_body(const telegram_channel *telegram,
                                              const sc_channel_approval_request *request,
                                              sc_str approve_data,
                                              sc_str deny_data,
                                              sc_allocator *alloc,
                                              sc_string *out)
{
    sc_string_builder builder = {0};
    sc_string formatted = {0};
    sc_str parse_mode = sc_str_from_cstr("");
    sc_status status = sc_status_ok();

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.approval_body_invalid_argument");
    }
    status = telegram_format_outbound_text(telegram, request->summary, alloc, &formatted, &parse_mode);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, request->conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&text=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_string_as_str(&formatted));
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_cstr(&builder, "&parse_mode=");
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_urlencoded(&builder, parse_mode);
    }
    if (sc_status_is_ok(status) && request->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && request->thread_id.len > 0) {
        status = append_urlencoded(&builder, request->thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&reply_markup=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder,
                                   sc_str_from_cstr("{\"inline_keyboard\":[[{\"text\":\"Approve\",\"callback_data\":\""));
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, approve_data);
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_str_from_cstr("\"},{\"text\":\"Deny\",\"callback_data\":\""));
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, deny_data);
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_str_from_cstr("\"}]]}"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&formatted);
    return status;
}

static sc_status telegram_delete_message(telegram_channel *telegram, sc_str chat_id, sc_str message_id)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || chat_id.len == 0 || message_id.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.delete_message_invalid_argument");
    }
    status = telegram_build_url(telegram, "deleteMessage", telegram->alloc, &url);
    if (sc_status_is_ok(status)) {
        status = telegram_build_delete_message_body(chat_id, message_id, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_build_delete_message_body(sc_str chat_id,
                                                    sc_str message_id,
                                                    sc_allocator *alloc,
                                                    sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || chat_id.len == 0 || message_id.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.delete_message_body_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, chat_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&message_id=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_message_draft_body(const telegram_channel *telegram,
                                                   const sc_channel_message *message,
                                                   sc_str text,
                                                   long draft_id,
                                                   sc_allocator *alloc,
                                                   sc_string *out)
{
    sc_string_builder builder = {0};
    sc_string formatted = {0};
    sc_str parse_mode = sc_str_from_cstr("");
    sc_status status = sc_status_ok();

    status = telegram_format_outbound_text(telegram, text, alloc, &formatted, &parse_mode);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&draft_id=");
    }
    if (sc_status_is_ok(status)) {
        status = append_long(&builder, draft_id <= 0 ? 1 : draft_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&text=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_string_as_str(&formatted));
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_cstr(&builder, "&parse_mode=");
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_urlencoded(&builder, parse_mode);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&formatted);
    return status;
}

static sc_status telegram_build_edit_body(const telegram_channel *telegram,
                                          const sc_channel_message *message,
                                          sc_str text,
                                          sc_allocator *alloc,
                                          sc_string *out)
{
    sc_string_builder builder = {0};
    sc_string formatted = {0};
    sc_str parse_mode = sc_str_from_cstr("");
    sc_status status = sc_status_ok();

    status = telegram_format_outbound_text(telegram, text, alloc, &formatted, &parse_mode);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&message_id=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_string_as_str(&telegram->last_draft_message_id));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&text=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_string_as_str(&formatted));
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_cstr(&builder, "&parse_mode=");
    }
    if (sc_status_is_ok(status) && parse_mode.len > 0) {
        status = append_urlencoded(&builder, parse_mode);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&formatted);
    return status;
}

static sc_status telegram_build_chat_action_body(const sc_channel_message *message,
                                                 sc_str action,
                                                 sc_allocator *alloc,
                                                 sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&action=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, action);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_reaction_body(const telegram_update *update, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (update == nullptr || out == nullptr || update->message_id <= 0) {
        return sc_status_invalid_argument("sc.channel_telegram.reaction_invalid_argument");
    }

    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_i64(&builder, update->chat_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&message_id=");
    }
    if (sc_status_is_ok(status)) {
        status = append_i64(&builder, update->message_id);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&reaction=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, sc_str_from_cstr("[{\"type\":\"emoji\",\"emoji\":\"\\uD83D\\uDC4D\"}]"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_audio_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (message == nullptr || message->attachment_bytes.ptr == nullptr || message->attachment_bytes.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.audio_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_cstr(&builder, "&reply_parameters=");
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("{\"message_id\":"));
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, message->reply_to_message_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("}"));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&audio_filename=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->attachment_filename.len == 0 ? sc_str_from_cstr("smolclaw-reply.wav") : message->attachment_filename);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&audio_content_type=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->attachment_content_type);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&audio_bytes=");
    }
    if (sc_status_is_ok(status)) {
        status = append_long(&builder, (long)message->attachment_bytes.len);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_voice_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (message == nullptr || message->attachment_bytes.ptr == nullptr || message->attachment_bytes.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.voice_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_cstr(&builder, "&reply_parameters=");
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("{\"message_id\":"));
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, message->reply_to_message_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("}"));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&voice_filename=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->attachment_filename.len == 0 ? sc_str_from_cstr("smolclaw-reply.ogg") : message->attachment_filename);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&voice_content_type=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->attachment_content_type.len == 0 ? sc_str_from_cstr("audio/ogg") : message->attachment_content_type);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&voice_bytes=");
    }
    if (sc_status_is_ok(status)) {
        status = append_long(&builder, (long)message->attachment_bytes.len);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&duration=0");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_photo_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (message == nullptr || message->attachment_bytes.ptr == nullptr || message->attachment_bytes.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.photo_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_cstr(&builder, "&reply_parameters=");
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("{\"message_id\":"));
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, message->reply_to_message_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("}"));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&photo_filename=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->attachment_filename.len == 0 ? sc_str_from_cstr("browser-screenshot.png") : message->attachment_filename);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&photo_content_type=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->attachment_content_type.len == 0 ? sc_str_from_cstr("image/png") : message->attachment_content_type);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&photo_bytes=");
    }
    if (sc_status_is_ok(status)) {
        status = append_long(&builder, (long)message->attachment_bytes.len);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_build_document_body(const sc_channel_message *message, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (message == nullptr || message->attachment_bytes.ptr == nullptr || message->attachment_bytes.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.document_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = append_cstr(&builder, "chat_id=");
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder, message->conversation_id);
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_cstr(&builder, "&message_thread_id=");
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        status = append_urlencoded(&builder, message->thread_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_cstr(&builder, "&reply_parameters=");
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("{\"message_id\":"));
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, message->reply_to_message_id);
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = append_urlencoded(&builder, sc_str_from_cstr("}"));
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&document_filename=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder,
                                   message->attachment_filename.len == 0 ? sc_str_from_cstr("smolclaw-attachment.bin") :
                                                                           message->attachment_filename);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&document_content_type=");
    }
    if (sc_status_is_ok(status)) {
        status = append_urlencoded(&builder,
                                   message->attachment_content_type.len == 0 ? sc_str_from_cstr("application/octet-stream") :
                                                                                message->attachment_content_type);
    }
    if (sc_status_is_ok(status)) {
        status = append_cstr(&builder, "&document_bytes=");
    }
    if (sc_status_is_ok(status)) {
        status = append_long(&builder, (long)message->attachment_bytes.len);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status telegram_send_attachment(telegram_channel *telegram, const sc_channel_message *message)
{
    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.attachment_invalid_argument");
    }
    if (message->attachment_delivery == SC_ATTACHMENT_DELIVERY_DOCUMENT) {
        return telegram_send_document(telegram, message);
    }
    if (telegram_attachment_is_image(message)) {
        return telegram_send_photo(telegram, message);
    }
    if (sc_media_content_type_is_audio(message->attachment_content_type)) {
        return telegram_send_audio(telegram, message);
    }
    return telegram_send_document(telegram, message);
}

static sc_status telegram_send_audio(telegram_channel *telegram, const sc_channel_message *message)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.audio_invalid_argument");
    }
    if (telegram_attachment_is_wav(message)) {
        return telegram_send_voice_from_wav(telegram, message);
    }
    status = telegram_build_url(telegram, "sendAudio", telegram->alloc, &url);
    if (sc_status_is_ok(status) && telegram->http_request == nullptr) {
#ifdef SC_HAVE_LIBCURL
        status = telegram_curl_send_audio_multipart(telegram,
                                                    sc_string_as_str(&url),
                                                    message,
                                                    sc_str_from_cstr("audio"),
                                                    sc_str_from_cstr("smolclaw-reply.wav"),
                                                    sc_str_from_cstr("audio/wav"),
                                                    telegram->alloc,
                                                    &response);
#else
        status = sc_status_unsupported("sc.channel_telegram.libcurl_unavailable");
#endif
    } else if (sc_status_is_ok(status)) {
        status = telegram_build_audio_body(message, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status) && telegram->http_request != nullptr) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_send_photo(telegram_channel *telegram, const sc_channel_message *message)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();
    bool fallback_to_document = false;

    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.photo_invalid_argument");
    }
    status = telegram_build_url(telegram, "sendPhoto", telegram->alloc, &url);
    if (sc_status_is_ok(status) && telegram->http_request == nullptr) {
#ifdef SC_HAVE_LIBCURL
        status = telegram_curl_send_audio_multipart(telegram,
                                                    sc_string_as_str(&url),
                                                    message,
                                                    sc_str_from_cstr("photo"),
                                                    sc_str_from_cstr("browser-screenshot.png"),
                                                    sc_str_from_cstr("image/png"),
                                                    telegram->alloc,
                                                    &response);
#else
        status = sc_status_unsupported("sc.channel_telegram.libcurl_unavailable");
#endif
    } else if (sc_status_is_ok(status)) {
        status = telegram_build_photo_body(message, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status) && telegram->http_request != nullptr) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    fallback_to_document = telegram_photo_status_allows_document_fallback(status);
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    if (fallback_to_document) {
        sc_status_clear(&status);
        status = telegram_send_document(telegram, message);
    }
    return status;
}

static sc_status telegram_send_document(telegram_channel *telegram, const sc_channel_message *message)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.document_invalid_argument");
    }
    status = telegram_build_url(telegram, "sendDocument", telegram->alloc, &url);
    if (sc_status_is_ok(status) && telegram->http_request == nullptr) {
#ifdef SC_HAVE_LIBCURL
        status = telegram_curl_send_audio_multipart(telegram,
                                                    sc_string_as_str(&url),
                                                    message,
                                                    sc_str_from_cstr("document"),
                                                    sc_str_from_cstr("smolclaw-attachment.bin"),
                                                    sc_str_from_cstr("application/octet-stream"),
                                                    telegram->alloc,
                                                    &response);
#else
        status = sc_status_unsupported("sc.channel_telegram.libcurl_unavailable");
#endif
    } else if (sc_status_is_ok(status)) {
        status = telegram_build_document_body(message, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status) && telegram->http_request != nullptr) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_send_voice_from_wav(telegram_channel *telegram, const sc_channel_message *message)
{
    sc_bytes opus = {0};
    sc_channel_message voice = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.voice_invalid_argument");
    }

    status = sc_media_wav_to_ogg_opus(telegram->alloc, message->attachment_bytes, telegram->max_media_bytes, &opus);
    if (sc_status_is_ok(status)) {
        voice = *message;
        voice.attachment_content_type = sc_str_from_cstr("audio/ogg");
        voice.attachment_filename = sc_str_from_cstr("smolclaw-reply.ogg");
        voice.attachment_bytes = sc_buf_from_parts(opus.ptr, opus.len);
        status = telegram_send_voice(telegram, &voice);
    }
    sc_bytes_clear(&opus);
    return status;
}

static sc_status telegram_send_voice(telegram_channel *telegram, const sc_channel_message *message)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.voice_invalid_argument");
    }
    status = telegram_build_url(telegram, "sendVoice", telegram->alloc, &url);
    if (sc_status_is_ok(status) && telegram->http_request == nullptr) {
#ifdef SC_HAVE_LIBCURL
        status = telegram_curl_send_audio_multipart(telegram,
                                                    sc_string_as_str(&url),
                                                    message,
                                                    sc_str_from_cstr("voice"),
                                                    sc_str_from_cstr("smolclaw-reply.ogg"),
                                                    sc_str_from_cstr("audio/ogg"),
                                                    telegram->alloc,
                                                    &response);
#else
        status = sc_status_unsupported("sc.channel_telegram.libcurl_unavailable");
#endif
    } else if (sc_status_is_ok(status)) {
        status = telegram_build_voice_body(message, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status) && telegram->http_request != nullptr) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_send_chat_action(telegram_channel *telegram,
                                           const sc_channel_message *message,
                                           sc_str action)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.chat_action_invalid_argument");
    }
    status = telegram_build_url(telegram, "sendChatAction", telegram->alloc, &url);
    if (sc_status_is_ok(status)) {
        status = telegram_build_chat_action_body(message, action, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static sc_status telegram_send_reaction(telegram_channel *telegram, const telegram_update *update)
{
    sc_string url = {0};
    sc_string body = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr || update == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.reaction_invalid_argument");
    }
    status = telegram_build_url(telegram, "setMessageReaction", telegram->alloc, &url);
    if (sc_status_is_ok(status)) {
        status = telegram_build_reaction_body(update, telegram->alloc, &body);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("POST"),
                                       sc_string_as_str(&url),
                                       sc_string_as_str(&body),
                                       telegram->alloc,
                                       &response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_ok_response(sc_string_as_str(&response));
    }
    sc_string_clear(&response);
    sc_string_clear(&body);
    sc_string_clear(&url);
    return status;
}

static void telegram_log_reaction_failure(sc_status status, const telegram_update *update)
{
    char chat_id[32] = {0};
    char message_id[32] = {0};
    sc_log_field fields[3] = {0};

    if (sc_status_is_ok(status) || update == nullptr) {
        return;
    }

    (void)snprintf(chat_id, sizeof(chat_id), "%" PRId64, update->chat_id);
    (void)snprintf(message_id, sizeof(message_id), "%" PRId64, update->message_id);
    fields[0] = (sc_log_field){.key = "chat_id", .value = sc_str_from_cstr(chat_id), .secret = false};
    fields[1] = (sc_log_field){.key = "message_id", .value = sc_str_from_cstr(message_id), .secret = false};
    fields[2] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
    sc_log_write(SC_LOG_DEBUG, "sc.channel", "channel.telegram.reaction_failed", fields, SC_ARRAY_LEN(fields));
}

static sc_status telegram_format_outbound_text(const telegram_channel *telegram,
                                               sc_str text,
                                               sc_allocator *alloc,
                                               sc_string *formatted,
                                               sc_str *parse_mode)
{
    if (formatted == nullptr || parse_mode == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.format_invalid_argument");
    }
    *parse_mode = telegram == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&telegram->parse_mode);

    if (telegram_should_render_markdown(telegram)) {
#ifdef SC_HAVE_LIBCMARK
        sc_status status = telegram_markdown_to_html(text, alloc, formatted);
        if (sc_status_is_ok(status)) {
            *parse_mode = sc_str_from_cstr("HTML");
        }
        return status;
#else
        return sc_string_from_str(alloc, text.ptr == nullptr ? sc_str_from_cstr("") : text, formatted);
#endif
    }
    return sc_string_from_str(alloc, text.ptr == nullptr ? sc_str_from_cstr("") : text, formatted);
}

static sc_status telegram_next_text_chunk(const telegram_channel *telegram,
                                          sc_str text,
                                          size_t offset,
                                          sc_allocator *alloc,
                                          sc_str *out)
{
    sc_string formatted = {0};
    sc_str parse_mode = {0};
    size_t remaining = 0;
    size_t limit = telegram_text_safe_limit_bytes;
    size_t chunk_len = 0;

    if (telegram == nullptr || out == nullptr || text.ptr == nullptr || offset > text.len) {
        return sc_status_invalid_argument("sc.channel_telegram.chunk_invalid_argument");
    }

    remaining = text.len - offset;
    if (remaining == 0) {
        *out = sc_str_from_cstr("");
        return sc_status_ok();
    }

    if (telegram->message_split_bytes > 0 && telegram->message_split_bytes < limit) {
        limit = telegram->message_split_bytes;
    }
    chunk_len = remaining < limit ? remaining : limit;
    chunk_len = telegram_utf8_chunk_boundary(text, offset, chunk_len);

    while (chunk_len > 1) {
        sc_str chunk = sc_str_from_parts(text.ptr + offset, chunk_len);

        sc_status status = telegram_format_outbound_text(telegram, chunk, alloc, &formatted, &parse_mode);
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&formatted);
            return status;
        }
        if (formatted.len <= limit) {
            sc_string_clear(&formatted);
            *out = chunk;
            return sc_status_ok();
        }
        sc_string_clear(&formatted);
        chunk_len = telegram_utf8_chunk_boundary(text, offset, chunk_len / 2U);
    }

    *out = sc_str_from_parts(text.ptr + offset, chunk_len == 0 ? 1 : chunk_len);
    return sc_status_ok();
}

static size_t telegram_utf8_chunk_boundary(sc_str text, size_t offset, size_t len)
{
    size_t adjusted = len;

    if (text.ptr == nullptr || offset >= text.len || adjusted > text.len - offset) {
        return 0;
    }
    while (adjusted > 0 && offset + adjusted < text.len &&
           (((unsigned char)text.ptr[offset + adjusted] & 0xC0U) == 0x80U)) {
        adjusted -= 1;
    }
    return adjusted == 0 ? len : adjusted;
}

static bool telegram_should_render_markdown(const telegram_channel *telegram)
{
    sc_str parse_mode;
    if (telegram == nullptr) {
        return false;
    }
    parse_mode = sc_string_as_str(&telegram->parse_mode);
    return parse_mode.len == 0 || sc_str_case_equal_cstr(parse_mode, "HTML");
}

static bool sc_str_case_equal_cstr(sc_str left, const char *right)
{
    size_t i = 0;
    if (right == nullptr || left.ptr == nullptr) {
        return false;
    }
    while (i < left.len && right[i] != '\0') {
        if (tolower((unsigned char)left.ptr[i]) != tolower((unsigned char)right[i])) {
            return false;
        }
        i += 1;
    }
    return i == left.len && right[i] == '\0';
}

#ifdef SC_HAVE_LIBCMARK
static sc_status telegram_markdown_to_html(sc_str markdown, sc_allocator *alloc, sc_string *out)
{
    char *html = nullptr;
    sc_status status = sc_status_ok();
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.markdown_invalid_argument");
    }
    if (markdown.ptr == nullptr) {
        return sc_string_from_cstr(alloc, "", out);
    }
    html = cmark_markdown_to_html(markdown.ptr, markdown.len, CMARK_OPT_DEFAULT);
    if (html == nullptr) {
        return sc_status_no_memory();
    }
    status = telegram_filter_cmark_html(sc_str_from_cstr(html), alloc, out);
    cmark_get_default_mem_allocator()->free(html);
    return status;
}

static sc_status telegram_filter_cmark_html(sc_str html, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < html.len;) {
        if (html.ptr[i] == '<') {
            size_t end = i + 1;
            while (end < html.len && html.ptr[end] != '>') {
                end += 1;
            }
            if (end >= html.len) {
                status = append_str(&builder, sc_str_from_parts(html.ptr + i, html.len - i));
                break;
            }
            sc_str tag = sc_str_from_parts(html.ptr + i, end - i + 1);
            if (telegram_html_tag_allowed(tag)) {
                status = append_str(&builder, tag);
            } else if (tag.len >= 3 && tag.ptr[1] == '/' &&
                       (tag.ptr[2] == 'p' || tag.ptr[2] == 'h' || tag.ptr[2] == 'l')) {
                status = append_cstr(&builder, "\n");
            }
            i = end + 1;
        } else {
            status = append_str(&builder, sc_str_from_parts(html.ptr + i, 1));
            i += 1;
        }
    }
    while (sc_status_is_ok(status) && builder.bytes.len > 0 && builder.bytes.ptr[builder.bytes.len - 1] == '\n') {
        builder.bytes.len -= 1;
        builder.bytes.ptr[builder.bytes.len] = '\0';
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool telegram_html_tag_allowed(sc_str tag)
{
    static const char *allowed[] = {
        "b",      "strong", "i",    "em",  "u",   "ins", "s", "strike",
        "del",    "code",   "pre",  "a",   "span", "tg-spoiler",
        "blockquote",
    };
    size_t name_start = 1;
    size_t name_end = 0;

    if (tag.len < 3 || tag.ptr[0] != '<' || tag.ptr[tag.len - 1] != '>') {
        return false;
    }
    if (tag.ptr[name_start] == '/') {
        name_start += 1;
    }
    name_end = name_start;
    while (name_end < tag.len && tag.ptr[name_end] != '>' && tag.ptr[name_end] != ' ' && tag.ptr[name_end] != '\t' &&
           tag.ptr[name_end] != '\r' && tag.ptr[name_end] != '\n') {
        name_end += 1;
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(allowed); i += 1) {
        if (sc_str_case_equal_cstr(sc_str_from_parts(tag.ptr + name_start, name_end - name_start), allowed[i])) {
            return true;
        }
    }
    return false;
}
#endif

static sc_status telegram_http_request(telegram_channel *telegram,
                                       sc_str method,
                                       sc_str url,
                                       sc_str body,
                                       sc_allocator *alloc,
                                       sc_string *out)
{
    sc_string redacted_url = {0};
    sc_str api_method = telegram_api_method_from_url(url);
    sc_status status = sc_status_ok();
    char body_bytes[32] = {0};
    char response_bytes[32] = {0};
    char attempt_text[32] = {0};
    char max_attempts_text[32] = {0};
    char backoff_text[32] = {0};
    sc_log_field start_fields[5] = {0};
    sc_log_field retry_fields[8] = {0};
    sc_log_field done_fields[9] = {0};
    uint32_t max_attempts = telegram == nullptr ? 1U : telegram->max_retries + 1U;
    uint32_t attempt = 0;

    (void)snprintf(body_bytes, sizeof(body_bytes), "%zu", body.len);
    (void)snprintf(max_attempts_text, sizeof(max_attempts_text), "%u", max_attempts);
    (void)telegram_redact_url_for_log(alloc == nullptr ? sc_allocator_heap() : alloc, url, &redacted_url);
    start_fields[0] = (sc_log_field){.key = "transport", .value = sc_str_from_cstr(telegram->http_request == nullptr ? "curl" : "custom"), .secret = false};
    start_fields[1] = (sc_log_field){.key = "method", .value = method, .secret = false};
    start_fields[2] = (sc_log_field){.key = "telegram_method", .value = api_method, .secret = false};
    start_fields[3] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
    start_fields[4] = (sc_log_field){.key = "body_bytes", .value = sc_str_from_cstr(body_bytes), .secret = false};
    sc_log_write(SC_LOG_TRACE, "sc.net", "net.telegram.request.start", start_fields, SC_ARRAY_LEN(start_fields));

    for (attempt = 1; attempt <= max_attempts; attempt += 1) {
        if (out != nullptr) {
            sc_string_clear(out);
        }
        if (telegram->http_request != nullptr) {
            status = telegram->http_request(telegram->http_user, method, url, body, alloc, out);
        } else {
#ifdef SC_HAVE_LIBCURL
            status = telegram_curl_request(telegram, method, url, body, alloc, out);
#else
            (void)url;
            status = sc_status_unsupported("sc.channel_telegram.libcurl_unavailable");
#endif
        }
        if (sc_status_is_ok(status) || attempt >= max_attempts || telegram->http_request == nullptr) {
            break;
        }
        (void)snprintf(attempt_text, sizeof(attempt_text), "%u", attempt);
        (void)snprintf(backoff_text, sizeof(backoff_text), "%u", telegram->retry_backoff_ms * attempt);
        retry_fields[0] = (sc_log_field){.key = "transport", .value = sc_str_from_cstr("custom"), .secret = false};
        retry_fields[1] = (sc_log_field){.key = "method", .value = method, .secret = false};
        retry_fields[2] = (sc_log_field){.key = "telegram_method", .value = api_method, .secret = false};
        retry_fields[3] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
        retry_fields[4] = (sc_log_field){.key = "attempt", .value = sc_str_from_cstr(attempt_text), .secret = false};
        retry_fields[5] = (sc_log_field){.key = "max_attempts", .value = sc_str_from_cstr(max_attempts_text), .secret = false};
        retry_fields[6] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
        retry_fields[7] = (sc_log_field){.key = "backoff_ms", .value = sc_str_from_cstr(backoff_text), .secret = false};
        sc_log_write(SC_LOG_TRACE, "sc.net", "net.telegram.request.retry", retry_fields, SC_ARRAY_LEN(retry_fields));
        sleep_ms(telegram->retry_backoff_ms * attempt);
    }

    (void)snprintf(attempt_text, sizeof(attempt_text), "%u", attempt);
    (void)snprintf(response_bytes, sizeof(response_bytes), "%zu", out == nullptr ? 0U : out->len);
    done_fields[0] = (sc_log_field){.key = "transport", .value = sc_str_from_cstr(telegram->http_request == nullptr ? "curl" : "custom"), .secret = false};
    done_fields[1] = (sc_log_field){.key = "method", .value = method, .secret = false};
    done_fields[2] = (sc_log_field){.key = "telegram_method", .value = api_method, .secret = false};
    done_fields[3] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
    done_fields[4] = (sc_log_field){.key = "response_bytes", .value = sc_str_from_cstr(response_bytes), .secret = false};
    done_fields[5] = (sc_log_field){.key = "status", .value = sc_str_from_cstr(sc_status_is_ok(status) ? "ok" : "error"), .secret = false};
    done_fields[6] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
    done_fields[7] = (sc_log_field){.key = "attempts", .value = sc_str_from_cstr(attempt_text), .secret = false};
    done_fields[8] = (sc_log_field){.key = "max_attempts", .value = sc_str_from_cstr(max_attempts_text), .secret = false};
    sc_log_write(SC_LOG_TRACE, "sc.net", "net.telegram.request.done", done_fields, SC_ARRAY_LEN(done_fields));
    sc_string_clear(&redacted_url);
    return status;
}

static sc_status telegram_redact_url_for_log(sc_allocator *alloc, sc_str url, sc_string *out)
{
    static const char marker[] = "[REDACTED]";
    sc_string_builder builder = {0};
    size_t authority_start = 0;
    size_t authority_end = 0;
    size_t at = SIZE_MAX;
    size_t bot = SIZE_MAX;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.log_url_invalid_argument");
    }
    if (url.ptr == nullptr) {
        return sc_string_from_cstr(alloc, "", out);
    }

    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; i + 2 < url.len; i += 1) {
        if (url.ptr[i] == ':' && url.ptr[i + 1] == '/' && url.ptr[i + 2] == '/') {
            authority_start = i + 3;
            break;
        }
    }
    authority_end = authority_start;
    while (authority_end < url.len && url.ptr[authority_end] != '/' && url.ptr[authority_end] != '?' && url.ptr[authority_end] != '#') {
        if (url.ptr[authority_end] == '@') {
            at = authority_end;
        }
        authority_end += 1;
    }

    for (size_t i = 0; i + 3 < url.len; i += 1) {
        if (url.ptr[i] == '/' && url.ptr[i + 1] == 'b' && url.ptr[i + 2] == 'o' && url.ptr[i + 3] == 't') {
            bot = i;
            break;
        }
    }

    if (at != SIZE_MAX) {
        status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr, authority_start));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, marker);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr + at, bot == SIZE_MAX ? url.len - at : bot - at));
        }
    } else {
        status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr, bot == SIZE_MAX ? url.len : bot));
    }

    if (sc_status_is_ok(status) && bot != SIZE_MAX) {
        size_t after_token = bot + 4;
        while (after_token < url.len && url.ptr[after_token] != '/' && url.ptr[after_token] != '?' && url.ptr[after_token] != '#') {
            after_token += 1;
        }
        status = sc_string_builder_append_cstr(&builder, "/bot");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, marker);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr + after_token, url.len - after_token));
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

static sc_str telegram_api_method_from_url(sc_str url)
{
    size_t start = SIZE_MAX;
    size_t end = 0;

    if (url.ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    for (size_t i = 0; i + 3 < url.len; i += 1) {
        if (url.ptr[i] == '/' && url.ptr[i + 1] == 'b' && url.ptr[i + 2] == 'o' && url.ptr[i + 3] == 't') {
            start = i + 4;
            break;
        }
    }
    if (start == SIZE_MAX) {
        return sc_str_from_cstr("");
    }
    while (start < url.len && url.ptr[start] != '/') {
        start += 1;
    }
    if (start >= url.len || url.ptr[start] != '/') {
        return sc_str_from_cstr("");
    }
    start += 1;
    end = start;
    while (end < url.len && url.ptr[end] != '?' && url.ptr[end] != '#' && url.ptr[end] != '/') {
        end += 1;
    }
    return sc_str_from_parts(url.ptr + start, end - start);
}

static sc_status telegram_parse_ok_response(sc_str json)
{
    JSON_Value *root_value = nullptr;
    JSON_Object *root = nullptr;
    sc_status status = sc_status_ok();

    if (json.ptr == nullptr) {
        return sc_status_parse("sc.channel_telegram.invalid_json");
    }
    root_value = json_parse_string(json.ptr);
    root = root_value == nullptr ? nullptr : json_value_get_object(root_value);
    if (root == nullptr) {
        status = sc_status_parse("sc.channel_telegram.invalid_json");
    } else if (json_object_get_boolean(root, "ok") != JSONBooleanTrue) {
        status = sc_status_http("sc.channel_telegram.api_error");
    }
    json_value_free(root_value);
    return status;
}

static sc_status telegram_parse_message_id(sc_str json, sc_allocator *alloc, sc_string *out)
{
    JSON_Value *root_value = nullptr;
    JSON_Object *root = nullptr;
    JSON_Object *result = nullptr;
    int64_t message_id = 0;
    sc_status status = sc_status_ok();

    if (json.ptr == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.message_id_invalid_argument");
    }
    root_value = json_parse_string(json.ptr);
    root = root_value == nullptr ? nullptr : json_value_get_object(root_value);
    result = root == nullptr ? nullptr : json_object_get_object(root, "result");
    if (result == nullptr || !json_i64(result, "message_id", &message_id)) {
        json_value_free(root_value);
        return sc_status_parse("sc.channel_telegram.message_id_missing");
    }
    status = copy_i64_string(alloc, message_id, out);
    json_value_free(root_value);
    return status;
}

static sc_status telegram_parse_file_path(sc_str json, sc_allocator *alloc, sc_string *out)
{
    JSON_Value *root_value = nullptr;
    JSON_Object *root = nullptr;
    JSON_Object *result = nullptr;
    const char *file_path = nullptr;
    sc_status status = sc_status_ok();

    if (json.ptr == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.file_path_invalid_argument");
    }
    root_value = json_parse_string(json.ptr);
    root = root_value == nullptr ? nullptr : json_value_get_object(root_value);
    result = root == nullptr ? nullptr : json_object_get_object(root, "result");
    file_path = result == nullptr ? nullptr : json_object_get_string(result, "file_path");
    if (root == nullptr || json_object_get_boolean(root, "ok") != JSONBooleanTrue || file_path == nullptr || file_path[0] == '\0') {
        json_value_free(root_value);
        return sc_status_parse("sc.channel_telegram.file_path_missing");
    }
    status = sc_string_from_cstr(alloc, file_path, out);
    json_value_free(root_value);
    return status;
}

static sc_status telegram_parse_first_update(telegram_channel *telegram,
                                             sc_str json,
                                             sc_allocator *alloc,
                                             sc_channel_inbound *out,
                                             int64_t *next_offset)
{
    JSON_Value *root_value = nullptr;
    JSON_Object *root = nullptr;
    JSON_Array *result = nullptr;
    sc_status status = sc_status_timeout("sc.channel_telegram.no_updates");
    int64_t latest_seen = 0;

    if (telegram == nullptr || json.ptr == nullptr || out == nullptr || next_offset == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.parse_invalid_argument");
    }
    latest_seen = *next_offset;

    root_value = json_parse_string(json.ptr);
    root = root_value == nullptr ? nullptr : json_value_get_object(root_value);
    if (root == nullptr || json_object_get_boolean(root, "ok") != JSONBooleanTrue) {
        json_value_free(root_value);
        return sc_status_parse("sc.channel_telegram.invalid_updates_json");
    }

    result = json_object_get_array(root, "result");
    if (result == nullptr) {
        json_value_free(root_value);
        return sc_status_parse("sc.channel_telegram.invalid_updates_json");
    }

    for (size_t i = 0; i < json_array_get_count(result); i += 1) {
        JSON_Object *update_obj = json_array_get_object(result, i);
        telegram_update update = {0};
        if (update_obj != nullptr && json_i64(update_obj, "update_id", &latest_seen) && telegram_extract_update(telegram, update_obj, &update)) {
            if (!telegram_update_visible(telegram, &update)) {
                *next_offset = update.update_id + 1;
                continue;
            }
            status = telegram_check_rate_limit(telegram);
            if (sc_status_is_ok(status)) {
                status = telegram_update_to_inbound(telegram, &update, alloc, out);
            }
            if (sc_status_is_ok(status) && telegram->ack_reactions && (!update.post || telegram->post_reactions)) {
                sc_status reaction_status = telegram_send_reaction(telegram, &update);
                if (!sc_status_is_ok(reaction_status)) {
                    telegram_log_reaction_failure(reaction_status, &update);
                    sc_status_clear(&reaction_status);
                }
            }
            if (sc_status_is_ok(status)) {
                *next_offset = update.update_id + 1;
            } else if (telegram_status_consumes_update(status)) {
                *next_offset = update.update_id + 1;
            }
            json_value_free(root_value);
            return status;
        }
    }

    if (latest_seen > *next_offset) {
        *next_offset = latest_seen + 1;
    }
    json_value_free(root_value);
    return status;
}

static bool telegram_extract_update(const telegram_channel *telegram, JSON_Object *update, telegram_update *out)
{
    JSON_Object *message = nullptr;
    JSON_Object *callback_query = nullptr;
    JSON_Object *reply_to_message = nullptr;
    JSON_Object *chat = nullptr;
    JSON_Object *from = nullptr;
    JSON_Object *sender_chat = nullptr;
    JSON_Object *document = nullptr;
    JSON_Object *video = nullptr;
    JSON_Object *voice = nullptr;
    JSON_Object *audio = nullptr;
    const JSON_Array *photos = nullptr;
    JSON_Object *photo = nullptr;
    double file_size = 0.0;

    (void)telegram;

    if (update == nullptr || out == nullptr || !json_i64(update, "update_id", &out->update_id)) {
        return false;
    }

    callback_query = json_object_get_object(update, "callback_query");
    if (callback_query != nullptr) {
        message = json_object_get_object(callback_query, "message");
        from = json_object_get_object(callback_query, "from");
        out->callback = true;
        out->callback_query_id = json_object_get_string(callback_query, "id");
        out->callback_data = json_object_get_string(callback_query, "data");
        out->text = out->callback_data;
    } else {
        message = json_object_get_object(update, "message");
    }
    if (message == nullptr) {
        message = json_object_get_object(update, "edited_message");
    }
    if (message == nullptr) {
        message = json_object_get_object(update, "channel_post");
        out->post = message != nullptr;
    }
    if (message == nullptr) {
        message = json_object_get_object(update, "edited_channel_post");
        out->post = message != nullptr;
    }
    if (message == nullptr) {
        return false;
    }
    if (!out->callback) {
        out->text = json_object_get_string(message, "text");
        if (out->text == nullptr) {
            out->text = json_object_get_string(message, "caption");
        }
    }
    chat = json_object_get_object(message, "chat");
    if (from == nullptr) {
        from = json_object_get_object(message, "from");
    }
    sender_chat = json_object_get_object(message, "sender_chat");
    if (from == nullptr && sender_chat != nullptr) {
        from = sender_chat;
    }
    out->username = from == nullptr ? nullptr : json_object_get_string(from, "username");
    out->chat_type = chat == nullptr ? nullptr : json_object_get_string(chat, "type");
    photos = json_object_get_array(message, "photo");
    document = json_object_get_object(message, "document");
    video = json_object_get_object(message, "video");
    voice = json_object_get_object(message, "voice");
    audio = json_object_get_object(message, "audio");
    out->media = photos != nullptr || document != nullptr || video != nullptr || voice != nullptr || audio != nullptr;
    if (photos != nullptr && json_array_get_count(photos) > 0) {
        photo = json_array_get_object(photos, json_array_get_count(photos) - 1);
        out->media_type = "image";
        out->file_id = photo == nullptr ? nullptr : json_object_get_string(photo, "file_id");
        file_size = photo == nullptr ? 0.0 : json_object_get_number(photo, "file_size");
    } else if (document != nullptr) {
        out->media_type = json_object_get_string(document, "mime_type");
        if (out->media_type == nullptr || out->media_type[0] == '\0') {
            out->media_type = "application/octet-stream";
        }
        out->file_id = json_object_get_string(document, "file_id");
        out->file_name = json_object_get_string(document, "file_name");
        file_size = json_object_get_number(document, "file_size");
    } else if (video != nullptr) {
        out->media_type = json_object_get_string(video, "mime_type");
        if (out->media_type == nullptr || out->media_type[0] == '\0') {
            out->media_type = "video/mp4";
        }
        out->file_id = json_object_get_string(video, "file_id");
        out->file_name = json_object_get_string(video, "file_name");
        file_size = json_object_get_number(video, "file_size");
    } else if (voice != nullptr || audio != nullptr) {
        JSON_Object *audio_object = voice != nullptr ? voice : audio;
        out->media_type = json_object_get_string(audio_object, "mime_type");
        if (out->media_type == nullptr || out->media_type[0] == '\0') {
            out->media_type = voice != nullptr ? "audio/ogg" : "audio/mpeg";
        }
        out->file_id = json_object_get_string(audio_object, "file_id");
        out->file_name = json_object_get_string(audio_object, "file_name");
        file_size = json_object_get_number(audio_object, "file_size");
    }
    if (file_size > 0.0) {
        out->file_size = (size_t)file_size;
    }
    out->has_timestamp = json_i64(message, "date", &out->timestamp_unix_secs);
    if (out->text == nullptr || chat == nullptr) {
        if (!out->media || chat == nullptr) {
            return false;
        }
        out->text = "[media]";
    }
    if (!json_i64(message, "message_id", &out->message_id) ||
        !json_i64(chat, "id", &out->chat_id)) {
        return false;
    }
    if (from == nullptr || !json_i64(from, "id", &out->sender_id)) {
        out->sender_id = out->chat_id;
    }
    if (out->sender_id == 0) {
        return false;
    }
    out->has_thread_id = json_i64(message, "message_thread_id", &out->thread_id);
    reply_to_message = json_object_get_object(message, "reply_to_message");
    out->has_reply_to_message_id = reply_to_message != nullptr &&
                                   json_i64(reply_to_message, "message_id", &out->reply_to_message_id);
    if (reply_to_message != nullptr) {
        out->reply_to_text = json_object_get_string(reply_to_message, "text");
        if (out->reply_to_text == nullptr) {
            out->reply_to_text = json_object_get_string(reply_to_message, "caption");
        }
    }
    return true;
}

static sc_status telegram_update_to_inbound(telegram_channel *telegram,
                                            const telegram_update *update,
                                            sc_allocator *alloc,
                                            sc_channel_inbound *out)
{
    sc_string storage_path = {0};
    size_t stored_size = 0;
    sc_str attachment_media_type = update->media ? telegram_declared_content_type(update) :
                                                   sc_str_from_cstr("");
    sc_status status = sc_status_ok();

    *out = (sc_channel_inbound){.struct_size = sizeof(*out)};
    status = copy_i64_string(alloc, update->update_id, &out->owned_message_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "telegram", &out->owned_channel_name);
    }
    if (sc_status_is_ok(status) && telegram->bot_username.len > 0) {
        status = copy_string(alloc, sc_string_as_str(&telegram->bot_username), &out->owned_platform_account_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_i64_string(alloc, update->chat_id, &out->owned_conversation_id);
    }
    if (sc_status_is_ok(status) && update->has_thread_id) {
        status = copy_i64_string(alloc, update->thread_id, &out->owned_thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = update->username != nullptr && update->username[0] != '\0'
                     ? sc_string_from_cstr(alloc, update->username, &out->owned_sender_id)
                     : copy_i64_string(alloc, update->sender_id, &out->owned_sender_id);
    }
    if (sc_status_is_ok(status) && telegram->mention_only && update->reply_to_text != nullptr &&
        update->reply_to_text[0] != '\0') {
        status = telegram_build_reply_context_text(update, alloc, &out->owned_text);
    } else if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, update->text, &out->owned_text);
    }
    if (sc_status_is_ok(status) && update->has_reply_to_message_id) {
        status = copy_i64_string(alloc, update->reply_to_message_id, &out->owned_reply_to_message_id);
    }
    if (sc_status_is_ok(status) && update->media && update->file_id != nullptr && update->file_id[0] != '\0') {
        status = telegram_download_media(telegram, update, alloc, &attachment_media_type, &storage_path, &stored_size);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, attachment_media_type, &out->owned_attachment_media_type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, update->file_name == nullptr ? "" : update->file_name, &out->owned_attachment_name);
    }
    if (sc_status_is_ok(status)) {
        out->owned_attachment_storage_path = storage_path;
        storage_path = (sc_string){0};
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&storage_path);
        sc_channel_inbound_clear(out);
        return status;
    }

    out->message_id = sc_string_as_str(&out->owned_message_id);
    out->channel_name = sc_string_as_str(&out->owned_channel_name);
    out->platform_account_id = sc_string_as_str(&out->owned_platform_account_id);
    out->conversation_id = sc_string_as_str(&out->owned_conversation_id);
    out->thread_id = sc_string_as_str(&out->owned_thread_id);
    out->sender_id = sc_string_as_str(&out->owned_sender_id);
    out->text = sc_string_as_str(&out->owned_text);
    out->reply_to_message_id = sc_string_as_str(&out->owned_reply_to_message_id);
    out->attachment_media_type = sc_string_as_str(&out->owned_attachment_media_type);
    out->attachment_name = sc_string_as_str(&out->owned_attachment_name);
    out->attachment_storage_path = sc_string_as_str(&out->owned_attachment_storage_path);
    out->attachment_size_bytes = stored_size == 0 ? update->file_size : stored_size;
    out->attachment_temporary = out->owned_attachment_storage_path.len > 0;
    out->timestamp_unix_secs = update->has_timestamp ? update->timestamp_unix_secs : 0;
    out->timestamp_trusted = update->has_timestamp;
    out->cancel_previous = false;
    out->disable_memory = telegram->mention_only;
    return sc_status_ok();
}

static sc_status telegram_download_media(telegram_channel *telegram,
                                         const telegram_update *update,
                                         sc_allocator *alloc,
                                         sc_str *content_type,
                                         sc_string *storage_path,
                                         size_t *size_bytes)
{
    sc_string get_file_url = {0};
    sc_string get_file_response = {0};
    sc_string file_path = {0};
    sc_string download_url = {0};
    sc_string bytes = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr ||
        update == nullptr ||
        content_type == nullptr ||
        storage_path == nullptr ||
        size_bytes == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.media_invalid_argument");
    }
    *size_bytes = 0;
    if (update->file_size > 0 && update->file_size > telegram->max_media_bytes) {
        return sc_status_security_denied("sc.channel_telegram.media_too_large");
    }

    status = telegram_build_get_file_url(telegram, sc_str_from_cstr(update->file_id), telegram->alloc, &get_file_url);
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("GET"),
                                       sc_string_as_str(&get_file_url),
                                       sc_str_from_cstr(""),
                                       alloc,
                                       &get_file_response);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_parse_file_path(sc_string_as_str(&get_file_response), telegram->alloc, &file_path);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_build_download_file_url(telegram, sc_string_as_str(&file_path), telegram->alloc, &download_url);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_http_request(telegram,
                                       sc_str_from_cstr("GET"),
                                       sc_string_as_str(&download_url),
                                       sc_str_from_cstr(""),
                                       alloc,
                                       &bytes);
    }
    if (sc_status_is_ok(status)) {
        status = telegram_store_media_bytes(telegram,
                                            update,
                                            sc_string_as_str(&bytes),
                                            alloc,
                                            content_type,
                                            storage_path,
                                            size_bytes);
    }

    sc_string_clear(&bytes);
    sc_string_clear(&download_url);
    sc_string_clear(&file_path);
    sc_string_clear(&get_file_response);
    sc_string_clear(&get_file_url);
    return status;
}

static sc_status telegram_store_media_bytes(telegram_channel *telegram,
                                            const telegram_update *update,
                                            sc_str bytes,
                                            sc_allocator *alloc,
                                            sc_str *content_type,
                                            sc_string *storage_path,
                                            size_t *size_bytes)
{
    sc_media_attachment input = {0};
    sc_media_attachment stored = {0};
    sc_media_limits limits = {0};
    sc_str declared_type = telegram_declared_content_type(update);
    sc_str detected_type = telegram_detect_media_type(bytes);
    sc_str resolved_type = sc_str_from_cstr("");
    sc_status status = sc_status_ok();

    if (telegram == nullptr ||
        update == nullptr ||
        content_type == nullptr ||
        storage_path == nullptr ||
        size_bytes == nullptr ||
        bytes.ptr == nullptr ||
        bytes.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.media_store_invalid_argument");
    }
    *size_bytes = 0;
    if (detected_type.len > 0 && !telegram_media_type_matches(update, declared_type, detected_type)) {
        return sc_status_security_denied("sc.channel_telegram.media_type_mismatch");
    }
    resolved_type = telegram_resolved_content_type(update, declared_type, detected_type);

    sc_str allowed[] = {resolved_type};
    limits = (sc_media_limits){
        .struct_size = sizeof(limits),
        .max_bytes = telegram->max_media_bytes,
        .allowed_content_types = allowed,
        .allowed_content_type_count = SC_ARRAY_LEN(allowed),
    };
    input = (sc_media_attachment){
        .struct_size = sizeof(input),
        .attachment_id = sc_str_from_cstr(update->file_id),
        .content_type = resolved_type,
        .filename = update->file_name == nullptr ? sc_str_from_cstr("") : sc_str_from_cstr(update->file_name),
        .bytes = sc_buf_from_parts(bytes.ptr, bytes.len),
        .safety_flags = SC_MEDIA_SAFETY_UNTRUSTED,
    };
    status = sc_media_attachment_from_bytes(alloc, &input, &limits, &stored);
    if (sc_status_is_ok(status)) {
        status = telegram_screen_media_attachment(alloc, &stored, &limits);
    }
    if (sc_status_is_ok(status)) {
        status = sc_media_attachment_store_temp(&stored, sc_string_as_str(&telegram->media_temp_dir));
    }
    if (sc_status_is_ok(status)) {
        *size_bytes = stored.size_bytes;
        *content_type = resolved_type;
        *storage_path = stored.owned_storage_path;
        stored.owned_storage_path = (sc_string){0};
        stored.storage_path = sc_str_from_cstr("");
        stored.temporary = false;
    }
    sc_media_attachment_clear(&stored);
    return status;
}

static sc_status telegram_screen_media_attachment(sc_allocator *alloc,
                                                  const sc_media_attachment *attachment,
                                                  const sc_media_limits *limits)
{
    sc_media_pipeline_result result = {0};
    sc_status status = sc_status_ok();

    if (attachment == nullptr || limits == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.media_screen_invalid_argument");
    }
    if (!sc_media_content_type_is_audio(attachment->content_type)) {
        return sc_status_ok();
    }
    status = sc_media_pipeline_run(alloc,
                                   &(sc_media_pipeline_options){
                                       .struct_size = sizeof(sc_media_pipeline_options),
                                       .limits = *limits,
                                   },
                                   attachment,
                                   sc_str_from_cstr(""),
                                   &result);
    sc_media_pipeline_result_clear(&result);
    if (status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    return status;
}

static sc_str telegram_declared_content_type(const telegram_update *update)
{
    if (update == nullptr || update->media_type == nullptr || update->media_type[0] == '\0') {
        return sc_str_from_cstr("application/octet-stream");
    }
    if (strcmp(update->media_type, "image") == 0) {
        return sc_str_from_cstr("image/png");
    }
    if (strcmp(update->media_type, "document") == 0) {
        return sc_str_from_cstr("application/octet-stream");
    }
    if (strcmp(update->media_type, "video") == 0) {
        return sc_str_from_cstr("video/mp4");
    }
    if (strcmp(update->media_type, "audio") == 0) {
        return sc_str_from_cstr("audio/mpeg");
    }
    return sc_str_from_cstr(update->media_type);
}

static sc_str telegram_detect_media_type(sc_str bytes)
{
    static const unsigned char png_sig[] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};
    if (bytes.ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    if (bytes.len >= sizeof(png_sig) && memcmp(bytes.ptr, png_sig, sizeof(png_sig)) == 0) {
        return sc_str_from_cstr("image/png");
    }
    if (bytes.len >= 3 && (unsigned char)bytes.ptr[0] == 0xFFu && (unsigned char)bytes.ptr[1] == 0xD8u &&
        (unsigned char)bytes.ptr[2] == 0xFFu) {
        return sc_str_from_cstr("image/jpeg");
    }
    if (bytes.len >= 4 && memcmp(bytes.ptr, "GIF8", 4) == 0) {
        return sc_str_from_cstr("image/gif");
    }
    if (bytes.len >= 4 && memcmp(bytes.ptr, "%PDF", 4) == 0) {
        return sc_str_from_cstr("application/pdf");
    }
    if (bytes.len >= 12 && memcmp(bytes.ptr, "RIFF", 4) == 0 && memcmp(bytes.ptr + 8, "WAVE", 4) == 0) {
        return sc_str_from_cstr("audio/wav");
    }
    return sc_str_from_cstr("");
}

static sc_str telegram_resolved_content_type(const telegram_update *update,
                                             sc_str declared_type,
                                             sc_str detected_type)
{
    if (detected_type.len > 0 && telegram_media_type_matches(update, declared_type, detected_type)) {
        return detected_type;
    }
    return declared_type;
}

static bool telegram_media_type_matches(const telegram_update *update, sc_str declared_type, sc_str detected_type)
{
    if (detected_type.len == 0) {
        return true;
    }
    if (sc_str_equal(declared_type, detected_type)) {
        return true;
    }
    return update != nullptr &&
           update->media_type != nullptr &&
           strcmp(update->media_type, "image") == 0 &&
           telegram_content_type_is_image(detected_type);
}

static bool telegram_content_type_is_image(sc_str content_type)
{
    return sc_str_equal(content_type, sc_str_from_cstr("image/png")) ||
           sc_str_equal(content_type, sc_str_from_cstr("image/jpeg")) ||
           sc_str_equal(content_type, sc_str_from_cstr("image/gif"));
}

static bool telegram_status_consumes_update(sc_status status)
{
    return status.error_key != nullptr &&
           (strcmp(status.error_key, "sc.channel_telegram.media_type_mismatch") == 0 ||
            strcmp(status.error_key, "sc.channel_telegram.media_too_large") == 0 ||
            strcmp(status.error_key, "sc.media.attachment.too_large") == 0);
}

static sc_status telegram_check_rate_limit(telegram_channel *telegram)
{
    time_t now;

    if (telegram == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.rate_limit_invalid_argument");
    }
    if (telegram->rate_limit_per_minute == 0) {
        return sc_status_ok();
    }
    now = time(nullptr);
    if (telegram->rate_window_start == 0 || now - telegram->rate_window_start >= 60) {
        telegram->rate_window_start = now;
        telegram->rate_window_count = 0;
    }
    if (telegram->rate_window_count >= telegram->rate_limit_per_minute) {
        return sc_status_security_denied("sc.channel_telegram.rate_limited");
    }
    telegram->rate_window_count += 1;
    return sc_status_ok();
}

static bool telegram_update_visible(const telegram_channel *telegram, const telegram_update *update)
{
    if (telegram == nullptr || update == nullptr || !telegram->mention_only) {
        return true;
    }
    if (update->callback) {
        return true;
    }
    if (telegram->bot_username.len == 0) {
        return true;
    }
    if (update->chat_type != nullptr && strcmp(update->chat_type, "private") == 0) {
        return true;
    }
    return telegram_update_mentions_bot(telegram, update);
}

static bool telegram_update_mentions_bot(const telegram_channel *telegram, const telegram_update *update)
{
    if (telegram == nullptr || update == nullptr) {
        return false;
    }
    return telegram_text_mentions_bot(telegram, update->text) ||
           telegram_text_mentions_bot(telegram, update->reply_to_text);
}

static bool telegram_text_mentions_bot(const telegram_channel *telegram, const char *text)
{
    sc_str view = text == nullptr ? sc_str_from_cstr("") : sc_str_from_cstr(text);
    sc_string_builder mention = {0};
    sc_string needle = {0};
    bool mentions = false;

    if (telegram == nullptr || telegram->bot_username.len == 0) {
        return false;
    }
    sc_string_builder_init(&mention, telegram->alloc);
    if (sc_status_is_ok(sc_string_builder_append_cstr(&mention, "@")) &&
        sc_status_is_ok(sc_string_builder_append(&mention, sc_string_as_str(&telegram->bot_username))) &&
        sc_status_is_ok(sc_string_builder_finish(&mention, &needle))) {
        mentions = str_contains_cstr(view, needle.ptr);
    } else {
        sc_string_builder_clear(&mention);
    }
    sc_string_clear(&needle);
    return mentions;
}

static sc_status telegram_build_reply_context_text(const telegram_update *update, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (update == nullptr || update->text == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_telegram.reply_context_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "Replied message context:\n");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_str_from_cstr(update->reply_to_text));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\n\nUser message:\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_str_from_cstr(update->text));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool str_contains_cstr(sc_str text, const char *needle)
{
    size_t needle_len = needle == nullptr ? 0 : strlen(needle);
    if (needle_len == 0) {
        return true;
    }
    if (text.ptr == nullptr || text.len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= text.len; i += 1) {
        if (memcmp(text.ptr + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool stream_mode_is_off(const telegram_channel *telegram)
{
    return telegram == nullptr || telegram->stream_mode.len == 0 ||
           sc_str_equal(sc_string_as_str(&telegram->stream_mode), sc_str_from_cstr("off"));
}

static bool telegram_is_placeholder_draft(const sc_channel_message *message)
{
    return message != nullptr && message->draft && sc_str_equal(message->text, sc_str_from_cstr("..."));
}

static bool same_message_target(const telegram_channel *telegram, const sc_channel_message *message)
{
    if (telegram == nullptr || message == nullptr) {
        return false;
    }
    return sc_str_equal(sc_string_as_str(&telegram->last_draft_chat_id), message->conversation_id) &&
           sc_str_equal(sc_string_as_str(&telegram->last_draft_thread_id), message->thread_id);
}

static bool telegram_attachment_is_wav(const sc_channel_message *message)
{
    return message != nullptr &&
           (sc_str_equal(message->attachment_content_type, sc_str_from_cstr("audio/wav")) ||
            sc_str_equal(message->attachment_content_type, sc_str_from_cstr("audio/x-wav")));
}

static bool telegram_attachment_is_image(const sc_channel_message *message)
{
    return message != nullptr &&
           (sc_str_equal(message->attachment_content_type, sc_str_from_cstr("image/png")) ||
            sc_str_equal(message->attachment_content_type, sc_str_from_cstr("image/jpeg")) ||
            sc_str_equal(message->attachment_content_type, sc_str_from_cstr("image/webp")));
}

static bool telegram_photo_status_allows_document_fallback(sc_status status)
{
    return status.code == SC_ERR_HTTP &&
           status.error_key != nullptr &&
           (strcmp(status.error_key, "sc.channel_telegram.http_error") == 0 ||
            strcmp(status.error_key, "sc.channel_telegram.api_error") == 0);
}

static bool telegram_http_status_retryable(long response_code)
{
    return response_code == 408 || response_code == 429 || response_code >= 500;
}

#ifdef SC_HAVE_LIBCURL
static bool telegram_curl_code_retryable(CURLcode code)
{
    return code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_COULDNT_CONNECT ||
           code == CURLE_OPERATION_TIMEDOUT ||
           code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR ||
           code == CURLE_GOT_NOTHING ||
           code == CURLE_PARTIAL_FILE;
}

static long telegram_curl_http_version(void)
{
#if defined(SC_HAVE_NGHTTP2) || defined(SC_HAVE_CURL_HTTP2)
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);

    if (info != nullptr && (info->features & CURL_VERSION_HTTP2) != 0) {
        return (long)CURL_HTTP_VERSION_2TLS;
    }
#endif
    return (long)CURL_HTTP_VERSION_1_1;
}
#endif

static void sleep_ms(uint32_t delay_ms)
{
    struct timespec requested = {
        .tv_sec = (time_t)(delay_ms / 1000U),
        .tv_nsec = (long)(delay_ms % 1000U) * 1000000L,
    };
    if (delay_ms == 0) {
        return;
    }
    (void)nanosleep(&requested, nullptr);
}

static bool json_i64(JSON_Object *object, const char *name, int64_t *out)
{
    JSON_Value *value = object == nullptr ? nullptr : json_object_get_value(object, name);
    double number = 0.0;
    if (value == nullptr || json_value_get_type(value) != JSONNumber || out == nullptr) {
        return false;
    }
    number = json_value_get_number(value);
    if (number < (double)INT64_MIN || number > (double)INT64_MAX) {
        return false;
    }
    *out = (int64_t)number;
    return true;
}

#ifdef SC_HAVE_LIBCURL
static sc_status telegram_curl_global_init(void)
{
    return sc_curl_global_init("sc.channel_telegram.curl_init_failed");
}

static void telegram_curl_apply_common_options(const telegram_channel *telegram, CURL *curl)
{
    const char *ca_bundle = telegram_ca_bundle_path();

    if (curl == nullptr) {
        return;
    }
    if (ca_bundle != nullptr && ca_bundle[0] != '\0') {
        (void)curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    }
    if (telegram_low_memory_transport_enabled()) {
        (void)curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, (long)telegram_low_memory_upload_buffer_size);
        (void)curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    } else if (telegram != nullptr) {
        (void)curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
    }
}

static void telegram_curl_release_if_low_memory(telegram_channel *telegram)
{
    if (telegram == nullptr || telegram->curl == nullptr || !telegram_low_memory_transport_enabled()) {
        return;
    }
    curl_easy_cleanup(telegram->curl);
    telegram->curl = nullptr;
}

static bool telegram_low_memory_transport_enabled(void)
{
    const char *value = getenv("SC_LOW_MEMORY_TRANSPORT");
    if (value == nullptr || value[0] == '\0') {
        value = getenv("SC_LOW_MEMORY");
    }
    return value != nullptr &&
        (strcmp(value, "1") == 0 ||
         strcmp(value, "true") == 0 ||
         strcmp(value, "TRUE") == 0 ||
         strcmp(value, "yes") == 0 ||
         strcmp(value, "YES") == 0 ||
         strcmp(value, "on") == 0 ||
         strcmp(value, "ON") == 0);
}

static const char *telegram_ca_bundle_path(void)
{
    const char *value = getenv("SC_TELEGRAM_CA_BUNDLE");
    if (value == nullptr || value[0] == '\0') {
        value = getenv("SC_CA_BUNDLE");
    }
    if (value == nullptr || value[0] == '\0') {
        value = getenv("CURL_CA_BUNDLE");
    }
    return value;
}

static sc_status telegram_curl_request(telegram_channel *telegram,
                                       sc_str method,
                                       sc_str url,
                                       sc_str body,
                                       sc_allocator *alloc,
                                       sc_string *out)
{
    CURL *curl = nullptr;
    // cppcheck-suppress unreadVariable
    CURLcode code = CURLE_FAILED_INIT;
    long response_code = 0;
    curl_response response = {0};
    sc_status status = sc_status_ok();
    char http_status_text[32] = {0};
    char curl_code_text[32] = {0};
    char response_bytes[32] = {0};
    char timeout_seconds[32] = {0};
    char attempts_text[32] = {0};
    char max_attempts_text[32] = {0};
    char backoff_text[32] = {0};
    char curl_error_buffer[CURL_ERROR_SIZE] = {0};
    sc_log_field retry_fields[9] = {0};
    sc_log_field fields[11] = {0};
    uint32_t max_attempts = telegram->max_retries + 1U;
    uint32_t attempt = 0;

    status = telegram_curl_global_init();
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (telegram->curl == nullptr) {
        telegram->curl = curl_easy_init();
    }
    curl = telegram->curl;
    if (curl == nullptr) {
        return sc_status_no_memory();
    }

    sc_bytes_init(&response.bytes, alloc);
    response.max_bytes = telegram->max_response_bytes;
    curl_easy_reset(curl);
    (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, telegram_curl_write_callback);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 telegram-channel");
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, telegram->poll_timeout_seconds + 10L);
    (void)curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, telegram_curl_http_version());
    (void)curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_buffer);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    telegram_curl_apply_common_options(telegram, curl);

    if (sc_str_equal(method, sc_str_from_cstr("POST"))) {
        (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.ptr == nullptr ? "" : body.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.len);
    } else {
        (void)curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    for (attempt = 1; attempt <= max_attempts; attempt += 1) {
        response.bytes.len = 0;
        response.too_large = false;
        response_code = 0;
        curl_error_buffer[0] = '\0';
        code = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response.too_large ||
            (!telegram_curl_code_retryable(code) && !telegram_http_status_retryable(response_code)) ||
            attempt >= max_attempts) {
            break;
        }
        (void)snprintf(http_status_text, sizeof(http_status_text), "%ld", response_code);
        (void)snprintf(curl_code_text, sizeof(curl_code_text), "%d", (int)code);
        (void)snprintf(attempts_text, sizeof(attempts_text), "%u", attempt);
        (void)snprintf(max_attempts_text, sizeof(max_attempts_text), "%u", max_attempts);
        (void)snprintf(backoff_text, sizeof(backoff_text), "%u", telegram->retry_backoff_ms * attempt);
        retry_fields[0] = (sc_log_field){.key = "method", .value = method, .secret = false};
        retry_fields[1] = (sc_log_field){.key = "http_status", .value = sc_str_from_cstr(http_status_text), .secret = false};
        retry_fields[2] = (sc_log_field){.key = "curl_code", .value = sc_str_from_cstr(curl_code_text), .secret = false};
        retry_fields[3] = (sc_log_field){.key = "curl_error", .value = sc_str_from_cstr(curl_easy_strerror(code)), .secret = false};
        retry_fields[4] = (sc_log_field){.key = "curl_detail", .value = sc_str_from_cstr(curl_error_buffer), .secret = false};
        retry_fields[5] = (sc_log_field){.key = "attempt", .value = sc_str_from_cstr(attempts_text), .secret = false};
        retry_fields[6] = (sc_log_field){.key = "max_attempts", .value = sc_str_from_cstr(max_attempts_text), .secret = false};
        retry_fields[7] = (sc_log_field){.key = "backoff_ms", .value = sc_str_from_cstr(backoff_text), .secret = false};
        retry_fields[8] = (sc_log_field){.key = "too_large", .value = sc_str_from_cstr(response.too_large ? "true" : "false"), .secret = false};
        sc_log_write(SC_LOG_TRACE, "sc.net", "net.telegram.curl.retry", retry_fields, SC_ARRAY_LEN(retry_fields));
        sleep_ms(telegram->retry_backoff_ms * attempt);
    }
    if (code != CURLE_OK || response.too_large) {
        status = sc_status_http(response.too_large ? "sc.channel_telegram.response_too_large" : "sc.channel_telegram.request_failed");
    } else if (response_code >= 400) {
        status = sc_status_http("sc.channel_telegram.http_error");
    } else {
        status = sc_string_from_str(alloc, sc_str_from_parts((const char *)response.bytes.ptr, response.bytes.len), out);
    }

    (void)snprintf(http_status_text, sizeof(http_status_text), "%ld", response_code);
    (void)snprintf(curl_code_text, sizeof(curl_code_text), "%d", (int)code);
    (void)snprintf(response_bytes, sizeof(response_bytes), "%zu", response.bytes.len);
    (void)snprintf(timeout_seconds, sizeof(timeout_seconds), "%ld", telegram->poll_timeout_seconds + 10L);
    (void)snprintf(attempts_text, sizeof(attempts_text), "%u", attempt);
    (void)snprintf(max_attempts_text, sizeof(max_attempts_text), "%u", max_attempts);
    fields[0] = (sc_log_field){.key = "method", .value = method, .secret = false};
    fields[1] = (sc_log_field){.key = "http_status", .value = sc_str_from_cstr(http_status_text), .secret = false};
    fields[2] = (sc_log_field){.key = "curl_code", .value = sc_str_from_cstr(curl_code_text), .secret = false};
    fields[3] = (sc_log_field){.key = "curl_error", .value = sc_str_from_cstr(curl_easy_strerror(code)), .secret = false};
    fields[4] = (sc_log_field){.key = "curl_detail", .value = sc_str_from_cstr(curl_error_buffer), .secret = false};
    fields[5] = (sc_log_field){.key = "response_bytes", .value = sc_str_from_cstr(response_bytes), .secret = false};
    fields[6] = (sc_log_field){.key = "timeout_seconds", .value = sc_str_from_cstr(timeout_seconds), .secret = false};
    fields[7] = (sc_log_field){.key = "too_large", .value = sc_str_from_cstr(response.too_large ? "true" : "false"), .secret = false};
    fields[8] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
    fields[9] = (sc_log_field){.key = "attempts", .value = sc_str_from_cstr(attempts_text), .secret = false};
    fields[10] = (sc_log_field){.key = "max_attempts", .value = sc_str_from_cstr(max_attempts_text), .secret = false};
    sc_log_write(sc_status_is_ok(status) ? SC_LOG_TRACE : SC_LOG_WARN, "sc.net", "net.telegram.curl.done", fields, SC_ARRAY_LEN(fields));

    telegram_curl_release_if_low_memory(telegram);
    sc_bytes_clear(&response.bytes);
    return status;
}

static sc_status telegram_curl_send_audio_multipart(telegram_channel *telegram,
                                                    sc_str url,
                                                    const sc_channel_message *message,
                                                    sc_str file_field,
                                                    sc_str default_filename,
                                                    sc_str default_content_type,
                                                    sc_allocator *alloc,
                                                    sc_string *out)
{
    CURL *curl = nullptr;
    CURLcode code;
    curl_mime *mime = nullptr;
    curl_mimepart *part = nullptr;
    curl_response response = {0};
    long response_code = 0;
    sc_string reply_parameters = {0};
    sc_string filename = {0};
    sc_string content_type = {0};
    sc_string_builder reply_builder = {0};
    sc_status status = sc_status_ok();

    if (telegram == nullptr ||
        message == nullptr ||
        file_field.ptr == nullptr ||
        file_field.len == 0 ||
        message->attachment_bytes.ptr == nullptr ||
        message->attachment_bytes.len == 0) {
        return sc_status_invalid_argument("sc.channel_telegram.audio_invalid_argument");
    }
    status = telegram_curl_global_init();
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (telegram->curl == nullptr) {
        telegram->curl = curl_easy_init();
    }
    curl = telegram->curl;
    if (curl == nullptr) {
        return sc_status_no_memory();
    }

    sc_bytes_init(&response.bytes, alloc);
    response.max_bytes = telegram->max_response_bytes;
    mime = curl_mime_init(curl);
    if (mime == nullptr) {
        status = sc_status_no_memory();
    }
    if (sc_status_is_ok(status)) {
        part = curl_mime_addpart(mime);
        if (part == nullptr ||
            curl_mime_name(part, "chat_id") != CURLE_OK ||
            curl_mime_data(part, message->conversation_id.ptr, message->conversation_id.len) != CURLE_OK) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status) && message->thread_id.len > 0) {
        part = curl_mime_addpart(mime);
        if (part == nullptr ||
            curl_mime_name(part, "message_thread_id") != CURLE_OK ||
            curl_mime_data(part, message->thread_id.ptr, message->thread_id.len) != CURLE_OK) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        sc_string_builder_init(&reply_builder, alloc);
        status = sc_string_builder_append_cstr(&reply_builder, "{\"message_id\":");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&reply_builder, message->reply_to_message_id);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&reply_builder, "}");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&reply_builder, &reply_parameters);
        } else {
            sc_string_builder_clear(&reply_builder);
        }
        if (sc_status_is_ok(status)) {
            part = curl_mime_addpart(mime);
            if (part == nullptr ||
                curl_mime_name(part, "reply_parameters") != CURLE_OK ||
                curl_mime_data(part, reply_parameters.ptr, reply_parameters.len) != CURLE_OK) {
                status = sc_status_no_memory();
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc,
                                    message->attachment_filename.len == 0 ? default_filename : message->attachment_filename,
                                    &filename);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc,
                                    message->attachment_content_type.len == 0 ? default_content_type : message->attachment_content_type,
                                    &content_type);
    }
    if (sc_status_is_ok(status)) {
        part = curl_mime_addpart(mime);
        if (part == nullptr ||
            curl_mime_name(part, file_field.ptr) != CURLE_OK ||
            curl_mime_data(part, (const char *)message->attachment_bytes.ptr, message->attachment_bytes.len) != CURLE_OK ||
            curl_mime_filename(part, filename.ptr) != CURLE_OK ||
            curl_mime_type(part, content_type.ptr) != CURLE_OK) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status)) {
        char curl_error_buffer[CURL_ERROR_SIZE] = {0};

        curl_easy_reset(curl);
        (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, telegram_curl_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 telegram-channel");
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, telegram->poll_timeout_seconds + 10L);
        (void)curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, telegram_curl_http_version());
        (void)curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error_buffer);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        telegram_curl_apply_common_options(telegram, curl);
        curl_error_buffer[0] = '\0';
        code = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response.too_large) {
            status = sc_status_http("sc.channel_telegram.response_too_large");
        } else if (code != CURLE_OK) {
            status = sc_status_http("sc.channel_telegram.request_failed");
        } else if (response_code >= 400) {
            status = sc_status_http("sc.channel_telegram.http_error");
        } else {
            status = sc_string_from_str(alloc, sc_str_from_parts((const char *)response.bytes.ptr, response.bytes.len), out);
        }
        if (!sc_status_is_ok(status)) {
            char http_status_text[32] = {0};
            char curl_code_text[32] = {0};
            char response_bytes_text[32] = {0};
            char response_excerpt[192] = {0};
            sc_str api_method = telegram_api_method_from_url(url);
            (void)snprintf(http_status_text, sizeof(http_status_text), "%ld", response_code);
            (void)snprintf(curl_code_text, sizeof(curl_code_text), "%d", (int)code);
            (void)snprintf(response_bytes_text, sizeof(response_bytes_text), "%zu", response.bytes.len);
            telegram_curl_response_excerpt(&response, response_excerpt, sizeof(response_excerpt));
            sc_log_field fields[] = {
                {.key = "method", .value = sc_str_from_cstr("POST"), .secret = false},
                {.key = "telegram_method", .value = api_method, .secret = false},
                {.key = "file_field", .value = file_field, .secret = false},
                {.key = "http_status", .value = sc_str_from_cstr(http_status_text), .secret = false},
                {.key = "curl_code", .value = sc_str_from_cstr(curl_code_text), .secret = false},
                {.key = "curl_error", .value = sc_str_from_cstr(curl_easy_strerror(code)), .secret = false},
                {.key = "curl_detail", .value = sc_str_from_cstr(curl_error_buffer), .secret = false},
                {.key = "response_bytes", .value = sc_str_from_cstr(response_bytes_text), .secret = false},
                {.key = "response_excerpt", .value = sc_str_from_cstr(response_excerpt), .secret = false},
                {.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false},
            };
            sc_log_write(SC_LOG_WARN, "sc.net", "net.telegram.curl.multipart_failed", fields, SC_ARRAY_LEN(fields));
        }
    }
    sc_string_clear(&content_type);
    sc_string_clear(&filename);
    sc_string_clear(&reply_parameters);
    telegram_curl_release_if_low_memory(telegram);
    sc_bytes_clear(&response.bytes);
    curl_mime_free(mime);
    return status;
}

static void telegram_curl_response_excerpt(const curl_response *response, char *out, size_t out_size)
{
    size_t limit = 0;

    if (out == nullptr || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (response == nullptr || response->bytes.ptr == nullptr || response->bytes.len == 0) {
        return;
    }
    limit = response->bytes.len;
    if (limit >= out_size) {
        limit = out_size - 1U;
    }
    for (size_t i = 0; i < limit; i += 1) {
        unsigned char ch = response->bytes.ptr[i];
        out[i] = isprint(ch) ? (char)ch : '.';
    }
    out[limit] = '\0';
}

static size_t telegram_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    curl_response *response = userdata;
    size_t bytes = size * nmemb;
    size_t next_len = 0;
    sc_status status = sc_status_ok();

    if (response == nullptr || ptr == nullptr) {
        return 0;
    }
    if (sc_size_add_overflow(response->bytes.len, bytes, &next_len) || next_len > response->max_bytes) {
        response->too_large = true;
        return 0;
    }
    status = sc_bytes_append(&response->bytes, sc_buf_from_parts(ptr, bytes));
    return sc_status_is_ok(status) ? bytes : 0;
}
#endif
#endif
