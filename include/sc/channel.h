#pragma once

#include <stdint.h>

#include "sc/allocator.h"
#include "sc/autonomy.h"
#include "sc/contract.h"
#include "sc/observer.h"
#include "sc/result.h"
#include "sc/runtime.h"
#include "sc/string.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: message fields are borrowed for the call. The handle
 * owns impl and calls destroy exactly once. The wrapper does not synchronize
 * sends; channel implementations document their own thread-safety.
 */
typedef struct sc_channel sc_channel;
typedef struct sc_channel_orchestrator sc_channel_orchestrator;
typedef struct sc_transcriber sc_transcriber;
typedef struct sc_tts sc_tts;

typedef sc_status (*sc_telegram_http_request_fn)(void *user,
                                                 sc_str method,
                                                 sc_str url,
                                                 sc_str body,
                                                 sc_allocator *alloc,
                                                 sc_string *out);

typedef sc_status (*sc_mail_request_fn)(void *user,
                                        sc_str operation,
                                        sc_str url,
                                        sc_str username,
                                        sc_str password,
                                        sc_str payload,
                                        sc_allocator *alloc,
                                        sc_string *out);

enum {
    SC_CHANNEL_CAP_DRAFT_UPDATES = 1u << 0u,
    SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM = 1u << 1u,
    SC_CHANNEL_CAP_REACTIONS = 1u << 2u,
    SC_CHANNEL_CAP_ATTACHMENTS = 1u << 3u,
    SC_CHANNEL_CAP_MODERATION = 1u << 4u,
    SC_CHANNEL_CAP_APPROVAL_PROMPTS = 1u << 5u,
    SC_CHANNEL_CAP_INLINE_APPROVAL_BUTTONS = 1u << 6u,
    SC_CHANNEL_CAP_THREAD_REPLIES = 1u << 7u
};

enum {
    SC_CHANNEL_INBOUND_TEXT = 1u << 0u,
    SC_CHANNEL_INBOUND_MEDIA = 1u << 1u,
    SC_CHANNEL_INBOUND_EDIT = 1u << 2u,
    SC_CHANNEL_INBOUND_CALLBACK = 1u << 3u
};

enum {
    SC_CHANNEL_MEDIA_NONE = 0u,
    SC_CHANNEL_MEDIA_IMAGE = 1u << 0u,
    SC_CHANNEL_MEDIA_AUDIO = 1u << 1u,
    SC_CHANNEL_MEDIA_VIDEO = 1u << 2u,
    SC_CHANNEL_MEDIA_DOCUMENT = 1u << 3u
};

typedef enum sc_channel_approval_decision {
    SC_CHANNEL_APPROVAL_PENDING = 0,
    SC_CHANNEL_APPROVAL_APPROVED,
    SC_CHANNEL_APPROVAL_DENIED,
    SC_CHANNEL_APPROVAL_ALWAYS
} sc_channel_approval_decision;

typedef struct sc_channel_message {
    size_t struct_size;
    sc_str conversation_id;
    sc_str thread_id;
    sc_str sender_id;
    sc_str text;
    bool draft;
    sc_str reply_to_message_id;
    sc_str attachment_content_type;
    sc_str attachment_filename;
    sc_buf attachment_bytes;
    sc_attachment_delivery attachment_delivery;
} sc_channel_message;

typedef enum sc_channel_tts_reply_mode {
    SC_CHANNEL_TTS_REPLY_OFF = 0,
    SC_CHANNEL_TTS_REPLY_TEXT_AND_AUDIO
} sc_channel_tts_reply_mode;

typedef struct sc_channel_inbound {
    size_t struct_size;
    sc_str message_id;
    sc_str channel_name;
    sc_str platform_account_id;
    sc_str conversation_id;
    sc_str thread_id;
    sc_str sender_id;
    sc_str text;
    sc_str reply_to_message_id;
    sc_str attachment_media_type;
    sc_str attachment_name;
    sc_str attachment_storage_path;
    size_t attachment_size_bytes;
    int64_t timestamp_unix_secs;
    bool timestamp_trusted;
    bool attachment_temporary;
    bool cancel_previous;
    bool disable_memory;
    sc_string owned_message_id;
    sc_string owned_channel_name;
    sc_string owned_platform_account_id;
    sc_string owned_conversation_id;
    sc_string owned_thread_id;
    sc_string owned_sender_id;
    sc_string owned_text;
    sc_string owned_reply_to_message_id;
    sc_string owned_attachment_media_type;
    sc_string owned_attachment_name;
    sc_string owned_attachment_storage_path;
} sc_channel_inbound;

typedef struct sc_channel_health {
    size_t struct_size;
    bool healthy;
    sc_string message;
} sc_channel_health;

typedef struct sc_channel_approval_request {
    size_t struct_size;
    sc_str conversation_id;
    sc_str thread_id;
    sc_str sender_id;
    sc_str summary;
} sc_channel_approval_request;

typedef struct sc_channel_approval_response {
    size_t struct_size;
    sc_channel_approval_decision decision;
    sc_string reason;
} sc_channel_approval_response;

typedef struct sc_channel_common_config {
    size_t struct_size;
    sc_str channel_name;
    bool enabled;
    bool enabled_set;
    const sc_str *allowed_users;
    size_t allowed_user_count;
    const sc_str *allowed_destinations;
    size_t allowed_destination_count;
    bool reply_to_mentions_only;
    sc_str mention_token;
    sc_str provider;
    sc_str model;
    bool autonomy_level_set;
    sc_autonomy_level autonomy_level;
    uint64_t draft_update_interval_ms;
    const sc_str *tools_allow;
    size_t tools_allow_count;
    const sc_str *tools_deny;
    size_t tools_deny_count;
    bool pairing_required;
    bool paired;
    const sc_str *allowed_chats;
    size_t allowed_chat_count;
} sc_channel_common_config;

typedef struct sc_channel_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    const char *description_key;
    const char *config_schema_ref;
    const char *const *required_secret_keys;
    size_t required_secret_key_count;
    uint64_t inbound_event_types;
    uint64_t media_capabilities;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*send)(void *impl, const sc_channel_message *message);
    sc_status (*listen)(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
    sc_status (*health)(void *impl, sc_allocator *alloc, sc_channel_health *out);
    sc_status (*request_approval)(void *impl,
                                  const sc_channel_approval_request *request,
                                  sc_allocator *alloc,
                                  sc_channel_approval_response *out);
    void (*destroy)(void *impl);
} sc_channel_vtab;

typedef struct sc_channel_orchestrator_options {
    size_t struct_size;
    sc_agent *agent;
    sc_observer *observer;
    sc_channel **channels;
    size_t channel_count;
    const sc_str *allowed_senders;
    size_t allowed_sender_count;
    size_t max_history_messages;
    bool session_persistence;
    sc_str session_db_path;
    sc_str session_jsonl_dir;
    bool ack_reactions;
    bool show_tool_calls;
    bool interrupt_on_new_message;
    sc_str stream_mode;
    uint64_t approval_timeout_secs;
    size_t max_seen_message_ids;
    const struct sc_channel_provider_route *provider_routes;
    size_t provider_route_count;
    const sc_channel_common_config *common_configs;
    size_t common_config_count;
    sc_transcriber *transcriber;
    sc_tts *tts;
    sc_channel_tts_reply_mode tts_reply_mode;
    sc_str tts_voice;
} sc_channel_orchestrator_options;

typedef struct sc_channel_provider_route {
    size_t struct_size;
    sc_str channel_name;
    sc_str provider_name;
    sc_str model;
} sc_channel_provider_route;

typedef struct sc_channel_process_result {
    size_t struct_size;
    bool processed;
    bool duplicate;
    bool denied;
    bool cancelled_previous;
    sc_string reply;
} sc_channel_process_result;

typedef struct sc_telegram_channel_options {
    size_t struct_size;
    sc_str bot_token;
    sc_str api_base_url;
    sc_str bot_username;
    sc_str parse_mode;
    sc_str stream_mode;
    sc_str media_temp_dir;
    long poll_timeout_seconds;
    long approval_timeout_seconds;
    long draft_update_interval_ms;
    size_t max_response_bytes;
    size_t max_media_bytes;
    size_t message_split_bytes;
    uint32_t max_retries;
    uint32_t retry_backoff_ms;
    uint32_t rate_limit_per_minute;
    bool mention_only;
    bool interrupt_on_new_message;
    bool ack_reactions;
    bool post_reactions;
    sc_telegram_http_request_fn http_request;
    void *http_user;
} sc_telegram_channel_options;

typedef struct sc_channel_transport_options {
    size_t struct_size;
    sc_str endpoint;
    sc_str topic;
    sc_str queue;
    sc_str client_id;
} sc_channel_transport_options;

typedef struct sc_webhook_channel_options {
    size_t struct_size;
    sc_str bind;
    uint16_t port;
    sc_str path;
    sc_str auth_token;
    sc_str media_temp_dir;
    size_t max_body_bytes;
    size_t max_media_bytes;
    uint32_t rate_limit_per_minute;
    uint64_t replay_window_secs;
    void *uv_loop;
    const sc_str *allowed_ips;
    size_t allowed_ip_count;
} sc_webhook_channel_options;

typedef enum sc_webhook_reply_mode {
    SC_WEBHOOK_REPLY_STATUS_ONLY = 0,
    SC_WEBHOOK_REPLY_INLINE_JSON,
    SC_WEBHOOK_REPLY_INLINE_TEXT
} sc_webhook_reply_mode;

typedef enum sc_webhook_dispatch_kind {
    SC_WEBHOOK_DISPATCH_CONVERSATION = 0,
    SC_WEBHOOK_DISPATCH_CRON,
    SC_WEBHOOK_DISPATCH_SYSTEM_PROMPT
} sc_webhook_dispatch_kind;

typedef struct sc_webhook_named_options {
    size_t struct_size;
    sc_str name;
    bool enabled;
    sc_str path;
    sc_str secret;
    const sc_str *allow_list;
    size_t allow_list_count;
    sc_str dispatch_to;
    sc_str template_text;
    sc_webhook_reply_mode reply_mode;
    uint32_t rate_limit_per_sec;
    size_t max_body_bytes;
} sc_webhook_named_options;

typedef struct sc_webhook_named_request {
    size_t struct_size;
    sc_str method;
    sc_str path;
    sc_str body;
    sc_str remote_addr;
    sc_str x_hub_signature_256;
    sc_str x_signature;
    sc_str x_line_signature;
    sc_str x_nextcloud_talk_random;
    sc_str x_nextcloud_talk_signature;
} sc_webhook_named_request;

typedef struct sc_webhook_named_result {
    size_t struct_size;
    sc_webhook_dispatch_kind dispatch_kind;
    sc_string dispatch_target;
    sc_string rendered_text;
    sc_webhook_reply_mode reply_mode;
    int http_status;
    sc_string response_body;
} sc_webhook_named_result;

typedef struct sc_webhook_router sc_webhook_router;

typedef struct sc_webhook_ingest_request {
    size_t struct_size;
    sc_str method;
    sc_str path;
    sc_str auth_token;
    sc_str body;
    sc_str remote_addr;
} sc_webhook_ingest_request;

typedef struct sc_platform_channel_options {
    size_t struct_size;
    sc_str endpoint;
    sc_str bot_token;
    sc_str bot_user_id;
    sc_str bot_display_name;
    const sc_str *allowed_users;
    size_t allowed_user_count;
    const sc_str *allowed_destinations;
    size_t allowed_destination_count;
    bool mention_only;
    bool thread_replies;
    bool ack_reactions;
    bool draft_updates;
    bool multi_message_streaming;
    bool e2ee_enabled;
    sc_str device_id;
    sc_str channel_secret;
    sc_str access_token;
} sc_platform_channel_options;

typedef struct sc_rabbitmq_channel_options {
    size_t struct_size;
    sc_str url;
    sc_str exchange;
    sc_str routing_key;
    sc_str queue;
    sc_str consumer_tag;
    uint16_t prefetch;
    bool durable;
} sc_rabbitmq_channel_options;

typedef struct sc_mail_channel_options {
    size_t struct_size;
    sc_str inbox_url;
    sc_str smtp_url;
    sc_str username;
    sc_str password;
    sc_str from;
    sc_str to;
    sc_str mailbox;
    long poll_interval_seconds;
    sc_str oauth_token;
    const sc_str *allowed_senders;
    size_t allowed_sender_count;
    sc_str subject_prefix;
    sc_str attachment_dir;
    size_t max_message_bytes;
    bool delete_after_read;
    sc_mail_request_fn request;
    void *request_user;
} sc_mail_channel_options;

static inline bool sc_channel_handle_is_null(const sc_channel *channel)
{
    return channel == nullptr;
}

bool sc_channel_vtab_valid(const sc_channel_vtab *vtab);
sc_status sc_channel_new(sc_allocator *alloc, const sc_channel_vtab *vtab, void *impl, sc_channel **out);
sc_status sc_channel_send(sc_channel *channel, const sc_channel_message *message);
sc_status sc_channel_listen(sc_channel *channel, sc_allocator *alloc, sc_channel_inbound *out);
sc_status sc_channel_health_check(sc_channel *channel, sc_allocator *alloc, sc_channel_health *out);
sc_status sc_channel_request_approval(sc_channel *channel,
                                      const sc_channel_approval_request *request,
                                      sc_allocator *alloc,
                                      sc_channel_approval_response *out);
const sc_channel_vtab *sc_channel_vtab_of(const sc_channel *channel);
void sc_channel_destroy(sc_channel *channel);
void sc_channel_inbound_clear(sc_channel_inbound *message);
void sc_channel_health_clear(sc_channel_health *health);
void sc_channel_approval_response_clear(sc_channel_approval_response *response);
bool sc_channel_common_config_allows_event(const sc_channel_common_config *config,
                                           const sc_channel_inbound *event);

sc_status sc_channel_cli_new(sc_allocator *alloc, sc_channel **out);
sc_status sc_channel_fake_new(sc_allocator *alloc, uint64_t capabilities, sc_channel **out);
sc_status sc_channel_telegram_new(sc_allocator *alloc, const sc_telegram_channel_options *options, sc_channel **out);
sc_status sc_channel_webhook_new(sc_allocator *alloc, const sc_webhook_channel_options *options, sc_channel **out);
sc_status sc_channel_webhook_ingest(sc_channel *channel, const sc_webhook_ingest_request *request);
sc_status sc_channel_websocket_client_new(sc_allocator *alloc, const sc_channel_transport_options *options, sc_channel **out);
sc_status sc_channel_mqtt_new(sc_allocator *alloc, const sc_channel_transport_options *options, sc_channel **out);
sc_status sc_channel_rabbitmq_new(sc_allocator *alloc, const sc_channel_transport_options *options, sc_channel **out);
sc_status sc_channel_rabbitmq_vendor_new(sc_allocator *alloc, const sc_rabbitmq_channel_options *options, sc_channel **out);
sc_status sc_channel_mail_new(sc_allocator *alloc, const sc_mail_channel_options *options, sc_channel **out);
sc_status sc_channel_matrix_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_mattermost_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_line_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_nextcloud_talk_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_discord_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_slack_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_signal_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out);
sc_status sc_channel_fake_push_inbound(sc_channel *channel, const sc_channel_inbound *message);
sc_status sc_channel_transport_push_inbound(sc_channel *channel, const sc_channel_inbound *message);
sc_status sc_webhook_router_new(sc_allocator *alloc, sc_webhook_router **out);
sc_status sc_webhook_router_add(sc_webhook_router *router, const sc_webhook_named_options *options);
sc_status sc_webhook_router_ingest(sc_webhook_router *router,
                                   const sc_webhook_named_request *request,
                                   sc_allocator *alloc,
                                   sc_webhook_named_result *out);
void sc_webhook_named_result_clear(sc_webhook_named_result *result);
void sc_webhook_router_destroy(sc_webhook_router *router);
size_t sc_channel_fake_sent_count(const sc_channel *channel);
sc_str sc_channel_fake_last_sent_text(const sc_channel *channel);
sc_str sc_channel_fake_last_sent_attachment_content_type(const sc_channel *channel);
sc_str sc_channel_fake_last_sent_attachment_filename(const sc_channel *channel);
sc_buf sc_channel_fake_last_sent_attachment_bytes(const sc_channel *channel);
void sc_channel_fake_set_approval(sc_channel *channel, sc_channel_approval_decision decision);

sc_status sc_channel_orchestrator_new(sc_allocator *alloc,
                                      const sc_channel_orchestrator_options *options,
                                      sc_channel_orchestrator **out);
sc_status sc_channel_orchestrator_process(sc_channel_orchestrator *orchestrator,
                                          sc_channel *channel,
                                          const sc_channel_inbound *message,
                                          sc_allocator *alloc,
                                          sc_channel_process_result *out);
sc_status sc_channel_orchestrator_poll(sc_channel_orchestrator *orchestrator,
                                       sc_channel *channel,
                                       sc_allocator *alloc,
                                       sc_channel_process_result *out);
sc_status sc_channel_orchestrator_request_approval(sc_channel_orchestrator *orchestrator,
                                                   sc_channel *channel,
                                                   const sc_channel_inbound *message,
                                                   sc_str summary,
                                                   sc_allocator *alloc,
                                                   sc_channel_approval_response *out);
sc_status sc_channel_orchestrator_delivery_new(sc_allocator *alloc,
                                              sc_channel_orchestrator *orchestrator,
                                              sc_delivery_target **out);
size_t sc_channel_orchestrator_history_len(const sc_channel_orchestrator *orchestrator,
                                           sc_str channel_name,
                                           sc_str sender_id,
                                           sc_str thread_id);
void sc_channel_process_result_clear(sc_channel_process_result *result);
void sc_channel_orchestrator_destroy(sc_channel_orchestrator *orchestrator);

SC_END_DECLS
