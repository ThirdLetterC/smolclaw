// cppcheck-suppress-file redundantInitialization
#include "sc/channel.h"
#include "sc/buffer.h"
#include "sc/media.h"
#include "sc/time.h"

#include "runtime/channel_policy.h"

#include "net/curl_global.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SC_HAVE_PARSON
#include "parson/parson.h"
#endif

#include <uv.h>

#ifdef SC_HAVE_RABBITMQ
#include "rabbitmq/amqp.h"
#endif

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

#ifdef SC_HAVE_SQLITE
#include <sqlite3.h>
#endif

typedef struct sc_contract_handle {
    // cppcheck-suppress unusedStructMember
    sc_allocator *alloc;
    const void *vtab;
    void *impl;
} sc_contract_handle;

struct sc_channel {
    sc_contract_handle base;
};

typedef struct stored_inbound {
    sc_string message_id;
    sc_string channel_name;
    sc_string platform_account_id;
    sc_string conversation_id;
    sc_string thread_id;
    sc_string sender_id;
    sc_string text;
    sc_string reply_to_message_id;
    sc_string attachment_media_type;
    sc_string attachment_name;
    sc_string attachment_storage_path;
    size_t attachment_size_bytes;
    int64_t timestamp_unix_secs;
    bool timestamp_trusted;
    bool attachment_temporary;
    bool cancel_previous;
    bool disable_memory;
} stored_inbound;

typedef struct stored_outbound {
    sc_string text;
    sc_string attachment_content_type;
    sc_string attachment_filename;
    sc_bytes attachment_bytes;
    sc_attachment_delivery attachment_delivery;
} stored_outbound;

typedef struct fake_channel {
    sc_allocator *alloc;
    uint64_t capabilities;
    sc_vec inbox;
    sc_vec sent;
    sc_channel_approval_decision approval;
} fake_channel;

typedef struct cli_channel {
    sc_allocator *alloc;
} cli_channel;

typedef struct transport_channel {
    sc_allocator *alloc;
    const sc_channel_vtab *vtab;
    sc_string endpoint;
    sc_string topic;
    sc_string queue;
    sc_string client_id;
    sc_vec inbox;
    sc_vec sent;
} transport_channel;

typedef struct webhook_channel {
    sc_allocator *alloc;
    sc_string bind;
    sc_string path;
    sc_string auth_token;
    sc_string media_temp_dir;
    size_t max_body_bytes;
    size_t max_media_bytes;
    uint32_t rate_limit_per_minute;
    uint64_t replay_window_secs;
    int64_t rate_window_unix_secs;
    uint32_t rate_window_count;
    sc_vec allowed_ips;
    sc_vec seen_nonces;
    sc_vec inbox;
    uv_loop_t *loop;
    uv_async_t async;
    bool async_initialized;
} webhook_channel;

typedef struct rabbitmq_channel {
    sc_allocator *alloc;
    sc_string url;
    sc_string exchange;
    sc_string routing_key;
    sc_string queue;
    sc_string consumer_tag;
    uint16_t prefetch;
    bool durable;
    sc_vec inbox;
    sc_vec sent;
} rabbitmq_channel;

typedef struct mail_channel {
    sc_allocator *alloc;
    sc_string inbox_url;
    sc_string smtp_url;
    sc_string username;
    sc_string password;
    sc_string from;
    sc_string to;
    sc_string mailbox;
    sc_string oauth_token;
    sc_string subject_prefix;
    sc_string attachment_dir;
    sc_vec allowed_senders;
    long poll_interval_seconds;
    size_t max_message_bytes;
    bool delete_after_read;
    sc_mail_request_fn request;
    void *request_user;
    sc_vec inbox;
    sc_vec sent;
} mail_channel;

#ifdef SC_HAVE_LIBCURL
typedef struct mail_curl_buffer {
    sc_bytes bytes;
    size_t max_bytes;
    bool too_large;
} mail_curl_buffer;

typedef struct mail_upload {
    const char *ptr;
    size_t len;
    size_t off;
} mail_upload;
#endif

typedef struct history_scope {
    sc_string channel_name;
    sc_string sender_id;
    sc_string thread_id;
    sc_history history;
} history_scope;

typedef struct channel_common_rule {
    sc_string channel_name;
    bool enabled;
    bool enabled_set;
    sc_vec allowed_users;
    sc_vec allowed_destinations;
    bool reply_to_mentions_only;
    sc_string mention_token;
    sc_string provider;
    sc_string model;
    bool autonomy_level_set;
    sc_autonomy_level autonomy_level;
    uint64_t draft_update_interval_ms;
    sc_vec tools_allow;
    sc_vec tools_deny;
    bool pairing_required;
    bool paired;
} channel_common_rule;

struct sc_channel_orchestrator {
    sc_allocator *alloc;
    sc_agent *agent;
    sc_observer *observer;
    sc_vec channels;
    sc_vec allowed_senders;
    sc_vec common_rules;
    sc_vec runtime_model_routes;
    sc_vec seen_message_ids;
    sc_vec histories;
    size_t max_history_messages;
    size_t max_seen_message_ids;
    bool session_persistence;
    sc_string session_db_path;
    sc_string session_jsonl_dir;
    bool ack_reactions;
    bool show_tool_calls;
    bool interrupt_on_new_message;
    sc_string stream_mode;
    uint64_t approval_timeout_secs;
    sc_transcriber *transcriber;
    sc_tts *tts;
    sc_channel_tts_reply_mode tts_reply_mode;
    sc_string tts_voice;
};

typedef struct channel_delivery_impl {
    sc_allocator *alloc;
    sc_channel_orchestrator *orchestrator;
} channel_delivery_impl;

typedef struct channel_tool_approval_context {
    sc_channel_orchestrator *orchestrator;
    sc_channel *channel;
    const sc_channel_inbound *message;
} channel_tool_approval_context;

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_str empty_if_null(sc_str value);
static void stored_inbound_clear(stored_inbound *message);
static sc_status stored_inbound_from_public(sc_allocator *alloc, const sc_channel_inbound *message, stored_inbound *out);
static sc_status public_inbound_from_stored(sc_allocator *alloc, stored_inbound *message, sc_channel_inbound *out);
static void stored_outbound_clear(stored_outbound *message);
static sc_status stored_outbound_from_public(sc_allocator *alloc, const sc_channel_message *message, stored_outbound *out);
static sc_status string_vec_copy(sc_allocator *alloc, const sc_str *values, size_t count, sc_vec *out);
static void string_vec_clear(sc_vec *vec);
static bool string_vec_contains(const sc_vec *vec, sc_str value);
static bool string_vec_contains_wildcard_or_value(const sc_vec *vec, sc_str value);
static bool text_contains(sc_str text, sc_str needle);
static bool common_config_allows_values(bool enabled_set,
                                        bool enabled,
                                        const sc_vec *allowed_users,
                                        const sc_vec *allowed_destinations,
                                        bool reply_to_mentions_only,
                                        sc_str mention_token,
                                        bool pairing_required,
                                        bool paired,
                                        const sc_channel_inbound *event);
static sc_status common_rule_copy(sc_allocator *alloc, const sc_channel_common_config *config, channel_common_rule *out);
static void common_rule_clear(channel_common_rule *rule);
static const channel_common_rule *common_rule_for(const sc_channel_orchestrator *orchestrator, sc_str channel_name);
static sc_status common_rule_tools_as_views(const channel_common_rule *rule,
                                            sc_allocator *alloc,
                                            sc_str **allow_out,
                                            size_t *allow_count_out,
                                            sc_str **deny_out,
                                            size_t *deny_count_out);
static sc_status webhook_check_replay(webhook_channel *webhook, sc_str nonce, int64_t timestamp_unix_secs);
static sc_status webhook_check_rate_limit(webhook_channel *webhook);
static sc_status webhook_remember_nonce(webhook_channel *webhook, sc_str nonce);
static void webhook_forget_nonce_at(webhook_channel *webhook, size_t index);
[[maybe_unused]] static sc_status webhook_store_media(sc_allocator *alloc,
                                     sc_str media_temp_dir,
                                     sc_str attachment_id,
                                     sc_str content_type,
                                     sc_str filename,
                                     sc_str data_base64,
                                     size_t max_media_bytes,
                                     sc_string *storage_path,
                                     size_t *size_bytes);
static sc_status webhook_screen_media_attachment(sc_allocator *alloc,
                                                 const sc_media_attachment *attachment,
                                                 const sc_media_limits *limits);
static sc_status base64_decode(sc_allocator *alloc, sc_str input, sc_bytes *out);
static int base64_value(char ch);
static sc_str detect_media_type(sc_buf bytes);
static bool media_type_matches(sc_str declared_type, sc_str detected_type);
static void history_scope_clear(history_scope *scope);
static void runtime_model_route_clear(sc_runtime_channel_provider_route_entry *route);
static bool message_seen(const sc_channel_orchestrator *orchestrator, sc_str message_id);
static sc_status remember_message(sc_channel_orchestrator *orchestrator, sc_str message_id);
static void forget_seen_message_at(sc_channel_orchestrator *orchestrator, size_t index);
static bool sender_allowed(const sc_channel_orchestrator *orchestrator, sc_str sender_id);
static bool mail_sender_allowed(const mail_channel *mail, sc_str sender);
static sc_status mail_apply_filters(mail_channel *mail, sc_channel_inbound *inbound);
static sc_status mail_store_attachment_metadata(mail_channel *mail, sc_str raw, sc_channel_inbound *inbound);
static sc_status history_for(sc_channel_orchestrator *orchestrator, const sc_channel_inbound *message, history_scope **out);
static sc_status history_for_values(sc_channel_orchestrator *orchestrator,
                                    sc_str channel_name,
                                    sc_str sender_id,
                                    sc_str thread,
                                    history_scope **out);
static sc_status emit_event(sc_channel_orchestrator *orchestrator, sc_str name, sc_str status_text);
static sc_status send_reply(sc_channel *channel, const sc_channel_inbound *inbound, sc_str text, bool draft);
static sc_status send_attachment_reply(sc_channel *channel,
                                       const sc_channel_inbound *inbound,
                                       const sc_agent_turn_result *result);
static sc_status request_channel_tool_approval(void *user_data,
                                               sc_str tool_name,
                                               sc_str arguments_json,
                                               sc_allocator *alloc,
                                               bool *out_approved);
static sc_status build_tool_approval_summary(sc_allocator *alloc, sc_str tool_name, sc_str arguments_json, sc_string *out);
static sc_status send_audio_reply(sc_channel_orchestrator *orchestrator,
                                  sc_channel *channel,
                                  const sc_channel_inbound *inbound,
                                  sc_str text);
static sc_status build_agent_input_for_message(const sc_channel_orchestrator *orchestrator,
                                               const sc_channel_inbound *message,
                                               sc_allocator *alloc,
                                               sc_string *owned_input,
                                               sc_str *out);
static sc_status persist_session_message(sc_channel_orchestrator *orchestrator,
                                         const sc_channel_inbound *inbound,
                                         sc_str role,
                                         sc_str content);
static sc_status persist_session_jsonl(sc_channel_orchestrator *orchestrator,
                                       sc_str session_key,
                                       sc_str role,
                                       sc_str content);
static sc_status persist_session_sqlite(sc_channel_orchestrator *orchestrator,
                                        sc_str session_key,
                                        sc_str role,
                                        sc_str content);
static sc_status load_session_history(sc_channel_orchestrator *orchestrator);
static sc_status load_session_history_sqlite(sc_channel_orchestrator *orchestrator);
static sc_status history_from_session_key(sc_channel_orchestrator *orchestrator,
                                          sc_str session_key,
                                          history_scope **out);
static sc_status append_json_string(sc_string_builder *builder, sc_str value);
static sc_status build_session_key(sc_allocator *alloc, const sc_channel_inbound *inbound, sc_string *out);
static sc_status build_jsonl_path(sc_channel_orchestrator *orchestrator, sc_str session_key, sc_string *out);
static sc_status current_time_text(sc_allocator *alloc, sc_string *out);
static uint64_t channel_capabilities(const sc_channel *channel);
static sc_channel *first_channel(sc_channel_orchestrator *orchestrator);
static bool channel_is_fake(const sc_channel *channel);
static bool channel_is_transport(const sc_channel *channel);
static bool channel_is_webhook(const sc_channel *channel);
static bool channel_is_rabbitmq_vendor(const sc_channel *channel);
static bool channel_is_mail(const sc_channel *channel);
static fake_channel *fake_from_handle(sc_channel *channel);
static const fake_channel *fake_from_const_handle(const sc_channel *channel);
static transport_channel *transport_from_handle(sc_channel *channel);
static const transport_channel *transport_from_const_handle(const sc_channel *channel);
static webhook_channel *webhook_from_handle(sc_channel *channel);
static const rabbitmq_channel *rabbitmq_from_const_handle(const sc_channel *channel);
static const mail_channel *mail_from_const_handle(const sc_channel *channel);

static sc_status fake_send(void *impl, const sc_channel_message *message);
static sc_status fake_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status fake_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status fake_request_approval(void *impl,
                                       const sc_channel_approval_request *request,
                                       sc_allocator *alloc,
                                       sc_channel_approval_response *out);
static void fake_destroy(void *impl);
static sc_status cli_send(void *impl, const sc_channel_message *message);
static sc_status cli_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status cli_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status cli_request_approval(void *impl,
                                      const sc_channel_approval_request *request,
                                      sc_allocator *alloc,
                                      sc_channel_approval_response *out);
static void cli_destroy(void *impl);
static sc_status transport_send(void *impl, const sc_channel_message *message);
static sc_status transport_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status transport_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status transport_request_approval(void *impl,
                                            const sc_channel_approval_request *request,
                                            sc_allocator *alloc,
                                            sc_channel_approval_response *out);
static void transport_destroy(void *impl);
static sc_status transport_new(sc_allocator *alloc,
                               const sc_channel_transport_options *options,
                               const sc_channel_vtab *vtab,
                               sc_channel **out);
static sc_status webhook_send(void *impl, const sc_channel_message *message);
static sc_status webhook_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status webhook_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status webhook_request_approval(void *impl,
                                          const sc_channel_approval_request *request,
                                          sc_allocator *alloc,
                                          sc_channel_approval_response *out);
static void webhook_destroy(void *impl);
static sc_status rabbitmq_send(void *impl, const sc_channel_message *message);
static sc_status rabbitmq_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status rabbitmq_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status rabbitmq_request_approval(void *impl,
                                           const sc_channel_approval_request *request,
                                           sc_allocator *alloc,
                                           sc_channel_approval_response *out);
static void rabbitmq_destroy(void *impl);
static sc_status mail_send(void *impl, const sc_channel_message *message);
static sc_status mail_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status mail_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status mail_request_approval(void *impl,
                                       const sc_channel_approval_request *request,
                                       sc_allocator *alloc,
                                       sc_channel_approval_response *out);
static void mail_destroy(void *impl);
static sc_status mail_fetch(mail_channel *mail, sc_allocator *alloc, sc_string *out);
static sc_status mail_deliver(mail_channel *mail, sc_str payload, sc_allocator *alloc, sc_string *out);
static sc_status mail_parse_message(sc_allocator *alloc, sc_str raw, sc_str inbox_url, sc_channel_inbound *out);
static sc_status mail_build_payload(mail_channel *mail, const sc_channel_message *message, sc_allocator *alloc, sc_string *out);
static sc_str mail_header_value(sc_str raw, const char *name);
static sc_str mail_body(sc_str raw);
static sc_str trim_ascii(sc_str value);
#ifdef SC_HAVE_LIBCURL
static sc_status mail_curl_request(mail_channel *mail,
                                   sc_str operation,
                                   sc_str url,
                                   sc_str payload,
                                   sc_allocator *alloc,
                                   sc_string *out);
static size_t mail_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
static size_t mail_curl_read_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
#endif
static sc_status queue_listen(sc_vec *inbox, sc_allocator *owner_alloc, sc_channel_inbound *out, const char *empty_code);
static sc_status parse_webhook_body(sc_allocator *alloc,
                                    sc_str body,
                                    sc_str remote_addr,
                                    size_t max_media_bytes,
                                    sc_str media_temp_dir,
                                    sc_string *nonce,
                                    int64_t *timestamp_unix_secs,
                                    sc_channel_inbound *out);
static void webhook_async_cb(uv_async_t *handle);
static sc_status channel_delivery_deliver(void *impl, const sc_delivery_message *message);
static void channel_delivery_destroy(void *impl);

static const char *const webhook_required_secret_keys[] = {"auth_token"};
static const char *const mail_required_secret_keys[] = {"password"};

static const sc_channel_vtab fake_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "fake",
    .display_name = "Fake channel",
    .feature_flag = "SC_CHANNEL_FAKE",
    .description_key = "channel.fake.description",
    .config_schema_ref = "sc.schema.channels.fake",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_DRAFT_UPDATES | SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM |
                    SC_CHANNEL_CAP_APPROVAL_PROMPTS | SC_CHANNEL_CAP_ATTACHMENTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = fake_send,
    .listen = fake_listen,
    .health = fake_health,
    .request_approval = fake_request_approval,
    .destroy = fake_destroy,
};

static const sc_channel_vtab cli_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "cli",
    .display_name = "CLI channel",
    .feature_flag = "SC_CHANNEL_CLI",
    .description_key = "channel.cli.description",
    .config_schema_ref = "sc.schema.channels.cli",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_APPROVAL_PROMPTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = cli_send,
    .listen = cli_listen,
    .health = cli_health,
    .request_approval = cli_request_approval,
    .destroy = cli_destroy,
};

static const sc_channel_vtab websocket_client_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "websocket-client",
    .display_name = "WebSocket client channel",
    .feature_flag = "SC_CHANNEL_WEBSOCKET_CLIENT",
    .description_key = "channel.websocket_client.description",
    .config_schema_ref = "sc.schema.channels.websocket_client",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const sc_channel_vtab mqtt_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mqtt",
    .display_name = "MQTT channel",
    .feature_flag = "SC_CHANNEL_MQTT",
    .description_key = "channel.mqtt.description",
    .config_schema_ref = "sc.schema.channels.mqtt",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const sc_channel_vtab rabbitmq_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "rabbitmq",
    .display_name = "RabbitMQ channel",
    .feature_flag = "SC_CHANNEL_RABBITMQ",
    .description_key = "channel.rabbitmq.description",
    .config_schema_ref = "sc.schema.channels.rabbitmq",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const sc_channel_vtab webhook_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "webhook",
    .display_name = "Webhook channel",
    .feature_flag = "SC_CHANNEL_WEBHOOK",
    .description_key = "channel.webhook.description",
    .config_schema_ref = "sc.schema.channels.webhook",
    .required_secret_keys = webhook_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(webhook_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA,
    .media_capabilities = SC_CHANNEL_MEDIA_IMAGE | SC_CHANNEL_MEDIA_AUDIO | SC_CHANNEL_MEDIA_VIDEO | SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = webhook_send,
    .listen = webhook_listen,
    .health = webhook_health,
    .request_approval = webhook_request_approval,
    .destroy = webhook_destroy,
};

static const sc_channel_vtab rabbitmq_vendor_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "rabbitmq",
    .display_name = "RabbitMQ channel",
    .feature_flag = "SC_CHANNEL_RABBITMQ",
    .description_key = "channel.rabbitmq.description",
    .config_schema_ref = "sc.schema.channels.rabbitmq",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = rabbitmq_send,
    .listen = rabbitmq_listen,
    .health = rabbitmq_health,
    .request_approval = rabbitmq_request_approval,
    .destroy = rabbitmq_destroy,
};

static const sc_channel_vtab mail_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mail",
    .display_name = "Mail channel",
    .feature_flag = "SC_CHANNEL_MAIL",
    .description_key = "channel.mail.description",
    .config_schema_ref = "sc.schema.channels.mail",
    .required_secret_keys = mail_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(mail_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = mail_send,
    .listen = mail_listen,
    .health = mail_health,
    .request_approval = mail_request_approval,
    .destroy = mail_destroy,
};

static const char *const matrix_required_secret_keys[] = {"access_token"};
static const sc_channel_vtab matrix_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "matrix",
    .display_name = "Matrix channel",
    .feature_flag = "SC_CHANNEL_MATRIX",
    .description_key = "channel.matrix.description",
    .config_schema_ref = "sc.schema.channels.matrix",
    .required_secret_keys = matrix_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(matrix_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA,
    .media_capabilities = SC_CHANNEL_MEDIA_IMAGE | SC_CHANNEL_MEDIA_AUDIO | SC_CHANNEL_MEDIA_VIDEO | SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_REACTIONS |
                    SC_CHANNEL_CAP_ATTACHMENTS | SC_CHANNEL_CAP_THREAD_REPLIES,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const char *const mattermost_required_secret_keys[] = {"access_token"};
static const sc_channel_vtab mattermost_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mattermost",
    .display_name = "Mattermost channel",
    .feature_flag = "SC_CHANNEL_MATTERMOST",
    .description_key = "channel.mattermost.description",
    .config_schema_ref = "sc.schema.channels.mattermost",
    .required_secret_keys = mattermost_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(mattermost_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA,
    .media_capabilities = SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_DRAFT_UPDATES | SC_CHANNEL_CAP_ATTACHMENTS | SC_CHANNEL_CAP_THREAD_REPLIES,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const char *const line_required_secret_keys[] = {"channel_secret", "access_token"};
static const sc_channel_vtab line_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "line",
    .display_name = "LINE channel",
    .feature_flag = "SC_CHANNEL_LINE",
    .description_key = "channel.line.description",
    .config_schema_ref = "sc.schema.channels.line",
    .required_secret_keys = line_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(line_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA | SC_CHANNEL_INBOUND_CALLBACK,
    .media_capabilities = SC_CHANNEL_MEDIA_AUDIO | SC_CHANNEL_MEDIA_IMAGE | SC_CHANNEL_MEDIA_VIDEO,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_ATTACHMENTS | SC_CHANNEL_CAP_APPROVAL_PROMPTS |
                    SC_CHANNEL_CAP_INLINE_APPROVAL_BUTTONS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const char *const nextcloud_talk_required_secret_keys[] = {"webhook_secret", "app_password"};
static const sc_channel_vtab nextcloud_talk_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "nextcloud-talk",
    .display_name = "Nextcloud Talk channel",
    .feature_flag = "SC_CHANNEL_NEXTCLOUD_TALK",
    .description_key = "channel.nextcloud_talk.description",
    .config_schema_ref = "sc.schema.channels.nextcloud_talk",
    .required_secret_keys = nextcloud_talk_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(nextcloud_talk_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const char *const discord_required_secret_keys[] = {"bot_token"};
static const sc_channel_vtab discord_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "discord",
    .display_name = "Discord channel",
    .feature_flag = "SC_CHANNEL_DISCORD",
    .description_key = "channel.discord.description",
    .config_schema_ref = "sc.schema.channels.discord",
    .required_secret_keys = discord_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(discord_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA,
    .media_capabilities = SC_CHANNEL_MEDIA_IMAGE | SC_CHANNEL_MEDIA_AUDIO | SC_CHANNEL_MEDIA_VIDEO | SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_DRAFT_UPDATES | SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_ATTACHMENTS |
                    SC_CHANNEL_CAP_APPROVAL_PROMPTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const char *const slack_required_secret_keys[] = {"bot_token", "signing_secret"};
static const sc_channel_vtab slack_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "slack",
    .display_name = "Slack channel",
    .feature_flag = "SC_CHANNEL_SLACK",
    .description_key = "channel.slack.description",
    .config_schema_ref = "sc.schema.channels.slack",
    .required_secret_keys = slack_required_secret_keys,
    .required_secret_key_count = SC_ARRAY_LEN(slack_required_secret_keys),
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_CALLBACK,
    .media_capabilities = SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_THREAD_REPLIES | SC_CHANNEL_CAP_APPROVAL_PROMPTS |
                    SC_CHANNEL_CAP_INLINE_APPROVAL_BUTTONS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const sc_channel_vtab signal_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "signal",
    .display_name = "Signal channel",
    .feature_flag = "SC_CHANNEL_SIGNAL",
    .description_key = "channel.signal.description",
    .config_schema_ref = "sc.schema.channels.signal",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT | SC_CHANNEL_INBOUND_MEDIA,
    .media_capabilities = SC_CHANNEL_MEDIA_IMAGE | SC_CHANNEL_MEDIA_AUDIO | SC_CHANNEL_MEDIA_VIDEO | SC_CHANNEL_MEDIA_DOCUMENT,
    .capabilities = SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM | SC_CHANNEL_CAP_ATTACHMENTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = transport_send,
    .listen = transport_listen,
    .health = transport_health,
    .request_approval = transport_request_approval,
    .destroy = transport_destroy,
};

static const sc_delivery_vtab channel_delivery_vtab = {
    .struct_size = sizeof(sc_delivery_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "channel-delivery",
    .deliver = channel_delivery_deliver,
    .destroy = channel_delivery_destroy,
};

void sc_channel_inbound_clear(sc_channel_inbound *message)
{
    if (message == nullptr) {
        return;
    }
    sc_string_clear(&message->owned_message_id);
    sc_string_clear(&message->owned_channel_name);
    sc_string_clear(&message->owned_platform_account_id);
    sc_string_clear(&message->owned_conversation_id);
    sc_string_clear(&message->owned_thread_id);
    sc_string_clear(&message->owned_sender_id);
    sc_string_clear(&message->owned_text);
    sc_string_clear(&message->owned_reply_to_message_id);
    sc_string_clear(&message->owned_attachment_media_type);
    sc_string_clear(&message->owned_attachment_name);
    if (message->attachment_temporary && message->owned_attachment_storage_path.ptr != nullptr) {
        (void)unlink(message->owned_attachment_storage_path.ptr);
    }
    sc_string_clear(&message->owned_attachment_storage_path);
    *message = (sc_channel_inbound){0};
}

void sc_channel_health_clear(sc_channel_health *health)
{
    if (health == nullptr) {
        return;
    }
    sc_string_clear(&health->message);
    *health = (sc_channel_health){0};
}

void sc_channel_approval_response_clear(sc_channel_approval_response *response)
{
    if (response == nullptr) {
        return;
    }
    sc_string_clear(&response->reason);
    *response = (sc_channel_approval_response){0};
}

sc_status sc_channel_websocket_client_new(sc_allocator *alloc, const sc_channel_transport_options *options, sc_channel **out)
{
    return transport_new(alloc, options, &websocket_client_vtab, out);
}

sc_status sc_channel_mqtt_new(sc_allocator *alloc, const sc_channel_transport_options *options, sc_channel **out)
{
    return transport_new(alloc, options, &mqtt_vtab, out);
}

sc_status sc_channel_rabbitmq_new(sc_allocator *alloc, const sc_channel_transport_options *options, sc_channel **out)
{
    return transport_new(alloc, options, &rabbitmq_vtab, out);
}

static sc_status platform_channel_new(sc_allocator *alloc,
                                      const sc_platform_channel_options *options,
                                      const sc_channel_vtab *vtab,
                                      sc_channel **out)
{
    sc_channel_transport_options transport = {0};

    if (options == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.channel_platform.invalid_argument");
    }
    if (vtab == &matrix_vtab && options->e2ee_enabled && options->device_id.len == 0) {
        return sc_status_invalid_argument("sc.channel_matrix.device_id_required");
    }
    transport = (sc_channel_transport_options){
        .struct_size = sizeof(transport),
        .endpoint = options->endpoint,
        .topic = options->bot_display_name,
        .queue = options->bot_user_id,
        .client_id = options->bot_user_id,
    };
    return transport_new(alloc, &transport, vtab, out);
}

sc_status sc_channel_matrix_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &matrix_vtab, out);
}

sc_status sc_channel_mattermost_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &mattermost_vtab, out);
}

sc_status sc_channel_line_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &line_vtab, out);
}

sc_status sc_channel_nextcloud_talk_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &nextcloud_talk_vtab, out);
}

sc_status sc_channel_discord_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &discord_vtab, out);
}

sc_status sc_channel_slack_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &slack_vtab, out);
}

sc_status sc_channel_signal_new(sc_allocator *alloc, const sc_platform_channel_options *options, sc_channel **out)
{
    return platform_channel_new(alloc, options, &signal_vtab, out);
}

sc_status sc_channel_webhook_new(sc_allocator *alloc, const sc_webhook_channel_options *options, sc_channel **out)
{
    webhook_channel *webhook = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || options->path.len == 0 || options->max_body_bytes > SIZE_MAX / 2u) {
        return sc_status_invalid_argument("sc.channel_webhook.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    webhook = sc_alloc(alloc, sizeof(*webhook), _Alignof(webhook_channel));
    if (webhook == nullptr) {
        return sc_status_no_memory();
    }
    *webhook = (webhook_channel){
        .alloc = alloc,
        .max_body_bytes = options->max_body_bytes == 0 ? 65536U : options->max_body_bytes,
        .max_media_bytes = options->max_media_bytes,
        .rate_limit_per_minute = options->rate_limit_per_minute,
        .replay_window_secs = options->replay_window_secs == 0 ? 300U : options->replay_window_secs,
    };
    sc_vec_init(&webhook->allowed_ips, alloc, sizeof(sc_string));
    sc_vec_init(&webhook->seen_nonces, alloc, sizeof(sc_string));
    sc_vec_init(&webhook->inbox, alloc, sizeof(stored_inbound));
    status = copy_string(alloc, options->bind, &webhook->bind);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->path, &webhook->path);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->auth_token, &webhook->auth_token);
    }
    if (sc_status_is_ok(status)) {
        sc_str media_temp_dir = options->media_temp_dir.len == 0 ? sc_str_from_cstr("/tmp") : options->media_temp_dir;
        status = copy_string(alloc, media_temp_dir, &webhook->media_temp_dir);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, options->allowed_ips, options->allowed_ip_count, &webhook->allowed_ips);
    }
    if (sc_status_is_ok(status)) {
        webhook->loop = options->uv_loop == nullptr ? uv_default_loop() : options->uv_loop;
        if (webhook->loop != nullptr && uv_async_init(webhook->loop, &webhook->async, webhook_async_cb) == 0) {
            webhook->async.data = webhook;
            webhook->async_initialized = true;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_channel_new(alloc, &webhook_vtab, webhook, out);
    }
    if (!sc_status_is_ok(status)) {
        webhook_destroy(webhook);
    }
    return status;
}

sc_status sc_channel_webhook_ingest(sc_channel *channel, const sc_webhook_ingest_request *request)
{
    webhook_channel *webhook = webhook_from_handle(channel);
    sc_channel_inbound inbound = {0};
    stored_inbound stored = {0};
    sc_string nonce = {0};
    int64_t timestamp_unix_secs = 0;
    sc_status status = sc_status_ok();

    if (webhook == nullptr || request == nullptr || request->body.ptr == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.ingest_invalid_argument");
    }
    if (!sc_str_equal(empty_if_null(request->method), sc_str_from_cstr("POST")) ||
        !sc_str_equal(empty_if_null(request->path), sc_string_as_str(&webhook->path))) {
        return sc_status_invalid_argument("sc.channel_webhook.request_rejected");
    }
    if (webhook->auth_token.len > 0 && !sc_str_equal(empty_if_null(request->auth_token), sc_string_as_str(&webhook->auth_token))) {
        return sc_status_security_denied("sc.channel_webhook.auth_denied");
    }
    if (request->body.len > webhook->max_body_bytes) {
        return sc_status_invalid_argument("sc.channel_webhook.body_too_large");
    }
    if (!string_vec_contains_wildcard_or_value(&webhook->allowed_ips, request->remote_addr)) {
        return sc_status_security_denied("sc.channel_webhook.ip_denied");
    }
    status = webhook_check_rate_limit(webhook);
    if (sc_status_is_ok(status)) {
        status = parse_webhook_body(webhook->alloc,
                                    request->body,
                                    request->remote_addr,
                                    webhook->max_media_bytes,
                                    sc_string_as_str(&webhook->media_temp_dir),
                                    &nonce,
                                    &timestamp_unix_secs,
                                    &inbound);
    }
    if (sc_status_is_ok(status)) {
        status = webhook_check_replay(webhook, sc_string_as_str(&nonce), timestamp_unix_secs);
    }
    if (sc_status_is_ok(status)) {
        status = stored_inbound_from_public(webhook->alloc, &inbound, &stored);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&webhook->inbox, &stored);
    }
    if (sc_status_is_ok(status)) {
        inbound.attachment_temporary = false;
        status = webhook_remember_nonce(webhook, sc_string_as_str(&nonce));
    }
    if (!sc_status_is_ok(status)) {
        stored_inbound_clear(&stored);
    }
    if (sc_status_is_ok(status) && webhook->async_initialized) {
        (void)uv_async_send(&webhook->async);
    }
    sc_channel_inbound_clear(&inbound);
    sc_string_clear(&nonce);
    return status;
}

sc_status sc_channel_rabbitmq_vendor_new(sc_allocator *alloc, const sc_rabbitmq_channel_options *options, sc_channel **out)
{
    rabbitmq_channel *rabbitmq = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || options->url.len == 0 || options->queue.len == 0) {
        return sc_status_invalid_argument("sc.channel_rabbitmq.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    rabbitmq = sc_alloc(alloc, sizeof(*rabbitmq), _Alignof(rabbitmq_channel));
    if (rabbitmq == nullptr) {
        return sc_status_no_memory();
    }
    *rabbitmq = (rabbitmq_channel){
        .alloc = alloc,
        .prefetch = options->prefetch == 0 ? 1 : options->prefetch,
        .durable = options->durable,
    };
    sc_vec_init(&rabbitmq->inbox, alloc, sizeof(stored_inbound));
    sc_vec_init(&rabbitmq->sent, alloc, sizeof(sc_string));
    status = copy_string(alloc, options->url, &rabbitmq->url);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->exchange, &rabbitmq->exchange);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->routing_key, &rabbitmq->routing_key);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->queue, &rabbitmq->queue);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->consumer_tag, &rabbitmq->consumer_tag);
    }
    if (sc_status_is_ok(status)) {
        status = sc_channel_new(alloc, &rabbitmq_vendor_vtab, rabbitmq, out);
    }
    if (!sc_status_is_ok(status)) {
        rabbitmq_destroy(rabbitmq);
    }
    return status;
}

sc_status sc_channel_mail_new(sc_allocator *alloc, const sc_mail_channel_options *options, sc_channel **out)
{
    mail_channel *mail = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || options->inbox_url.len == 0) {
        return sc_status_invalid_argument("sc.channel_mail.invalid_argument");
    }
#ifndef SC_HAVE_LIBCURL
    if (options->request == nullptr) {
        return sc_status_unsupported("sc.channel_mail.libcurl_unavailable");
    }
#endif
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    mail = sc_alloc(alloc, sizeof(*mail), _Alignof(mail_channel));
    if (mail == nullptr) {
        return sc_status_no_memory();
    }
    *mail = (mail_channel){
        .alloc = alloc,
        .max_message_bytes = options->max_message_bytes == 0 ? 1024U * 1024U : options->max_message_bytes,
        .delete_after_read = options->delete_after_read,
        .poll_interval_seconds = options->poll_interval_seconds <= 0 ? 60 : options->poll_interval_seconds,
        .request = options->request,
        .request_user = options->request_user,
    };
    sc_vec_init(&mail->inbox, alloc, sizeof(stored_inbound));
    sc_vec_init(&mail->sent, alloc, sizeof(sc_string));
    sc_vec_init(&mail->allowed_senders, alloc, sizeof(sc_string));
    status = copy_string(alloc, options->inbox_url, &mail->inbox_url);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->smtp_url, &mail->smtp_url);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->username, &mail->username);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->password, &mail->password);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->from, &mail->from);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->to, &mail->to);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->mailbox, &mail->mailbox);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->oauth_token, &mail->oauth_token);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->subject_prefix, &mail->subject_prefix);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->attachment_dir, &mail->attachment_dir);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, options->allowed_senders, options->allowed_sender_count, &mail->allowed_senders);
    }
    if (sc_status_is_ok(status)) {
        status = sc_channel_new(alloc, &mail_vtab, mail, out);
    }
    if (!sc_status_is_ok(status)) {
        mail_destroy(mail);
    }
    return status;
}

sc_status sc_channel_cli_new(sc_allocator *alloc, sc_channel **out)
{
    cli_channel *impl = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_cli.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(cli_channel));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (cli_channel){.alloc = alloc};
    status = sc_channel_new(alloc, &cli_vtab, impl, out);
    if (!sc_status_is_ok(status)) {
        cli_destroy(impl);
    }
    return status;
}

sc_status sc_channel_fake_new(sc_allocator *alloc, uint64_t capabilities, sc_channel **out)
{
    fake_channel *impl = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_fake.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(fake_channel));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (fake_channel){.alloc = alloc, .capabilities = capabilities == 0 ? fake_vtab.capabilities : capabilities, .approval = SC_CHANNEL_APPROVAL_APPROVED};
    sc_vec_init(&impl->inbox, alloc, sizeof(stored_inbound));
    sc_vec_init(&impl->sent, alloc, sizeof(stored_outbound));
    status = sc_channel_new(alloc, &fake_vtab, impl, out);
    if (!sc_status_is_ok(status)) {
        fake_destroy(impl);
    }
    return status;
}

sc_status sc_channel_fake_push_inbound(sc_channel *channel, const sc_channel_inbound *message)
{
    fake_channel *fake = fake_from_handle(channel);
    stored_inbound stored = {0};
    sc_status status = sc_status_ok();

    if (fake == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_fake.push_invalid_argument");
    }
    status = stored_inbound_from_public(fake->alloc, message, &stored);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&fake->inbox, &stored);
    }
    if (!sc_status_is_ok(status)) {
        stored_inbound_clear(&stored);
    }
    return status;
}

sc_status sc_channel_transport_push_inbound(sc_channel *channel, const sc_channel_inbound *message)
{
    transport_channel *transport = transport_from_handle(channel);
    stored_inbound stored = {0};
    sc_status status = sc_status_ok();

    if (transport == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_transport.push_invalid_argument");
    }
    status = stored_inbound_from_public(transport->alloc, message, &stored);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&transport->inbox, &stored);
    }
    if (!sc_status_is_ok(status)) {
        stored_inbound_clear(&stored);
    }
    return status;
}

size_t sc_channel_fake_sent_count(const sc_channel *channel)
{
    const fake_channel *fake = fake_from_const_handle(channel);
    const transport_channel *transport = transport_from_const_handle(channel);
    const rabbitmq_channel *rabbitmq = rabbitmq_from_const_handle(channel);
    const mail_channel *mail = mail_from_const_handle(channel);
    if (fake != nullptr) {
        return fake->sent.len;
    }
    if (transport != nullptr) {
        return transport->sent.len;
    }
    if (rabbitmq != nullptr) {
        return rabbitmq->sent.len;
    }
    return mail == nullptr ? 0 : mail->sent.len;
}

sc_str sc_channel_fake_last_sent_text(const sc_channel *channel)
{
    const fake_channel *fake = fake_from_const_handle(channel);
    const transport_channel *transport = transport_from_const_handle(channel);
    const rabbitmq_channel *rabbitmq = rabbitmq_from_const_handle(channel);
    const mail_channel *mail = mail_from_const_handle(channel);
    const sc_string *last = nullptr;
    if (fake != nullptr) {
        const stored_outbound *outbound = nullptr;
        if (fake->sent.len == 0) {
            return sc_str_from_cstr("");
        }
        outbound = sc_vec_at_const(&fake->sent, fake->sent.len - 1);
        return sc_string_as_str(&outbound->text);
    }
    if (transport == nullptr || transport->sent.len == 0) {
        if (rabbitmq == nullptr || rabbitmq->sent.len == 0) {
            if (mail == nullptr || mail->sent.len == 0) {
                return sc_str_from_cstr("");
            }
            last = sc_vec_at_const(&mail->sent, mail->sent.len - 1);
            return sc_string_as_str(last);
        }
        last = sc_vec_at_const(&rabbitmq->sent, rabbitmq->sent.len - 1);
        return sc_string_as_str(last);
    }
    last = sc_vec_at_const(&transport->sent, transport->sent.len - 1);
    return sc_string_as_str(last);
}

sc_str sc_channel_fake_last_sent_attachment_content_type(const sc_channel *channel)
{
    const fake_channel *fake = fake_from_const_handle(channel);
    const stored_outbound *outbound = nullptr;
    if (fake == nullptr || fake->sent.len == 0) {
        return sc_str_from_cstr("");
    }
    outbound = sc_vec_at_const(&fake->sent, fake->sent.len - 1);
    return sc_string_as_str(&outbound->attachment_content_type);
}

sc_str sc_channel_fake_last_sent_attachment_filename(const sc_channel *channel)
{
    const fake_channel *fake = fake_from_const_handle(channel);
    const stored_outbound *outbound = nullptr;
    if (fake == nullptr || fake->sent.len == 0) {
        return sc_str_from_cstr("");
    }
    outbound = sc_vec_at_const(&fake->sent, fake->sent.len - 1);
    return sc_string_as_str(&outbound->attachment_filename);
}

sc_buf sc_channel_fake_last_sent_attachment_bytes(const sc_channel *channel)
{
    const fake_channel *fake = fake_from_const_handle(channel);
    const stored_outbound *outbound = nullptr;
    if (fake == nullptr || fake->sent.len == 0) {
        return sc_buf_from_parts(nullptr, 0);
    }
    outbound = sc_vec_at_const(&fake->sent, fake->sent.len - 1);
    return sc_buf_from_parts(outbound->attachment_bytes.ptr, outbound->attachment_bytes.len);
}

void sc_channel_fake_set_approval(sc_channel *channel, sc_channel_approval_decision decision)
{
    fake_channel *fake = fake_from_handle(channel);
    if (fake != nullptr) {
        fake->approval = decision;
    }
}

sc_status sc_channel_orchestrator_new(sc_allocator *alloc,
                                      const sc_channel_orchestrator_options *options,
                                      sc_channel_orchestrator **out)
{
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || options->agent == nullptr) {
        return sc_status_invalid_argument("sc.channel_orchestrator.invalid_argument");
    }
    *out = nullptr;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    orchestrator = sc_alloc(alloc, sizeof(*orchestrator), _Alignof(sc_channel_orchestrator));
    if (orchestrator == nullptr) {
        return sc_status_no_memory();
    }
    *orchestrator = (sc_channel_orchestrator){
        .alloc = alloc,
        .agent = options->agent,
        .observer = options->observer,
        .max_history_messages = options->max_history_messages == 0 ? 16 : options->max_history_messages,
        .max_seen_message_ids = options->max_seen_message_ids == 0 ? 1'024 : options->max_seen_message_ids,
        .session_persistence = options->session_persistence,
        .ack_reactions = options->ack_reactions,
        .show_tool_calls = options->show_tool_calls,
        .interrupt_on_new_message = options->interrupt_on_new_message,
        .approval_timeout_secs = options->approval_timeout_secs,
        .transcriber = options->transcriber,
        .tts = options->tts,
        .tts_reply_mode = options->tts_reply_mode,
    };
    sc_vec_init(&orchestrator->channels, alloc, sizeof(sc_channel *));
    sc_vec_init(&orchestrator->allowed_senders, alloc, sizeof(sc_string));
    sc_vec_init(&orchestrator->common_rules, alloc, sizeof(channel_common_rule));
    sc_vec_init(&orchestrator->runtime_model_routes, alloc, sizeof(sc_runtime_channel_provider_route_entry));
    sc_vec_init(&orchestrator->seen_message_ids, alloc, sizeof(sc_string));
    sc_vec_init(&orchestrator->histories, alloc, sizeof(history_scope));
    status = copy_string(alloc, options->session_db_path, &orchestrator->session_db_path);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->session_jsonl_dir, &orchestrator->session_jsonl_dir);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->stream_mode, &orchestrator->stream_mode);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->tts_voice, &orchestrator->tts_voice);
    }

    for (size_t i = 0; sc_status_is_ok(status) && i < options->channel_count; i += 1) {
        sc_channel *channel = options->channels[i];
        status = sc_vec_push(&orchestrator->channels, &channel);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < options->allowed_sender_count; i += 1) {
        sc_string sender = {0};
        status = copy_string(alloc, options->allowed_senders[i], &sender);
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(&orchestrator->allowed_senders, &sender);
        }
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&sender);
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < options->common_config_count; i += 1) {
        channel_common_rule rule = {0};
        status = common_rule_copy(alloc, &options->common_configs[i], &rule);
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(&orchestrator->common_rules, &rule);
        }
        if (!sc_status_is_ok(status)) {
            common_rule_clear(&rule);
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < options->provider_route_count; i += 1) {
        sc_runtime_channel_provider_route_entry route = {0};
        status = copy_string(alloc, options->provider_routes[i].channel_name, &route.channel_name);
        if (sc_status_is_ok(status)) {
            status = copy_string(alloc, options->provider_routes[i].provider_name, &route.provider_name);
        }
        if (sc_status_is_ok(status)) {
            status = copy_string(alloc, options->provider_routes[i].model, &route.model);
        }
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(&orchestrator->runtime_model_routes, &route);
        }
        if (!sc_status_is_ok(status)) {
            runtime_model_route_clear(&route);
        }
    }
    if (sc_status_is_ok(status)) {
        status = load_session_history(orchestrator);
    }
    if (!sc_status_is_ok(status)) {
        sc_channel_orchestrator_destroy(orchestrator);
        return status;
    }
    *out = orchestrator;
    return sc_status_ok();
}

sc_status sc_channel_orchestrator_process(sc_channel_orchestrator *orchestrator,
                                          sc_channel *channel,
                                          const sc_channel_inbound *message,
                                          sc_allocator *alloc,
                                          sc_channel_process_result *out)
{
    history_scope *history = nullptr;
    sc_agent_turn_result agent_result = {0};
    sc_status status = sc_status_ok();
    bool can_draft = false;
    sc_model_switch_request model_switch = {0};
    sc_model_switch_request *model_switch_ptr = nullptr;
    const channel_common_rule *common = nullptr;
    sc_str *tools_allow = nullptr;
    sc_str *tools_deny = nullptr;
    size_t tools_allow_count = 0;
    size_t tools_deny_count = 0;
    sc_string agent_input_storage = {0};
    sc_str agent_input = {0};
    bool agent_input_attempted = false;

    if (orchestrator == nullptr || channel == nullptr || message == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_orchestrator.process_invalid_argument");
    }
    can_draft = (channel_capabilities(channel) & SC_CHANNEL_CAP_DRAFT_UPDATES) != 0;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_channel_process_result){.struct_size = sizeof(*out)};
    if (message_seen(orchestrator, message->message_id)) {
        out->duplicate = true;
        return sc_status_ok();
    }
    status = remember_message(orchestrator, message->message_id);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    common = common_rule_for(orchestrator, message->channel_name);
    if (common != nullptr && !common_config_allows_values(common->enabled_set,
                                                          common->enabled,
                                                          &common->allowed_users,
                                                          &common->allowed_destinations,
                                                          common->reply_to_mentions_only,
                                                          sc_string_as_str(&common->mention_token),
                                                          common->pairing_required,
                                                          common->paired,
                                                          message)) {
        out->denied = true;
        (void)emit_event(orchestrator, sc_str_from_cstr("channel.denied"), message->sender_id);
        return sc_status_ok();
    }
    if (!sender_allowed(orchestrator, message->sender_id)) {
        out->denied = true;
        (void)emit_event(orchestrator, sc_str_from_cstr("channel.denied"), message->sender_id);
        return sc_status_ok();
    }
    status = history_for(orchestrator, message, &history);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    out->cancelled_previous = message->cancel_previous && history->history.messages.len > 0;
    if (can_draft) {
        status = send_reply(channel, message, sc_str_from_cstr("..."), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_runtime_channel_model_switch_for(&orchestrator->runtime_model_routes, message->channel_name, &model_switch);
    }
    if (sc_status_is_ok(status) && (model_switch.model.len == 0) && common != nullptr && common->model.len > 0) {
        model_switch = (sc_model_switch_request){
            .struct_size = sizeof(model_switch),
            .provider_name = sc_string_as_str(&common->provider),
            .model = sc_string_as_str(&common->model),
            .reason = message->channel_name,
        };
    }
    if (sc_status_is_ok(status)) {
        status = common_rule_tools_as_views(common,
                                            alloc,
                                            &tools_allow,
                                            &tools_allow_count,
                                            &tools_deny,
                                            &tools_deny_count);
    }
    if (sc_status_is_ok(status)) {
        agent_input_attempted = true;
        status = build_agent_input_for_message(orchestrator, message, alloc, &agent_input_storage, &agent_input);
    }
    if (agent_input_attempted && !sc_status_is_ok(status)) {
        (void)send_reply(channel,
                         message,
                         sc_str_from_cstr("Sorry, I couldn't transcribe that audio message."),
                         false);
        (void)emit_event(orchestrator, sc_str_from_cstr("channel.asr_failed"), message->message_id);
        sc_string_clear(&agent_input_storage);
        sc_free(alloc, tools_allow, tools_allow_count * sizeof(*tools_allow), _Alignof(sc_str));
        sc_free(alloc, tools_deny, tools_deny_count * sizeof(*tools_deny), _Alignof(sc_str));
        return status;
    }
    if (sc_status_is_ok(status)) {
        channel_tool_approval_context approval_context = {
            .orchestrator = orchestrator,
            .channel = channel,
            .message = message,
        };
        if (model_switch.model.len > 0) {
            model_switch_ptr = &model_switch;
        }
        sc_turn turn = {
            .struct_size = sizeof(sc_turn),
            .input = agent_input,
            .session_id = message->thread_id.len > 0 ? message->thread_id : message->conversation_id,
            .cancel_requested = false,
            .allowed_tools = tools_allow,
            .allowed_tool_count = tools_allow_count,
            .denied_tools = tools_deny,
            .denied_tool_count = tools_deny_count,
            .autonomy_override_set = common != nullptr && common->autonomy_level_set,
            .autonomy_override = common == nullptr ? SC_AUTONOMY_SUPERVISED : common->autonomy_level,
            .model_switch = model_switch_ptr,
            .request_tool_approval = request_channel_tool_approval,
            .request_tool_approval_user_data = &approval_context,
            .disable_memory = message->disable_memory,
        };
        status = sc_agent_process_message(orchestrator->agent, &turn, alloc, &agent_result);
    }
    if (!sc_status_is_ok(status)) {
        (void)send_reply(channel,
                         message,
                         sc_str_from_cstr("Sorry, I couldn't process that request."),
                         false);
        sc_agent_turn_result_clear(&agent_result);
        sc_string_clear(&agent_input_storage);
        sc_free(alloc, tools_allow, tools_allow_count * sizeof(*tools_allow), _Alignof(sc_str));
        sc_free(alloc, tools_deny, tools_deny_count * sizeof(*tools_deny), _Alignof(sc_str));
        return status;
    }
    if (sc_status_is_ok(status)) {
        status = send_reply(channel, message, sc_string_as_str(&agent_result.output), false);
    }
    if (sc_status_is_ok(status)) {
        status = send_attachment_reply(channel, message, &agent_result);
    }
    if (sc_status_is_ok(status)) {
        (void)send_audio_reply(orchestrator, channel, message, sc_string_as_str(&agent_result.output));
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&agent_result.output), &out->reply);
    }
    if (sc_status_is_ok(status)) {
        out->processed = true;
        status = sc_history_append(&history->history, sc_str_from_cstr("user"), agent_input);
    }
    if (sc_status_is_ok(status)) {
        status = persist_session_message(orchestrator, message, sc_str_from_cstr("user"), agent_input);
    }
    if (sc_status_is_ok(status)) {
        status = sc_history_append(&history->history, sc_str_from_cstr("assistant"), sc_string_as_str(&agent_result.output));
    }
    if (sc_status_is_ok(status)) {
        status = persist_session_message(orchestrator, message, sc_str_from_cstr("assistant"), sc_string_as_str(&agent_result.output));
    }
    if (sc_status_is_ok(status)) {
        status = emit_event(orchestrator, sc_str_from_cstr("channel.processed"), message->message_id);
    }
    sc_agent_turn_result_clear(&agent_result);
    sc_string_clear(&agent_input_storage);
    sc_free(alloc, tools_allow, tools_allow_count * sizeof(*tools_allow), _Alignof(sc_str));
    sc_free(alloc, tools_deny, tools_deny_count * sizeof(*tools_deny), _Alignof(sc_str));
    return status;
}

sc_status sc_channel_orchestrator_poll(sc_channel_orchestrator *orchestrator,
                                       sc_channel *channel,
                                       sc_allocator *alloc,
                                       sc_channel_process_result *out)
{
    sc_channel_inbound inbound = {0};
    sc_status status = sc_status_ok();
    if (orchestrator == nullptr || channel == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_orchestrator.poll_invalid_argument");
    }
    status = sc_channel_listen(channel, alloc, &inbound);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_channel_orchestrator_process(orchestrator, channel, &inbound, alloc, out);
    sc_channel_inbound_clear(&inbound);
    return status;
}

sc_status sc_channel_orchestrator_request_approval(sc_channel_orchestrator *orchestrator,
                                                   sc_channel *channel,
                                                   const sc_channel_inbound *message,
                                                   sc_str summary,
                                                   sc_allocator *alloc,
                                                   sc_channel_approval_response *out)
{
    sc_channel_approval_request request = {0};
    (void)orchestrator;
    if (channel == nullptr || message == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_orchestrator.approval_invalid_argument");
    }
    request = (sc_channel_approval_request){
        .struct_size = sizeof(request),
        .conversation_id = message->conversation_id,
        .thread_id = message->thread_id,
        .sender_id = message->sender_id,
        .summary = summary,
    };
    return sc_channel_request_approval(channel, &request, alloc, out);
}

sc_status sc_channel_orchestrator_delivery_new(sc_allocator *alloc,
                                              sc_channel_orchestrator *orchestrator,
                                              sc_delivery_target **out)
{
    channel_delivery_impl *impl = nullptr;
    sc_status status = sc_status_ok();
    if (out == nullptr || orchestrator == nullptr) {
        return sc_status_invalid_argument("sc.channel_delivery.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(channel_delivery_impl));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (channel_delivery_impl){.alloc = alloc, .orchestrator = orchestrator};
    status = sc_delivery_target_new(alloc, &channel_delivery_vtab, impl, out);
    if (!sc_status_is_ok(status)) {
        channel_delivery_destroy(impl);
    }
    return status;
}

size_t sc_channel_orchestrator_history_len(const sc_channel_orchestrator *orchestrator,
                                           sc_str channel_name,
                                           sc_str sender_id,
                                           sc_str thread_id)
{
    if (orchestrator == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < orchestrator->histories.len; i += 1) {
        const history_scope *scope = sc_vec_at_const(&orchestrator->histories, i);
        if (scope != nullptr &&
            sc_str_equal(sc_string_as_str(&scope->channel_name), channel_name) &&
            sc_str_equal(sc_string_as_str(&scope->sender_id), sender_id) &&
            sc_str_equal(sc_string_as_str(&scope->thread_id), thread_id)) {
            return scope->history.messages.len;
        }
    }
    return 0;
}

void sc_channel_process_result_clear(sc_channel_process_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->reply);
    *result = (sc_channel_process_result){0};
}

void sc_channel_orchestrator_destroy(sc_channel_orchestrator *orchestrator)
{
    if (orchestrator == nullptr) {
        return;
    }
    for (size_t i = 0; i < orchestrator->allowed_senders.len; i += 1) {
        sc_string *sender = sc_vec_at(&orchestrator->allowed_senders, i);
        sc_string_clear(sender);
    }
    for (size_t i = 0; i < orchestrator->common_rules.len; i += 1) {
        channel_common_rule *rule = sc_vec_at(&orchestrator->common_rules, i);
        common_rule_clear(rule);
    }
    for (size_t i = 0; i < orchestrator->runtime_model_routes.len; i += 1) {
        sc_runtime_channel_provider_route_entry *route = sc_vec_at(&orchestrator->runtime_model_routes, i);
        runtime_model_route_clear(route);
    }
    for (size_t i = 0; i < orchestrator->seen_message_ids.len; i += 1) {
        sc_string *id = sc_vec_at(&orchestrator->seen_message_ids, i);
        sc_string_clear(id);
    }
    for (size_t i = 0; i < orchestrator->histories.len; i += 1) {
        history_scope *scope = sc_vec_at(&orchestrator->histories, i);
        history_scope_clear(scope);
    }
    sc_vec_clear(&orchestrator->channels);
    sc_vec_clear(&orchestrator->allowed_senders);
    sc_vec_clear(&orchestrator->common_rules);
    sc_vec_clear(&orchestrator->runtime_model_routes);
    sc_vec_clear(&orchestrator->seen_message_ids);
    sc_vec_clear(&orchestrator->histories);
    sc_string_clear(&orchestrator->session_db_path);
    sc_string_clear(&orchestrator->session_jsonl_dir);
    sc_string_clear(&orchestrator->stream_mode);
    sc_string_clear(&orchestrator->tts_voice);
    sc_free(orchestrator->alloc, orchestrator, sizeof(*orchestrator), _Alignof(sc_channel_orchestrator));
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out)
{
    return sc_string_from_str(alloc, empty_if_null(input), out);
}

static sc_str empty_if_null(sc_str value)
{
    return value.ptr == nullptr ? sc_str_from_cstr("") : value;
}

static sc_status string_vec_copy(sc_allocator *alloc, const sc_str *values, size_t count, sc_vec *out)
{
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel.string_vec.invalid_argument");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < count; i += 1) {
        sc_string copy = {0};
        status = copy_string(alloc, values[i], &copy);
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(out, &copy);
        }
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&copy);
        }
    }
    return status;
}

static void string_vec_clear(sc_vec *vec)
{
    if (vec == nullptr) {
        return;
    }
    for (size_t i = 0; i < vec->len; i += 1) {
        sc_string *value = sc_vec_at(vec, i);
        sc_string_clear(value);
    }
    sc_vec_clear(vec);
}

static bool string_vec_contains(const sc_vec *vec, sc_str value)
{
    if (vec == nullptr) {
        return false;
    }
    for (size_t i = 0; i < vec->len; i += 1) {
        const sc_string *candidate = sc_vec_at_const(vec, i);
        if (candidate != nullptr && sc_str_equal(sc_string_as_str(candidate), value)) {
            return true;
        }
    }
    return false;
}

static bool string_vec_contains_wildcard_or_value(const sc_vec *vec, sc_str value)
{
    if (vec == nullptr || vec->len == 0) {
        return true;
    }
    return string_vec_contains(vec, sc_str_from_cstr("*")) || string_vec_contains(vec, value);
}

static bool text_contains(sc_str text, sc_str needle)
{
    if (text.ptr == nullptr || needle.ptr == nullptr || needle.len == 0 || needle.len > text.len) {
        return false;
    }
    for (size_t i = 0; i + needle.len <= text.len; i += 1) {
        if (memcmp(text.ptr + i, needle.ptr, needle.len) == 0) {
            return true;
        }
    }
    return false;
}

static bool common_config_allows_values(bool enabled_set,
                                        bool enabled,
                                        const sc_vec *allowed_users,
                                        const sc_vec *allowed_destinations,
                                        bool reply_to_mentions_only,
                                        sc_str mention_token,
                                        bool pairing_required,
                                        bool paired,
                                        const sc_channel_inbound *event)
{
    if (event == nullptr) {
        return false;
    }
    if (enabled_set && !enabled) {
        return false;
    }
    if (!sc_status_is_ok(sc_security_validate_pairing(pairing_required, paired, event->sender_id))) {
        return false;
    }
    if (!string_vec_contains_wildcard_or_value(allowed_users, event->sender_id)) {
        return false;
    }
    if (!string_vec_contains_wildcard_or_value(allowed_destinations, event->conversation_id)) {
        return false;
    }
    return !reply_to_mentions_only || text_contains(event->text, mention_token);
}

bool sc_channel_common_config_allows_event(const sc_channel_common_config *config,
                                           const sc_channel_inbound *event)
{
    sc_vec users = {0};
    sc_vec destinations = {0};
    sc_allocator *alloc = sc_allocator_heap();
    bool allowed = false;

    if (config == nullptr || event == nullptr) {
        return false;
    }
    sc_vec_init(&users, alloc, sizeof(sc_string));
    sc_vec_init(&destinations, alloc, sizeof(sc_string));
    if (sc_status_is_ok(string_vec_copy(alloc, config->allowed_users, config->allowed_user_count, &users)) &&
        sc_status_is_ok(string_vec_copy(alloc, config->allowed_destinations, config->allowed_destination_count, &destinations)) &&
        sc_status_is_ok(string_vec_copy(alloc, config->allowed_chats, config->allowed_chat_count, &destinations))) {
        allowed = common_config_allows_values(config->enabled_set,
                                              config->enabled,
                                              &users,
                                              &destinations,
                                              config->reply_to_mentions_only,
                                              config->mention_token,
                                              config->pairing_required,
                                              config->paired,
                                              event);
    }
    string_vec_clear(&destinations);
    string_vec_clear(&users);
    return allowed;
}

static sc_status common_rule_copy(sc_allocator *alloc, const sc_channel_common_config *config, channel_common_rule *out)
{
    sc_status status = sc_status_ok();

    if (config == nullptr || out == nullptr || config->channel_name.len == 0) {
        return sc_status_invalid_argument("sc.channel_common.invalid_argument");
    }
    *out = (channel_common_rule){
        .enabled = config->enabled,
        .enabled_set = config->enabled_set,
        .reply_to_mentions_only = config->reply_to_mentions_only,
        .draft_update_interval_ms = config->draft_update_interval_ms,
        .autonomy_level_set = config->autonomy_level_set,
        .autonomy_level = config->autonomy_level,
        .pairing_required = config->pairing_required,
        .paired = config->paired,
    };
    if (config->tools_allow_count > 0 && config->tools_deny_count > 0) {
        return sc_status_invalid_argument("sc.channel_common.tools_allow_deny_conflict");
    }
    sc_vec_init(&out->allowed_users, alloc, sizeof(sc_string));
    sc_vec_init(&out->allowed_destinations, alloc, sizeof(sc_string));
    sc_vec_init(&out->tools_allow, alloc, sizeof(sc_string));
    sc_vec_init(&out->tools_deny, alloc, sizeof(sc_string));
    status = copy_string(alloc, config->channel_name, &out->channel_name);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, config->mention_token, &out->mention_token);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, config->provider, &out->provider);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, config->model, &out->model);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, config->allowed_users, config->allowed_user_count, &out->allowed_users);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, config->allowed_destinations, config->allowed_destination_count, &out->allowed_destinations);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, config->allowed_chats, config->allowed_chat_count, &out->allowed_destinations);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, config->tools_allow, config->tools_allow_count, &out->tools_allow);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, config->tools_deny, config->tools_deny_count, &out->tools_deny);
    }
    if (!sc_status_is_ok(status)) {
        common_rule_clear(out);
    }
    return status;
}

static void common_rule_clear(channel_common_rule *rule)
{
    if (rule == nullptr) {
        return;
    }
    sc_string_clear(&rule->channel_name);
    sc_string_clear(&rule->mention_token);
    sc_string_clear(&rule->provider);
    sc_string_clear(&rule->model);
    string_vec_clear(&rule->allowed_users);
    string_vec_clear(&rule->allowed_destinations);
    string_vec_clear(&rule->tools_allow);
    string_vec_clear(&rule->tools_deny);
    *rule = (channel_common_rule){0};
}

static const channel_common_rule *common_rule_for(const sc_channel_orchestrator *orchestrator, sc_str channel_name)
{
    if (orchestrator == nullptr || channel_name.len == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < orchestrator->common_rules.len; i += 1) {
        const channel_common_rule *rule = sc_vec_at_const(&orchestrator->common_rules, i);
        if (rule != nullptr && sc_str_equal(sc_string_as_str(&rule->channel_name), channel_name)) {
            return rule;
        }
    }
    return nullptr;
}

static sc_status string_vec_as_views(const sc_vec *vec, sc_allocator *alloc, sc_str **out, size_t *count_out)
{
    sc_str *views = nullptr;

    if (out == nullptr || count_out == nullptr) {
        return sc_status_invalid_argument("sc.channel_common.tool_views_invalid_argument");
    }
    *out = nullptr;
    *count_out = 0;
    if (vec == nullptr || vec->len == 0) {
        return sc_status_ok();
    }
    views = sc_alloc(alloc, vec->len * sizeof(*views), _Alignof(sc_str));
    if (views == nullptr) {
        return sc_status_no_memory();
    }
    for (size_t i = 0; i < vec->len; i += 1) {
        const sc_string *value = sc_vec_at_const(vec, i);
        views[i] = sc_string_as_str(value);
    }
    *out = views;
    *count_out = vec->len;
    return sc_status_ok();
}

static sc_status common_rule_tools_as_views(const channel_common_rule *rule,
                                            sc_allocator *alloc,
                                            sc_str **allow_out,
                                            size_t *allow_count_out,
                                            sc_str **deny_out,
                                            size_t *deny_count_out)
{
    sc_status status = sc_status_ok();

    if (allow_out == nullptr || allow_count_out == nullptr || deny_out == nullptr || deny_count_out == nullptr) {
        return sc_status_invalid_argument("sc.channel_common.tools_invalid_argument");
    }
    *allow_out = nullptr;
    *deny_out = nullptr;
    *allow_count_out = 0;
    *deny_count_out = 0;
    if (rule == nullptr) {
        return sc_status_ok();
    }
    status = string_vec_as_views(&rule->tools_allow, alloc, allow_out, allow_count_out);
    if (sc_status_is_ok(status)) {
        status = string_vec_as_views(&rule->tools_deny, alloc, deny_out, deny_count_out);
    }
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, *allow_out, *allow_count_out * sizeof(**allow_out), _Alignof(sc_str));
        *allow_out = nullptr;
        *allow_count_out = 0;
    }
    return status;
}

static void stored_inbound_clear(stored_inbound *message)
{
    if (message == nullptr) {
        return;
    }
    sc_string_clear(&message->message_id);
    sc_string_clear(&message->channel_name);
    sc_string_clear(&message->platform_account_id);
    sc_string_clear(&message->conversation_id);
    sc_string_clear(&message->thread_id);
    sc_string_clear(&message->sender_id);
    sc_string_clear(&message->text);
    sc_string_clear(&message->reply_to_message_id);
    sc_string_clear(&message->attachment_media_type);
    sc_string_clear(&message->attachment_name);
    if (message->attachment_temporary && message->attachment_storage_path.ptr != nullptr) {
        (void)unlink(message->attachment_storage_path.ptr);
    }
    sc_string_clear(&message->attachment_storage_path);
    *message = (stored_inbound){0};
}

static sc_status stored_inbound_from_public(sc_allocator *alloc, const sc_channel_inbound *message, stored_inbound *out)
{
    sc_status status = sc_status_ok();
    *out = (stored_inbound){
        .attachment_size_bytes = message->attachment_size_bytes,
        .timestamp_unix_secs = message->timestamp_unix_secs,
        .timestamp_trusted = message->timestamp_trusted,
        .attachment_temporary = message->attachment_temporary,
        .cancel_previous = message->cancel_previous,
        .disable_memory = message->disable_memory,
    };
    status = copy_string(alloc, message->message_id, &out->message_id);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->channel_name, &out->channel_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->platform_account_id, &out->platform_account_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->conversation_id, &out->conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->thread_id, &out->thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->sender_id, &out->sender_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->text, &out->text);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->reply_to_message_id, &out->reply_to_message_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->attachment_media_type, &out->attachment_media_type);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->attachment_name, &out->attachment_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->attachment_storage_path, &out->attachment_storage_path);
    }
    if (!sc_status_is_ok(status)) {
        stored_inbound_clear(out);
    }
    return status;
}

static sc_status public_inbound_from_stored(sc_allocator *alloc, stored_inbound *message, sc_channel_inbound *out)
{
    sc_status status = sc_status_ok();
    if (message == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_inbound.invalid_argument");
    }
    *out = (sc_channel_inbound){
        .struct_size = sizeof(*out),
        .attachment_size_bytes = message->attachment_size_bytes,
        .timestamp_unix_secs = message->timestamp_unix_secs,
        .timestamp_trusted = message->timestamp_trusted,
        .attachment_temporary = message->attachment_temporary,
        .cancel_previous = message->cancel_previous,
        .disable_memory = message->disable_memory,
    };
    status = copy_string(alloc, sc_string_as_str(&message->message_id), &out->owned_message_id);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->channel_name), &out->owned_channel_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->platform_account_id), &out->owned_platform_account_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->conversation_id), &out->owned_conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->thread_id), &out->owned_thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->sender_id), &out->owned_sender_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->text), &out->owned_text);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->reply_to_message_id), &out->owned_reply_to_message_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->attachment_media_type), &out->owned_attachment_media_type);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->attachment_name), &out->owned_attachment_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&message->attachment_storage_path), &out->owned_attachment_storage_path);
    }
    if (!sc_status_is_ok(status)) {
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
    message->attachment_temporary = false;
    return sc_status_ok();
}

static void stored_outbound_clear(stored_outbound *message)
{
    if (message == nullptr) {
        return;
    }
    sc_string_clear(&message->text);
    sc_string_clear(&message->attachment_content_type);
    sc_string_clear(&message->attachment_filename);
    sc_bytes_clear(&message->attachment_bytes);
    *message = (stored_outbound){0};
}

static sc_status stored_outbound_from_public(sc_allocator *alloc, const sc_channel_message *message, stored_outbound *out)
{
    sc_status status = sc_status_ok();
    if (message == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_outbound.invalid_argument");
    }
    *out = (stored_outbound){0};
    status = copy_string(alloc, message->text, &out->text);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->attachment_content_type, &out->attachment_content_type);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, message->attachment_filename, &out->attachment_filename);
    }
    if (sc_status_is_ok(status) && message->attachment_bytes.len > 0) {
        status = sc_bytes_from_buf(alloc, message->attachment_bytes, &out->attachment_bytes);
    }
    if (sc_status_is_ok(status) && message->attachment_bytes.len > 0) {
        out->attachment_delivery = message->attachment_delivery;
    }
    if (!sc_status_is_ok(status)) {
        stored_outbound_clear(out);
    }
    return status;
}

static sc_status webhook_check_replay(webhook_channel *webhook, sc_str nonce, int64_t timestamp_unix_secs)
{
    sc_wall_time now = {0};

    if (webhook == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.replay_invalid_argument");
    }
    if (timestamp_unix_secs > 0) {
        sc_status status = sc_clock_wall(&now);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        int64_t now_secs = now.unix_ns / 1'000'000'000;
        if (timestamp_unix_secs > now_secs + (int64_t)webhook->replay_window_secs ||
            timestamp_unix_secs < now_secs - (int64_t)webhook->replay_window_secs) {
            return sc_status_security_denied("sc.channel_webhook.replay_window_denied");
        }
    }
    if (nonce.len == 0) {
        return sc_status_ok();
    }
    for (size_t i = 0; i < webhook->seen_nonces.len; i += 1) {
        const sc_string *seen = sc_vec_at_const(&webhook->seen_nonces, i);
        if (seen != nullptr && sc_str_equal(sc_string_as_str(seen), nonce)) {
            return sc_status_security_denied("sc.channel_webhook.replay_denied");
        }
    }
    return sc_status_ok();
}

static sc_status webhook_check_rate_limit(webhook_channel *webhook)
{
    sc_wall_time now = {0};
    sc_status status = sc_status_ok();
    int64_t now_secs = 0;

    if (webhook == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.rate_invalid_argument");
    }
    if (webhook->rate_limit_per_minute == 0) {
        return sc_status_ok();
    }
    status = sc_clock_wall(&now);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    now_secs = now.unix_ns / 1'000'000'000;
    if (webhook->rate_window_unix_secs == 0 || now_secs - webhook->rate_window_unix_secs >= 60) {
        webhook->rate_window_unix_secs = now_secs;
        webhook->rate_window_count = 0;
    }
    if (webhook->rate_window_count >= webhook->rate_limit_per_minute) {
        return sc_status_security_denied("sc.channel_webhook.rate_limited");
    }
    webhook->rate_window_count += 1;
    return sc_status_ok();
}

static sc_status webhook_remember_nonce(webhook_channel *webhook, sc_str nonce)
{
    sc_string copy = {0};
    sc_status status = sc_status_ok();

    if (webhook == nullptr || nonce.len == 0) {
        return sc_status_ok();
    }
    while (webhook->seen_nonces.len >= 1'024) {
        webhook_forget_nonce_at(webhook, 0);
    }
    status = copy_string(webhook->alloc, nonce, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&webhook->seen_nonces, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&copy);
    }
    return status;
}

static void webhook_forget_nonce_at(webhook_channel *webhook, size_t index)
{
    sc_string *nonce = nullptr;

    if (webhook == nullptr || index >= webhook->seen_nonces.len) {
        return;
    }
    nonce = sc_vec_at(&webhook->seen_nonces, index);
    sc_string_clear(nonce);
    if (index + 1 < webhook->seen_nonces.len) {
        (void)memmove((unsigned char *)webhook->seen_nonces.ptr + index * webhook->seen_nonces.item_size,
                      (unsigned char *)webhook->seen_nonces.ptr + (index + 1) * webhook->seen_nonces.item_size,
                      (webhook->seen_nonces.len - index - 1) * webhook->seen_nonces.item_size);
    }
    webhook->seen_nonces.len -= 1;
}

static sc_status webhook_store_media(sc_allocator *alloc,
                                     sc_str media_temp_dir,
                                     sc_str attachment_id,
                                     sc_str content_type,
                                     sc_str filename,
                                     sc_str data_base64,
                                     size_t max_media_bytes,
                                     sc_string *storage_path,
                                     size_t *size_bytes)
{
    sc_bytes bytes = {0};
    sc_media_attachment attachment = {0};
    sc_media_attachment stored = {0};
    sc_media_limits limits = {0};
    sc_str allowed_content_types[1] = {0};
    sc_status status = sc_status_ok();

    if (storage_path == nullptr || size_bytes == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.media_store_invalid_argument");
    }
    *size_bytes = 0;
    if (data_base64.len == 0) {
        return sc_status_ok();
    }
    if (media_temp_dir.ptr == nullptr || media_temp_dir.len == 0) {
        return sc_status_invalid_argument("sc.channel_webhook.media_temp_missing");
    }

    sc_bytes_init(&bytes, alloc);
    status = base64_decode(alloc, data_base64, &bytes);
    if (sc_status_is_ok(status)) {
        sc_str detected_type = detect_media_type(sc_buf_from_parts(bytes.ptr, bytes.len));
        if (detected_type.len > 0 && !media_type_matches(content_type, detected_type)) {
            status = sc_status_security_denied("sc.channel_webhook.media_type_mismatch");
        }
    }
    if (sc_status_is_ok(status)) {
        allowed_content_types[0] = content_type;
        limits = (sc_media_limits){
            .struct_size = sizeof(limits),
            .max_bytes = max_media_bytes,
            .allowed_content_types = allowed_content_types,
            .allowed_content_type_count = SC_ARRAY_LEN(allowed_content_types),
        };
        attachment = (sc_media_attachment){
            .struct_size = sizeof(attachment),
            .attachment_id = attachment_id,
            .content_type = content_type,
            .filename = filename,
            .bytes = sc_buf_from_parts(bytes.ptr, bytes.len),
            .safety_flags = SC_MEDIA_SAFETY_UNTRUSTED,
        };
        status = sc_media_attachment_from_bytes(alloc, &attachment, &limits, &stored);
    }
    if (sc_status_is_ok(status)) {
        status = webhook_screen_media_attachment(alloc, &stored, &limits);
    }
    if (sc_status_is_ok(status)) {
        status = sc_media_attachment_store_temp(&stored, media_temp_dir);
    }
    if (sc_status_is_ok(status)) {
        *size_bytes = stored.size_bytes;
        *storage_path = stored.owned_storage_path;
        stored.owned_storage_path = (sc_string){0};
        stored.storage_path = sc_str_from_cstr("");
        stored.temporary = false;
    }

    sc_media_attachment_clear(&stored);
    sc_bytes_secure_clear(&bytes);
    return status;
}

static sc_status webhook_screen_media_attachment(sc_allocator *alloc,
                                                 const sc_media_attachment *attachment,
                                                 const sc_media_limits *limits)
{
    sc_media_pipeline_result result = {0};
    sc_status status = sc_status_ok();

    if (attachment == nullptr || limits == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.media_screen_invalid_argument");
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

static sc_status base64_decode(sc_allocator *alloc, sc_str input, sc_bytes *out)
{
    uint32_t buffer = 0;
    uint32_t bits = 0;
    bool padding = false;
    sc_status status = sc_status_ok();

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.channel_webhook.base64_invalid_argument");
    }
    sc_bytes_init(out, alloc);
    for (size_t i = 0; i < input.len; i += 1) {
        char ch = input.ptr[i];
        int value = 0;
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            continue;
        }
        if (ch == '=') {
            padding = true;
            continue;
        }
        value = base64_value(ch);
        if (value < 0 || padding) {
            sc_bytes_secure_clear(out);
            return sc_status_parse("sc.channel_webhook.base64_invalid");
        }
        buffer = (buffer << 6u) | (uint32_t)value;
        bits += 6u;
        if (bits >= 8u) {
            uint8_t byte = 0;
            bits -= 8u;
            byte = (uint8_t)((buffer >> bits) & 0xFFu);
            status = sc_bytes_append(out, sc_buf_from_parts(&byte, 1));
            if (!sc_status_is_ok(status)) {
                sc_bytes_secure_clear(out);
                return status;
            }
            buffer &= (1u << bits) - 1u;
        }
    }
    if (out->len == 0) {
        sc_bytes_secure_clear(out);
        return sc_status_invalid_argument("sc.channel_webhook.base64_empty");
    }
    return sc_status_ok();
}

static int base64_value(char ch)
{
    if (ch >= 'A' && ch <= 'Z') {
        return ch - 'A';
    }
    if (ch >= 'a' && ch <= 'z') {
        return ch - 'a' + 26;
    }
    if (ch >= '0' && ch <= '9') {
        return ch - '0' + 52;
    }
    if (ch == '+') {
        return 62;
    }
    if (ch == '/') {
        return 63;
    }
    return -1;
}

static sc_str detect_media_type(sc_buf bytes)
{
    static const uint8_t png_sig[] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};
    if (bytes.ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    if (bytes.len >= sizeof(png_sig) && memcmp(bytes.ptr, png_sig, sizeof(png_sig)) == 0) {
        return sc_str_from_cstr("image/png");
    }
    if (bytes.len >= 3 && bytes.ptr[0] == 0xFFu && bytes.ptr[1] == 0xD8u && bytes.ptr[2] == 0xFFu) {
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

static bool media_type_matches(sc_str declared_type, sc_str detected_type)
{
    if (detected_type.len == 0) {
        return true;
    }
    return sc_str_equal(declared_type, detected_type);
}

static void history_scope_clear(history_scope *scope)
{
    if (scope == nullptr) {
        return;
    }
    sc_string_clear(&scope->channel_name);
    sc_string_clear(&scope->sender_id);
    sc_string_clear(&scope->thread_id);
    sc_history_clear(&scope->history);
    *scope = (history_scope){0};
}

static void runtime_model_route_clear(sc_runtime_channel_provider_route_entry *route)
{
    if (route == nullptr) {
        return;
    }
    sc_string_clear(&route->channel_name);
    sc_string_clear(&route->provider_name);
    sc_string_clear(&route->model);
    *route = (sc_runtime_channel_provider_route_entry){0};
}

static bool message_seen(const sc_channel_orchestrator *orchestrator, sc_str message_id)
{
    if (orchestrator == nullptr || message_id.len == 0) {
        return false;
    }
    for (size_t i = 0; i < orchestrator->seen_message_ids.len; i += 1) {
        const sc_string *id = sc_vec_at_const(&orchestrator->seen_message_ids, i);
        if (id != nullptr && sc_str_equal(sc_string_as_str(id), message_id)) {
            return true;
        }
    }
    return false;
}

static sc_status remember_message(sc_channel_orchestrator *orchestrator, sc_str message_id)
{
    sc_string copy = {0};
    sc_status status = sc_status_ok();
    if (message_id.len == 0) {
        return sc_status_ok();
    }
    while (orchestrator->seen_message_ids.len >= orchestrator->max_seen_message_ids) {
        forget_seen_message_at(orchestrator, 0);
    }
    status = copy_string(orchestrator->alloc, message_id, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&orchestrator->seen_message_ids, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&copy);
    }
    return status;
}

static void forget_seen_message_at(sc_channel_orchestrator *orchestrator, size_t index)
{
    sc_string *id = nullptr;

    if (orchestrator == nullptr || index >= orchestrator->seen_message_ids.len) {
        return;
    }

    id = sc_vec_at(&orchestrator->seen_message_ids, index);
    sc_string_clear(id);
    if (index + 1 < orchestrator->seen_message_ids.len) {
        (void)memmove((unsigned char *)orchestrator->seen_message_ids.ptr +
                          index * orchestrator->seen_message_ids.item_size,
                      (unsigned char *)orchestrator->seen_message_ids.ptr +
                          (index + 1) * orchestrator->seen_message_ids.item_size,
                      (orchestrator->seen_message_ids.len - index - 1) *
                          orchestrator->seen_message_ids.item_size);
    }
    orchestrator->seen_message_ids.len -= 1;
}

static bool sender_allowed(const sc_channel_orchestrator *orchestrator, sc_str sender_id)
{
    if (orchestrator == nullptr || orchestrator->allowed_senders.len == 0) {
        return true;
    }
    for (size_t i = 0; i < orchestrator->allowed_senders.len; i += 1) {
        const sc_string *allowed = sc_vec_at_const(&orchestrator->allowed_senders, i);
        if (allowed != nullptr && sc_str_equal(sc_string_as_str(allowed), sender_id)) {
            return true;
        }
    }
    return false;
}

static sc_status history_for(sc_channel_orchestrator *orchestrator, const sc_channel_inbound *message, history_scope **out)
{
    sc_str channel_name = message->channel_name.len == 0 ? sc_str_from_cstr("unknown") : message->channel_name;
    sc_str thread = message->thread_id.len == 0 ? message->conversation_id : message->thread_id;
    return history_for_values(orchestrator, channel_name, message->sender_id, thread, out);
}

static sc_status history_for_values(sc_channel_orchestrator *orchestrator,
                                    sc_str channel_name,
                                    sc_str sender_id,
                                    sc_str thread,
                                    history_scope **out)
{
    history_scope scope = {0};
    sc_status status = sc_status_ok();

    for (size_t i = 0; i < orchestrator->histories.len; i += 1) {
        history_scope *existing = sc_vec_at(&orchestrator->histories, i);
        if (existing != nullptr &&
            sc_str_equal(sc_string_as_str(&existing->channel_name), channel_name) &&
            sc_str_equal(sc_string_as_str(&existing->sender_id), sender_id) &&
            sc_str_equal(sc_string_as_str(&existing->thread_id), thread)) {
            *out = existing;
            return sc_status_ok();
        }
    }

    status = copy_string(orchestrator->alloc, channel_name, &scope.channel_name);
    if (sc_status_is_ok(status)) {
        status = copy_string(orchestrator->alloc, sender_id, &scope.sender_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(orchestrator->alloc, thread, &scope.thread_id);
    }
    if (sc_status_is_ok(status)) {
        sc_history_init(&scope.history, orchestrator->alloc, orchestrator->max_history_messages);
        status = sc_vec_push(&orchestrator->histories, &scope);
    }
    if (!sc_status_is_ok(status)) {
        history_scope_clear(&scope);
        return status;
    }
    *out = sc_vec_at(&orchestrator->histories, orchestrator->histories.len - 1);
    return sc_status_ok();
}

static sc_status emit_event(sc_channel_orchestrator *orchestrator, sc_str name, sc_str status_text)
{
    sc_log_field fields[] = {
        {.key = "status", .value = status_text, .secret = false},
    };
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("sc.channel"),
        .name = name,
        .fields = fields,
        .field_count = 1,
    };
    if (orchestrator == nullptr || orchestrator->observer == nullptr) {
        return sc_status_ok();
    }
    return sc_observer_emit_safe(orchestrator->observer, &event, nullptr);
}

static sc_status send_reply(sc_channel *channel, const sc_channel_inbound *inbound, sc_str text, bool draft)
{
    sc_status status = sc_status_ok();
    sc_channel_message message = {
        .struct_size = sizeof(message),
        .conversation_id = inbound->conversation_id,
        .thread_id = inbound->thread_id,
        .sender_id = inbound->sender_id,
        .text = text,
        .draft = draft,
        .reply_to_message_id = inbound->reply_to_message_id,
    };
    status = sc_security_validate_outbound_message(text);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_channel_send(channel, &message);
}

static sc_status send_attachment_reply(sc_channel *channel,
                                       const sc_channel_inbound *inbound,
                                       const sc_agent_turn_result *result)
{
    sc_channel_message message = {0};
    sc_status status;

    if (channel == nullptr || inbound == nullptr || result == nullptr) {
        return sc_status_invalid_argument("sc.channel.attachment.invalid_argument");
    }
    if (result->attachment_bytes.len == 0) {
        return sc_status_ok();
    }
    if ((channel_capabilities(channel) & SC_CHANNEL_CAP_ATTACHMENTS) == 0) {
        return sc_status_ok();
    }

    message = (sc_channel_message){
        .struct_size = sizeof(message),
        .conversation_id = inbound->conversation_id,
        .thread_id = inbound->thread_id,
        .sender_id = inbound->sender_id,
        .text = sc_str_from_cstr(""),
        .draft = false,
        .reply_to_message_id = inbound->reply_to_message_id,
        .attachment_content_type = sc_string_as_str(&result->attachment_content_type),
        .attachment_filename = sc_string_as_str(&result->attachment_filename),
        .attachment_bytes = sc_buf_from_parts(result->attachment_bytes.ptr, result->attachment_bytes.len),
        .attachment_delivery = result->attachment_delivery,
    };
    status = sc_channel_send(channel, &message);
    if (!sc_status_is_ok(status) && status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    return status;
}

static sc_status request_channel_tool_approval(void *user_data,
                                               sc_str tool_name,
                                               sc_str arguments_json,
                                               sc_allocator *alloc,
                                               bool *out_approved)
{
    channel_tool_approval_context *context = user_data;
    sc_channel_approval_response response = {0};
    sc_string summary = {0};
    sc_status status = sc_status_ok();

    if (context == nullptr || context->channel == nullptr || context->message == nullptr || out_approved == nullptr) {
        return sc_status_invalid_argument("sc.channel.tool_approval_invalid_argument");
    }
    *out_approved = false;
    status = build_tool_approval_summary(alloc, tool_name, arguments_json, &summary);
    if (sc_status_is_ok(status)) {
        status = sc_channel_orchestrator_request_approval(context->orchestrator,
                                                          context->channel,
                                                          context->message,
                                                          sc_string_as_str(&summary),
                                                          alloc,
                                                          &response);
    }
    if (sc_status_is_ok(status)) {
        *out_approved = response.decision == SC_CHANNEL_APPROVAL_APPROVED ||
                        response.decision == SC_CHANNEL_APPROVAL_ALWAYS;
    }
    sc_channel_approval_response_clear(&response);
    sc_string_clear(&summary);
    return status;
}

static sc_status build_tool_approval_summary(sc_allocator *alloc, sc_str tool_name, sc_str arguments_json, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_str args = arguments_json;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel.tool_approval_summary_invalid_argument");
    }
    if (args.len > 512) {
        args.len = 512;
    }
    sc_string_builder_init(&builder, alloc == nullptr ? sc_allocator_heap() : alloc);
    status = sc_string_builder_append_cstr(&builder, "Approve tool call?\n\nTool: ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, tool_name);
    }
    if (sc_status_is_ok(status) && args.len > 0) {
        status = sc_string_builder_append_cstr(&builder, "\nArgs: ");
    }
    if (sc_status_is_ok(status) && args.len > 0) {
        status = sc_string_builder_append(&builder, args);
    }
    if (sc_status_is_ok(status) && args.len < arguments_json.len) {
        status = sc_string_builder_append_cstr(&builder, "...");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status send_audio_reply(sc_channel_orchestrator *orchestrator,
                                  sc_channel *channel,
                                  const sc_channel_inbound *inbound,
                                  sc_str text)
{
    sc_tts_result speech = {0};
    sc_channel_message message = {0};
    sc_status status = sc_status_ok();

    if (orchestrator == nullptr || channel == nullptr || inbound == nullptr) {
        return sc_status_invalid_argument("sc.channel.tts.invalid_argument");
    }
    if (orchestrator->tts == nullptr ||
        orchestrator->tts_reply_mode != SC_CHANNEL_TTS_REPLY_TEXT_AND_AUDIO ||
        text.len == 0) {
        return sc_status_ok();
    }
    if ((channel_capabilities(channel) & SC_CHANNEL_CAP_ATTACHMENTS) == 0) {
        return sc_status_ok();
    }

    sc_tts_request tts_request = {
        .struct_size = sizeof(tts_request),
        .text = text,
        .voice = sc_string_as_str(&orchestrator->tts_voice),
    };
    status = sc_tts_synthesize(orchestrator->tts, &tts_request, orchestrator->alloc, &speech);
    if (!sc_status_is_ok(status)) {
        (void)emit_event(orchestrator, sc_str_from_cstr("channel.tts_failed"), sc_str_from_cstr(status.error_key == nullptr ? "sc.channel.tts_failed" : status.error_key));
        sc_status_clear(&status);
        return sc_status_ok();
    }

    message = (sc_channel_message){
        .struct_size = sizeof(message),
        .conversation_id = inbound->conversation_id,
        .thread_id = inbound->thread_id,
        .sender_id = inbound->sender_id,
        .text = sc_str_from_cstr(""),
        .draft = false,
        .reply_to_message_id = inbound->reply_to_message_id,
        .attachment_content_type = sc_string_as_str(&speech.content_type),
        .attachment_filename = sc_str_from_cstr("smolclaw-reply.wav"),
        .attachment_bytes = sc_buf_from_parts(speech.audio.ptr, speech.audio.len),
    };
    status = sc_channel_send(channel, &message);
    if (!sc_status_is_ok(status) && status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&status);
        status = sc_status_ok();
    }
    sc_tts_result_clear(&speech);
    return status;
}

static sc_status build_agent_input_for_message(const sc_channel_orchestrator *orchestrator,
                                               const sc_channel_inbound *message,
                                               sc_allocator *alloc,
                                               sc_string *owned_input,
                                               sc_str *out)
{
    sc_media_attachment attachment = {0};
    sc_media_pipeline_result result = {0};
    sc_string_builder builder = {0};
    sc_string storage_path = {0};
    struct stat st = {0};
    sc_status status = sc_status_ok();
    size_t attachment_size = 0;

    if (orchestrator == nullptr || message == nullptr || owned_input == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel.asr.invalid_argument");
    }
    attachment_size = message->attachment_size_bytes;
    *out = message->text;
    if (orchestrator->transcriber == nullptr ||
        message->attachment_media_type.len == 0 ||
        !sc_media_content_type_is_audio(message->attachment_media_type) ||
        message->attachment_storage_path.len == 0) {
        return sc_status_ok();
    }

    if (attachment_size == 0) {
        status = sc_string_from_str(alloc, message->attachment_storage_path, &storage_path);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        if (stat(storage_path.ptr, &st) != 0 || st.st_size <= 0) {
            sc_string_clear(&storage_path);
            return sc_status_io("sc.channel.asr.audio_stat_failed");
        }
        attachment_size = (size_t)st.st_size;
    }

    attachment = (sc_media_attachment){
        .struct_size = sizeof(attachment),
        .content_type = message->attachment_media_type,
        .filename = message->attachment_name,
        .size_bytes = attachment_size,
        .storage_kind = SC_MEDIA_STORAGE_PATH,
        .storage_path = message->attachment_storage_path,
        .safety_flags = SC_MEDIA_SAFETY_UNTRUSTED,
    };
    sc_media_pipeline_options pipeline_options = {
        .struct_size = sizeof(pipeline_options),
        .limits =
            {
                .struct_size = sizeof(sc_media_limits),
                .max_bytes = attachment_size,
                .timeout_ms = 30'000,
            },
        .transcriber = orchestrator->transcriber,
        .timeout_ms = 30'000,
    };
    status = sc_media_pipeline_run(alloc, &pipeline_options, &attachment, sc_str_from_cstr(""), &result);
    if (sc_status_is_ok(status) && message->text.len == 0) {
        status = copy_string(alloc, sc_string_as_str(&result.transcript), owned_input);
    } else if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "User text: ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, message->text);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\nMedia transcript: ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&result.transcript));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, owned_input);
        }
        if (!sc_status_is_ok(status)) {
            sc_string_builder_clear(&builder);
        }
    }
    if (sc_status_is_ok(status)) {
        *out = sc_string_as_str(owned_input);
    }
    sc_media_pipeline_result_clear(&result);
    sc_string_clear(&storage_path);
    return status;
}

static sc_status persist_session_message(sc_channel_orchestrator *orchestrator,
                                         const sc_channel_inbound *inbound,
                                         sc_str role,
                                         sc_str content)
{
    sc_string session_key = {0};
    sc_status status = sc_status_ok();

    if (orchestrator == nullptr || inbound == nullptr || !orchestrator->session_persistence) {
        return sc_status_ok();
    }
    status = build_session_key(orchestrator->alloc, inbound, &session_key);
    if (sc_status_is_ok(status)) {
        status = persist_session_jsonl(orchestrator, sc_string_as_str(&session_key), role, content);
    }
    if (sc_status_is_ok(status)) {
        status = persist_session_sqlite(orchestrator, sc_string_as_str(&session_key), role, content);
    }
    sc_string_clear(&session_key);
    return status;
}

static sc_status persist_session_jsonl(sc_channel_orchestrator *orchestrator,
                                       sc_str session_key,
                                       sc_str role,
                                       sc_str content)
{
    sc_string path = {0};
    sc_string created = {0};
    sc_string line = {0};
    sc_string_builder builder = {0};
    FILE *file = nullptr;
    sc_status status = sc_status_ok();

    if (orchestrator->session_jsonl_dir.len == 0) {
        return sc_status_ok();
    }
    status = build_jsonl_path(orchestrator, session_key, &path);
    if (sc_status_is_ok(status)) {
        status = current_time_text(orchestrator->alloc, &created);
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, orchestrator->alloc);
        status = sc_string_builder_append_cstr(&builder, "{\"session_key\":");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, session_key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"role\":");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, role);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"content\":");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"created_at\":");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, sc_string_as_str(&created));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "}\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &line);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        file = fopen(path.ptr, "ab");
        if (file == nullptr) {
            status = sc_status_io("sc.channel_session.jsonl_open_failed");
        }
    }
    if (sc_status_is_ok(status) && fwrite(line.ptr, 1, line.len, file) != line.len) {
        status = sc_status_io("sc.channel_session.jsonl_write_failed");
    }
    if (file != nullptr && fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.channel_session.jsonl_close_failed");
    }
    sc_string_clear(&line);
    sc_string_clear(&created);
    sc_string_clear(&path);
    return status;
}

typedef struct sqlite_session_api {
    void *library;
    int (*open)(const char *, void **);
    int (*close)(void *);
    int (*exec)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    int (*prepare_v2)(void *, const char *, int, void **, const char **);
    int (*bind_text)(void *, int, const char *, int, void (*)(void *));
    int (*step)(void *);
    const unsigned char *(*column_text)(void *, int);
    int (*finalize)(void *);
    const char *(*errmsg)(void *);
} sqlite_session_api;

static sc_status load_sqlite_session_api(sqlite_session_api *api)
{
    if (api == nullptr) {
        return sc_status_invalid_argument("sc.channel_session.sqlite_invalid_argument");
    }
#ifdef SC_HAVE_SQLITE
    *api = (sqlite_session_api){
        .open = (int (*)(const char *, void **))sqlite3_open,
        .close = (int (*)(void *))sqlite3_close,
        .exec = (int (*)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **))sqlite3_exec,
        .prepare_v2 = (int (*)(void *, const char *, int, void **, const char **))sqlite3_prepare_v2,
        .bind_text = (int (*)(void *, int, const char *, int, void (*)(void *)))sqlite3_bind_text,
        .step = (int (*)(void *))sqlite3_step,
        .column_text = (const unsigned char *(*)(void *, int))sqlite3_column_text,
        .finalize = (int (*)(void *))sqlite3_finalize,
        .errmsg = (const char *(*)(void *))sqlite3_errmsg,
    };
    return sc_status_ok();
#else
    *api = (sqlite_session_api){0};
    api->library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (api->library == nullptr) {
        api->library = dlopen("libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (api->library == nullptr) {
        return sc_status_ok();
    }
#define LOAD_SQLITE(name)                                                                                 \
    do {                                                                                                  \
        *(void **)(&api->name) = dlsym(api->library, "sqlite3_" #name);                                  \
        if (api->name == nullptr) {                                                                          \
            dlclose(api->library);                                                                        \
            *api = (sqlite_session_api){0};                                                               \
            return sc_status_ok();                                                                        \
        }                                                                                                 \
    } while (0)
    LOAD_SQLITE(open);
    LOAD_SQLITE(close);
    LOAD_SQLITE(exec);
    LOAD_SQLITE(prepare_v2);
    LOAD_SQLITE(bind_text);
    LOAD_SQLITE(step);
    LOAD_SQLITE(column_text);
    LOAD_SQLITE(finalize);
    LOAD_SQLITE(errmsg);
#undef LOAD_SQLITE
    return sc_status_ok();
#endif
}

static sc_status sqlite_session_status(const sqlite_session_api *api, void *db, const char *error_key)
{
    const char *message = nullptr;

    if (api != nullptr && api->errmsg != nullptr && db != nullptr) {
        message = api->errmsg(db);
    }
    return sc_status_make_owned(sc_allocator_heap(), SC_ERR_IO, error_key, message);
}

static bool sqlite_session_error_mentions(const sqlite_session_api *api, void *db, const char *needle)
{
    const char *message = nullptr;

    if (api == nullptr || api->errmsg == nullptr || db == nullptr || needle == nullptr) {
        return false;
    }
    message = api->errmsg(db);
    return message != nullptr && strstr(message, needle) != nullptr;
}

static void migrate_session_metadata_sqlite(const sqlite_session_api *api, void *db)
{
    const char *migrations[] = {
        "ALTER TABLE session_metadata ADD COLUMN created_at TEXT NOT null DEFAULT ''",
        "ALTER TABLE session_metadata ADD COLUMN last_activity TEXT NOT null DEFAULT ''",
        "ALTER TABLE session_metadata ADD COLUMN message_count INTEGER NOT null DEFAULT 0",
        "ALTER TABLE session_metadata ADD COLUMN name TEXT",
        "ALTER TABLE session_metadata ADD COLUMN state TEXT NOT null DEFAULT 'idle'",
        "ALTER TABLE session_metadata ADD COLUMN turn_id TEXT",
        "ALTER TABLE session_metadata ADD COLUMN turn_started_at TEXT",
    };

    for (size_t i = 0; i < SC_ARRAY_LEN(migrations); i += 1) {
        (void)api->exec(db, migrations[i], nullptr, nullptr, nullptr);
    }
}

static void disable_session_fts_sqlite(const sqlite_session_api *api, void *db)
{
    /*
     * Some deployment SQLite builds omit FTS5. If a workspace was created with
     * FTS5 support, normal prepares can fail while parsing those virtual-table
     * schema rows. Session persistence only needs the base tables, so remove
     * the optional FTS hooks and keep this connection in writable-schema mode
     * until close so SQLite ignores any stale virtual-table metadata.
     */
    const char *cleanup =
        "PRAGMA writable_schema=ON;"
        "DELETE FROM sqlite_schema WHERE name IN ("
        "'sessions_fts','sessions_fts_data','sessions_fts_idx','sessions_fts_docsize','sessions_fts_config',"
        "'sessions_ai','sessions_ad');";

    (void)api->exec(db, cleanup, nullptr, nullptr, nullptr);
}

static sc_status prepare_session_sqlite(const sqlite_session_api *api, void *db, const char *sql, void **stmt)
{
    if (api->prepare_v2(db, sql, -1, stmt, nullptr) == 0) {
        return sc_status_ok();
    }
    if (sqlite_session_error_mentions(api, db, "no such module: fts5")) {
        disable_session_fts_sqlite(api, db);
        if (api->prepare_v2(db, sql, -1, stmt, nullptr) == 0) {
            return sc_status_ok();
        }
    }
    return sqlite_session_status(api, db, "sc.channel_session.sqlite_prepare_failed");
}

static sc_status persist_session_sqlite(sc_channel_orchestrator *orchestrator,
                                        sc_str session_key,
                                        sc_str role,
                                        sc_str content)
{
    void (*sqlite_static)(void *) = nullptr;
    sqlite_session_api api = {0};
    void *db = nullptr;
    void *stmt = nullptr;
    sc_string created = {0};
    sc_status status = sc_status_ok();
    const char *schema =
        "CREATE TABLE IF NOT EXISTS session_metadata ("
        "session_key TEXT PRIMARY KEY, created_at TEXT NOT null, last_activity TEXT NOT null, "
        "message_count INTEGER NOT null DEFAULT 0, name TEXT, state TEXT NOT null DEFAULT 'idle', "
        "turn_id TEXT, turn_started_at TEXT);"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, session_key TEXT NOT null, role TEXT NOT null, "
        "content TEXT NOT null, created_at TEXT NOT null);"
        "CREATE INDEX IF NOT EXISTS idx_sessions_key ON sessions(session_key);";
    // cppcheck-suppress variableScope
    const char *fts_schema =
        "CREATE VIRTUAL TABLE IF NOT EXISTS sessions_fts USING fts5("
        "session_key, content, content='sessions', content_rowid='id');"
        "CREATE TRIGGER IF NOT EXISTS sessions_ai AFTER INSERT ON sessions BEGIN "
        "INSERT INTO sessions_fts(rowid, session_key, content) VALUES (new.id, new.session_key, new.content);"
        "END;"
        "CREATE TRIGGER IF NOT EXISTS sessions_ad AFTER DELETE ON sessions BEGIN "
        "INSERT INTO sessions_fts(sessions_fts, rowid, session_key, content) "
        "VALUES('delete', old.id, old.session_key, old.content);"
        "END;";
    // cppcheck-suppress variableScope
    const char *upsert =
        "INSERT INTO session_metadata(session_key, created_at, last_activity, message_count, state) "
        "VALUES(?1, ?2, ?2, 1, 'idle') "
        "ON CONFLICT(session_key) DO UPDATE SET last_activity=excluded.last_activity, "
        "message_count=message_count + 1";
    // cppcheck-suppress variableScope
    const char *insert = "INSERT INTO sessions(session_key, role, content, created_at) VALUES(?1, ?2, ?3, ?4)";

    if (orchestrator->session_db_path.len == 0) {
        return sc_status_ok();
    }
    status = load_sqlite_session_api(&api);
    if (!sc_status_is_ok(status) || api.open == nullptr) {
        return status;
    }
    if (api.open(orchestrator->session_db_path.ptr, &db) != 0) {
        status = sqlite_session_status(&api, db, "sc.channel_session.sqlite_open_failed");
        if (db != nullptr) {
            (void)api.close(db);
        }
        if (api.library != nullptr) {
            dlclose(api.library);
        }
        return status;
    }
    if (api.exec(db, schema, nullptr, nullptr, nullptr) != 0) {
        status = sqlite_session_status(&api, db, "sc.channel_session.sqlite_schema_failed");
    }
    if (sc_status_is_ok(status)) {
        migrate_session_metadata_sqlite(&api, db);
    }
    if (sc_status_is_ok(status)) {
        /* FTS accelerates future search features but is not required for replay. */
        if (api.exec(db, fts_schema, nullptr, nullptr, nullptr) != 0 &&
            sqlite_session_error_mentions(&api, db, "no such module: fts5")) {
            disable_session_fts_sqlite(&api, db);
        }
    }
    if (sc_status_is_ok(status)) {
        status = current_time_text(orchestrator->alloc, &created);
    }
    if (sc_status_is_ok(status)) {
        status = prepare_session_sqlite(&api, db, upsert, &stmt);
    }
    if (sc_status_is_ok(status)) {
        (void)api.bind_text(stmt, 1, session_key.ptr, (int)session_key.len, sqlite_static);
        (void)api.bind_text(stmt, 2, created.ptr, (int)created.len, sqlite_static);
        if (api.step(stmt) != 101) {
            status = sqlite_session_status(&api, db, "sc.channel_session.sqlite_step_failed");
        }
    }
    if (stmt != nullptr) {
        (void)api.finalize(stmt);
        stmt = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = prepare_session_sqlite(&api, db, insert, &stmt);
    }
    if (sc_status_is_ok(status)) {
        (void)api.bind_text(stmt, 1, session_key.ptr, (int)session_key.len, sqlite_static);
        (void)api.bind_text(stmt, 2, role.ptr, (int)role.len, sqlite_static);
        (void)api.bind_text(stmt, 3, content.ptr, (int)content.len, sqlite_static);
        (void)api.bind_text(stmt, 4, created.ptr, (int)created.len, sqlite_static);
        if (api.step(stmt) != 101) {
            status = sqlite_session_status(&api, db, "sc.channel_session.sqlite_step_failed");
        }
    }
    if (stmt != nullptr) {
        (void)api.finalize(stmt);
    }
    sc_string_clear(&created);
    (void)api.close(db);
    if (api.library != nullptr) {
        dlclose(api.library);
    }
    return status;
}

static sc_status load_session_history(sc_channel_orchestrator *orchestrator)
{
    if (orchestrator == nullptr || !orchestrator->session_persistence || orchestrator->session_db_path.len == 0) {
        return sc_status_ok();
    }
    return load_session_history_sqlite(orchestrator);
}

static sc_status load_session_history_sqlite(sc_channel_orchestrator *orchestrator)
{
    sqlite_session_api api = {0};
    void *db = nullptr;
    void *stmt = nullptr;
    sc_status status = sc_status_ok();
    const char *select_sql =
        "SELECT session_key, role, content FROM sessions ORDER BY session_key ASC, id ASC";

    status = load_sqlite_session_api(&api);
    if (!sc_status_is_ok(status) || api.open == nullptr) {
        return status;
    }
    if (api.open(orchestrator->session_db_path.ptr, &db) != 0) {
        if (api.library != nullptr) {
            dlclose(api.library);
        }
        return sc_status_ok();
    }
    if (api.prepare_v2(db, select_sql, -1, &stmt, nullptr) != 0) {
        (void)api.close(db);
        if (api.library != nullptr) {
            dlclose(api.library);
        }
        return sc_status_ok();
    }
    status = sc_status_ok();
    for (;;) {
        int step = api.step(stmt);
        if (step == 100) {
            const unsigned char *session_key_text = api.column_text(stmt, 0);
            const unsigned char *role_text = api.column_text(stmt, 1);
            const unsigned char *content_text = api.column_text(stmt, 2);
            history_scope *scope = nullptr;
            sc_status row_status;
            if (session_key_text == nullptr || role_text == nullptr || content_text == nullptr) {
                continue;
            }
            row_status = history_from_session_key(orchestrator, sc_str_from_cstr((const char *)session_key_text), &scope);
            if (row_status.code == SC_ERR_INVALID_ARGUMENT) {
                sc_status_clear(&row_status);
                continue;
            }
            if (!sc_status_is_ok(row_status)) {
                status = row_status;
                break;
            }
            if (scope != nullptr) {
                status = sc_history_append(&scope->history,
                                           sc_str_from_cstr((const char *)role_text),
                                           sc_str_from_cstr((const char *)content_text));
                if (!sc_status_is_ok(status)) {
                    break;
                }
            }
        } else if (step == 101) {
            break;
        } else {
            status = sc_status_io("sc.channel_session.sqlite_load_step_failed");
            break;
        }
    }
    if (stmt != nullptr) {
        (void)api.finalize(stmt);
    }
    (void)api.close(db);
    if (api.library != nullptr) {
        dlclose(api.library);
    }
    return status;
}

static sc_status history_from_session_key(sc_channel_orchestrator *orchestrator,
                                          sc_str session_key,
                                          history_scope **out)
{
    size_t first = SIZE_MAX;
    size_t last = SIZE_MAX;
    sc_str channel = {0};
    sc_str thread = {0};
    sc_str sender = {0};

    if (out == nullptr || session_key.ptr == nullptr || session_key.len == 0) {
        return sc_status_invalid_argument("sc.channel_session.session_key_invalid");
    }
    for (size_t i = 0; i < session_key.len; i += 1) {
        if (session_key.ptr[i] == '_') {
            if (first == SIZE_MAX) {
                first = i;
            }
            last = i;
        }
    }
    if (first == SIZE_MAX || last == first || first == 0 || last + 1 >= session_key.len) {
        return sc_status_invalid_argument("sc.channel_session.session_key_invalid");
    }
    channel = sc_str_from_parts(session_key.ptr, first);
    thread = sc_str_from_parts(session_key.ptr + first + 1, last - first - 1);
    sender = sc_str_from_parts(session_key.ptr + last + 1, session_key.len - last - 1);
    return history_for_values(orchestrator, channel, sender, thread, out);
}

static sc_status append_json_string(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char ch = value.ptr[i];
        if (ch == '"' || ch == '\\') {
            char escaped[2] = {'\\', ch};
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(&ch, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

static sc_status build_session_key(sc_allocator *alloc, const sc_channel_inbound *inbound, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, inbound->channel_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "_");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, inbound->conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "_");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, inbound->sender_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_jsonl_path(sc_channel_orchestrator *orchestrator, sc_str session_key, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_str dir = sc_string_as_str(&orchestrator->session_jsonl_dir);
    sc_string_builder_init(&builder, orchestrator->alloc);
    status = sc_string_builder_append(&builder, dir);
    if (sc_status_is_ok(status) && (dir.len == 0 || dir.ptr[dir.len - 1] != '/')) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        for (size_t i = 0; sc_status_is_ok(status) && i < session_key.len; i += 1) {
            char ch = session_key.ptr[i];
            if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') {
                ch = '_';
            }
            status = sc_string_builder_append(&builder, sc_str_from_parts(&ch, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".jsonl");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status current_time_text(sc_allocator *alloc, sc_string *out)
{
    sc_wall_time now = {0};
    sc_status status = sc_clock_wall(&now);
    if (sc_status_is_ok(status)) {
        status = sc_time_format_rfc3339(alloc, now, out);
    }
    return status;
}

static uint64_t channel_capabilities(const sc_channel *channel)
{
    const fake_channel *fake = fake_from_const_handle(channel);
    const sc_channel_vtab *vtab = sc_channel_vtab_of(channel);
    if (fake != nullptr) {
        return fake->capabilities;
    }
    return vtab == nullptr ? 0 : vtab->capabilities;
}

static sc_channel *first_channel(sc_channel_orchestrator *orchestrator)
{
    sc_channel **slot = orchestrator == nullptr || orchestrator->channels.len == 0 ? nullptr : sc_vec_at(&orchestrator->channels, 0);
    return slot == nullptr ? nullptr : *slot;
}

static bool channel_is_fake(const sc_channel *channel)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    return vtab != nullptr && sc_str_equal(sc_str_from_cstr(vtab->name), sc_str_from_cstr("fake"));
}

static bool channel_is_transport(const sc_channel *channel)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    return vtab == &websocket_client_vtab || vtab == &mqtt_vtab || vtab == &rabbitmq_vtab;
}

static bool channel_is_webhook(const sc_channel *channel)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    return vtab == &webhook_vtab;
}

static bool channel_is_rabbitmq_vendor(const sc_channel *channel)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    return vtab == &rabbitmq_vendor_vtab;
}

static bool channel_is_mail(const sc_channel *channel)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    return vtab == &mail_vtab;
}

static fake_channel *fake_from_handle(sc_channel *channel)
{
    if (!channel_is_fake(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static const fake_channel *fake_from_const_handle(const sc_channel *channel)
{
    if (!channel_is_fake(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static transport_channel *transport_from_handle(sc_channel *channel)
{
    if (!channel_is_transport(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static const transport_channel *transport_from_const_handle(const sc_channel *channel)
{
    if (!channel_is_transport(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static webhook_channel *webhook_from_handle(sc_channel *channel)
{
    if (!channel_is_webhook(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static const rabbitmq_channel *rabbitmq_from_const_handle(const sc_channel *channel)
{
    if (!channel_is_rabbitmq_vendor(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static const mail_channel *mail_from_const_handle(const sc_channel *channel)
{
    if (!channel_is_mail(channel)) {
        return nullptr;
    }
    return channel->base.impl;
}

static sc_status fake_send(void *impl, const sc_channel_message *message)
{
    fake_channel *fake = impl;
    stored_outbound outbound = {0};
    sc_status status = sc_status_ok();
    if (fake == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_fake.send_invalid_argument");
    }
    status = stored_outbound_from_public(fake->alloc, message, &outbound);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&fake->sent, &outbound);
    }
    if (!sc_status_is_ok(status)) {
        stored_outbound_clear(&outbound);
    }
    return status;
}

static sc_status fake_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    fake_channel *fake = impl;
    stored_inbound *stored = nullptr;
    sc_status status = sc_status_ok();
    (void)alloc;
    if (fake == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_fake.listen_invalid_argument");
    }
    if (fake->inbox.len == 0) {
        return sc_status_timeout("sc.channel_fake.no_message");
    }
    stored = sc_vec_at(&fake->inbox, 0);
    status = public_inbound_from_stored(fake->alloc, stored, out);
    if (sc_status_is_ok(status)) {
        stored_inbound_clear(stored);
        if (fake->inbox.len > 1) {
            (void)memmove(fake->inbox.ptr,
                          (unsigned char *)fake->inbox.ptr + fake->inbox.item_size,
                          (fake->inbox.len - 1) * fake->inbox.item_size);
        }
        fake->inbox.len -= 1;
    }
    return status;
}

static sc_status fake_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    (void)impl;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_fake.health_invalid_argument");
    }
    *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
    return sc_string_from_cstr(alloc, "fake ok", &out->message);
}

static sc_status fake_request_approval(void *impl,
                                       const sc_channel_approval_request *request,
                                       sc_allocator *alloc,
                                       sc_channel_approval_response *out)
{
    const fake_channel *fake = impl;
    (void)request;
    if (fake == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_fake.approval_invalid_argument");
    }
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = fake->approval};
    return sc_string_from_cstr(alloc, fake->approval == SC_CHANNEL_APPROVAL_DENIED ? "denied" : "approved", &out->reason);
}

static void fake_destroy(void *impl)
{
    fake_channel *fake = impl;
    if (fake == nullptr) {
        return;
    }
    for (size_t i = 0; i < fake->inbox.len; i += 1) {
        stored_inbound *message = sc_vec_at(&fake->inbox, i);
        stored_inbound_clear(message);
    }
    for (size_t i = 0; i < fake->sent.len; i += 1) {
        stored_outbound *message = sc_vec_at(&fake->sent, i);
        stored_outbound_clear(message);
    }
    sc_vec_clear(&fake->inbox);
    sc_vec_clear(&fake->sent);
    sc_free(fake->alloc, fake, sizeof(*fake), _Alignof(fake_channel));
}

static sc_status transport_send(void *impl, const sc_channel_message *message)
{
    transport_channel *transport = impl;
    sc_string text = {0};
    sc_status status = sc_status_ok();
    if (transport == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_transport.send_invalid_argument");
    }
    status = copy_string(transport->alloc, message->text, &text);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&transport->sent, &text);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&text);
    }
    return status;
}

static sc_status transport_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    transport_channel *transport = impl;
    stored_inbound *stored = nullptr;
    sc_status status = sc_status_ok();
    (void)alloc;
    if (transport == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_transport.listen_invalid_argument");
    }
    if (transport->inbox.len == 0) {
        return sc_status_timeout("sc.channel_transport.no_message");
    }
    stored = sc_vec_at(&transport->inbox, 0);
    status = public_inbound_from_stored(transport->alloc, stored, out);
    if (sc_status_is_ok(status)) {
        stored_inbound_clear(stored);
        if (transport->inbox.len > 1) {
            (void)memmove(transport->inbox.ptr,
                          (unsigned char *)transport->inbox.ptr + transport->inbox.item_size,
                          (transport->inbox.len - 1) * transport->inbox.item_size);
        }
        transport->inbox.len -= 1;
    }
    return status;
}

static sc_status transport_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    const transport_channel *transport = impl;
    sc_string_builder builder = {0};
    sc_string message = {0};
    sc_status status = sc_status_ok();
    if (transport == nullptr || out == nullptr || transport->vtab == nullptr) {
        return sc_status_invalid_argument("sc.channel_transport.health_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, transport->vtab->name);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " configured");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &message);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
        out->message = message;
        message = (sc_string){0};
    }
    sc_string_clear(&message);
    return status;
}

static sc_status transport_request_approval(void *impl,
                                            const sc_channel_approval_request *request,
                                            sc_allocator *alloc,
                                            sc_channel_approval_response *out)
{
    (void)impl;
    (void)request;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_transport.approval_invalid_argument");
    }
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
    return sc_string_from_cstr(alloc, "transport approval prompts are not interactive", &out->reason);
}

static void transport_destroy(void *impl)
{
    transport_channel *transport = impl;
    if (transport == nullptr) {
        return;
    }
    sc_string_clear(&transport->endpoint);
    sc_string_clear(&transport->topic);
    sc_string_clear(&transport->queue);
    sc_string_clear(&transport->client_id);
    for (size_t i = 0; i < transport->inbox.len; i += 1) {
        stored_inbound *message = sc_vec_at(&transport->inbox, i);
        stored_inbound_clear(message);
    }
    for (size_t i = 0; i < transport->sent.len; i += 1) {
        sc_string *message = sc_vec_at(&transport->sent, i);
        sc_string_clear(message);
    }
    sc_vec_clear(&transport->inbox);
    sc_vec_clear(&transport->sent);
    sc_free(transport->alloc, transport, sizeof(*transport), _Alignof(transport_channel));
}

static sc_status transport_new(sc_allocator *alloc,
                               const sc_channel_transport_options *options,
                               const sc_channel_vtab *vtab,
                               sc_channel **out)
{
    transport_channel *transport = nullptr;
    sc_status status = sc_status_ok();
    if (out == nullptr || options == nullptr || vtab == nullptr || options->endpoint.len == 0) {
        return sc_status_invalid_argument("sc.channel_transport.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    transport = sc_alloc(alloc, sizeof(*transport), _Alignof(transport_channel));
    if (transport == nullptr) {
        return sc_status_no_memory();
    }
    *transport = (transport_channel){.alloc = alloc, .vtab = vtab};
    sc_vec_init(&transport->inbox, alloc, sizeof(stored_inbound));
    sc_vec_init(&transport->sent, alloc, sizeof(sc_string));
    status = copy_string(alloc, options->endpoint, &transport->endpoint);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->topic, &transport->topic);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->queue, &transport->queue);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->client_id, &transport->client_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_channel_new(alloc, vtab, transport, out);
    }
    if (!sc_status_is_ok(status)) {
        transport_destroy(transport);
    }
    return status;
}

static sc_status webhook_send(void *impl, const sc_channel_message *message)
{
    (void)impl;
    (void)message;
    return sc_status_ok();
}

static sc_status webhook_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    webhook_channel *webhook = impl;
    (void)alloc;
    if (webhook == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.listen_invalid_argument");
    }
    return queue_listen(&webhook->inbox, webhook->alloc, out, "sc.channel_webhook.no_message");
}

static sc_status webhook_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    const webhook_channel *webhook = impl;
    bool use_libuv_transport = false;
    if (webhook == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.health_invalid_argument");
    }
    use_libuv_transport = webhook->async_initialized;
    *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
    return sc_string_from_cstr(alloc,
                               use_libuv_transport ? "webhook configured with libuv ingestion" : "webhook configured",
                               &out->message);
}

static sc_status webhook_request_approval(void *impl,
                                          const sc_channel_approval_request *request,
                                          sc_allocator *alloc,
                                          sc_channel_approval_response *out)
{
    (void)impl;
    (void)request;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.approval_invalid_argument");
    }
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
    return sc_string_from_cstr(alloc, "webhook approval prompts are not interactive", &out->reason);
}

static void webhook_destroy(void *impl)
{
    webhook_channel *webhook = impl;
    if (webhook == nullptr) {
        return;
    }
    if (webhook->async_initialized && !uv_is_closing((uv_handle_t *)&webhook->async)) {
        uv_close((uv_handle_t *)&webhook->async, nullptr);
    }
    sc_string_clear(&webhook->bind);
    sc_string_clear(&webhook->path);
    sc_string_secure_clear(&webhook->auth_token);
    sc_string_clear(&webhook->media_temp_dir);
    string_vec_clear(&webhook->allowed_ips);
    for (size_t i = 0; i < webhook->seen_nonces.len; i += 1) {
        sc_string *nonce = sc_vec_at(&webhook->seen_nonces, i);
        sc_string_clear(nonce);
    }
    for (size_t i = 0; i < webhook->inbox.len; i += 1) {
        stored_inbound *message = sc_vec_at(&webhook->inbox, i);
        stored_inbound_clear(message);
    }
    sc_vec_clear(&webhook->seen_nonces);
    sc_vec_clear(&webhook->inbox);
    sc_free(webhook->alloc, webhook, sizeof(*webhook), _Alignof(webhook_channel));
}

static sc_status rabbitmq_send(void *impl, const sc_channel_message *message)
{
    rabbitmq_channel *rabbitmq = impl;
    sc_string text = {0};
    sc_status status = sc_status_ok();
    if (rabbitmq == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_rabbitmq.send_invalid_argument");
    }
    status = copy_string(rabbitmq->alloc, message->text, &text);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&rabbitmq->sent, &text);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&text);
    }
    return status;
}

static sc_status rabbitmq_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    rabbitmq_channel *rabbitmq = impl;
    (void)alloc;
    if (rabbitmq == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_rabbitmq.listen_invalid_argument");
    }
    return queue_listen(&rabbitmq->inbox, rabbitmq->alloc, out, "sc.channel_rabbitmq.no_message");
}

static sc_status rabbitmq_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    const rabbitmq_channel *rabbitmq = impl;
    sc_string_builder builder = {0};
    sc_string message = {0};
    sc_status status = sc_status_ok();
    if (rabbitmq == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_rabbitmq.health_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "rabbitmq configured");
#ifdef SC_HAVE_RABBITMQ
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " using rabbitmq-c ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, amqp_version());
    }
#else
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " without vendor library");
    }
#endif
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &message);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
        out->message = message;
        message = (sc_string){0};
    }
    sc_string_clear(&message);
    return status;
}

static sc_status rabbitmq_request_approval(void *impl,
                                           const sc_channel_approval_request *request,
                                           sc_allocator *alloc,
                                           sc_channel_approval_response *out)
{
    (void)impl;
    (void)request;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_rabbitmq.approval_invalid_argument");
    }
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
    return sc_string_from_cstr(alloc, "rabbitmq approval prompts are not interactive", &out->reason);
}

static void rabbitmq_destroy(void *impl)
{
    rabbitmq_channel *rabbitmq = impl;
    if (rabbitmq == nullptr) {
        return;
    }
    sc_string_secure_clear(&rabbitmq->url);
    sc_string_clear(&rabbitmq->exchange);
    sc_string_clear(&rabbitmq->routing_key);
    sc_string_clear(&rabbitmq->queue);
    sc_string_clear(&rabbitmq->consumer_tag);
    for (size_t i = 0; i < rabbitmq->inbox.len; i += 1) {
        stored_inbound *message = sc_vec_at(&rabbitmq->inbox, i);
        stored_inbound_clear(message);
    }
    for (size_t i = 0; i < rabbitmq->sent.len; i += 1) {
        sc_string *message = sc_vec_at(&rabbitmq->sent, i);
        sc_string_clear(message);
    }
    sc_vec_clear(&rabbitmq->inbox);
    sc_vec_clear(&rabbitmq->sent);
    sc_free(rabbitmq->alloc, rabbitmq, sizeof(*rabbitmq), _Alignof(rabbitmq_channel));
}

static sc_status mail_send(void *impl, const sc_channel_message *message)
{
    mail_channel *mail = impl;
    sc_string payload = {0};
    sc_string response = {0};
    sc_string saved = {0};
    sc_status status = sc_status_ok();
    if (mail == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.channel_mail.send_invalid_argument");
    }
    status = mail_build_payload(mail, message, mail->alloc, &payload);
    if (sc_status_is_ok(status)) {
        status = mail_deliver(mail, sc_string_as_str(&payload), mail->alloc, &response);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(mail->alloc, sc_string_as_str(&payload), &saved);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&mail->sent, &saved);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&saved);
    }
    sc_string_clear(&response);
    sc_string_clear(&payload);
    return status;
}

static sc_status mail_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    mail_channel *mail = impl;
    sc_string raw = {0};
    sc_channel_inbound inbound = {0};
    stored_inbound stored = {0};
    if (mail == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_mail.listen_invalid_argument");
    }
    if (mail->inbox.len == 0) {
        sc_status status = mail_fetch(mail, alloc == nullptr ? mail->alloc : alloc, &raw);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        if (raw.len == 0) {
            sc_string_clear(&raw);
            return sc_status_timeout("sc.channel_mail.no_message");
        }
        status = mail_parse_message(mail->alloc, sc_string_as_str(&raw), sc_string_as_str(&mail->inbox_url), &inbound);
        if (sc_status_is_ok(status)) {
            status = mail_apply_filters(mail, &inbound);
        }
        if (sc_status_is_ok(status)) {
            status = mail_store_attachment_metadata(mail, sc_string_as_str(&raw), &inbound);
        }
        if (sc_status_is_ok(status)) {
            status = stored_inbound_from_public(mail->alloc, &inbound, &stored);
        }
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(&mail->inbox, &stored);
        }
        if (!sc_status_is_ok(status)) {
            stored_inbound_clear(&stored);
        }
        sc_channel_inbound_clear(&inbound);
        sc_string_clear(&raw);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    return queue_listen(&mail->inbox, mail->alloc, out, "sc.channel_mail.no_message");
}

static sc_status mail_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    const mail_channel *mail = impl;
    if (mail == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_mail.health_invalid_argument");
    }
    *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
#ifdef SC_HAVE_LIBCURL
    return sc_string_from_cstr(alloc, "mail configured with libcurl SMTP/POP3/IMAP", &out->message);
#else
    return sc_string_from_cstr(alloc, mail->request == nullptr ? "mail configured without libcurl" : "mail configured with custom transport", &out->message);
#endif
}

static sc_status mail_request_approval(void *impl,
                                       const sc_channel_approval_request *request,
                                       sc_allocator *alloc,
                                       sc_channel_approval_response *out)
{
    (void)impl;
    (void)request;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_mail.approval_invalid_argument");
    }
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
    return sc_string_from_cstr(alloc, "mail approval prompts are not interactive", &out->reason);
}

static bool mail_sender_allowed(const mail_channel *mail, sc_str sender)
{
    if (mail == nullptr) {
        return false;
    }
    return string_vec_contains_wildcard_or_value(&mail->allowed_senders, sender);
}

static sc_status mail_apply_filters(mail_channel *mail, sc_channel_inbound *inbound)
{
    if (mail == nullptr || inbound == nullptr) {
        return sc_status_invalid_argument("sc.channel_mail.filter_invalid_argument");
    }
    if (!mail_sender_allowed(mail, inbound->sender_id)) {
        return sc_status_security_denied("sc.channel_mail.sender_denied");
    }
    if (mail->subject_prefix.len > 0) {
        sc_str text = inbound->text;
        sc_str prefix = sc_string_as_str(&mail->subject_prefix);
        if (text.len < prefix.len || memcmp(text.ptr, prefix.ptr, prefix.len) != 0) {
            return sc_status_security_denied("sc.channel_mail.subject_prefix_denied");
        }
    }
    return sc_status_ok();
}

static bool mail_attachment_name_safe(sc_str name)
{
    if (name.ptr == nullptr || name.len == 0 || name.len > 128) {
        return false;
    }
    for (size_t i = 0; i < name.len; i += 1) {
        char ch = name.ptr[i];
        if (ch == '/' || ch == '\\' || ch == '\0') {
            return false;
        }
    }
    return name.len != 2 || memcmp(name.ptr, "..", 2) != 0;
}

static sc_status mail_store_attachment_metadata(mail_channel *mail, sc_str raw, sc_channel_inbound *inbound)
{
    sc_str name = trim_ascii(mail_header_value(raw, "Attachment-Name"));
    sc_str media_type = trim_ascii(mail_header_value(raw, "Attachment-Type"));
    sc_str size_text = trim_ascii(mail_header_value(raw, "Attachment-Size"));
    sc_string_builder builder = {0};
    sc_string path = {0};
    sc_status status = sc_status_ok();
    size_t size = 0;

    if (mail == nullptr || inbound == nullptr || name.len == 0) {
        return sc_status_ok();
    }
    if (!mail_attachment_name_safe(name)) {
        return sc_status_security_denied("sc.channel_mail.attachment_name_denied");
    }
    for (size_t i = 0; i < size_text.len; i += 1) {
        if (size_text.ptr[i] < '0' || size_text.ptr[i] > '9') {
            return sc_status_parse("sc.channel_mail.attachment_size_invalid");
        }
        size = (size * 10u) + (size_t)(size_text.ptr[i] - '0');
    }
    sc_string_clear(&inbound->owned_attachment_name);
    sc_string_clear(&inbound->owned_attachment_media_type);
    sc_string_clear(&inbound->owned_attachment_storage_path);
    status = sc_string_from_str(mail->alloc, name, &inbound->owned_attachment_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(mail->alloc, media_type, &inbound->owned_attachment_media_type);
    }
    if (sc_status_is_ok(status) && mail->attachment_dir.len > 0) {
        sc_string_builder_init(&builder, mail->alloc);
        status = sc_string_builder_append(&builder, sc_string_as_str(&mail->attachment_dir));
        if (sc_status_is_ok(status) && mail->attachment_dir.ptr[mail->attachment_dir.len - 1] != '/') {
            status = sc_string_builder_append_cstr(&builder, "/");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, inbound->conversation_id);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "/");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &path);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status)) {
            inbound->owned_attachment_storage_path = path;
            path = (sc_string){0};
        }
    }
    if (sc_status_is_ok(status)) {
        inbound->attachment_name = sc_string_as_str(&inbound->owned_attachment_name);
        inbound->attachment_media_type = sc_string_as_str(&inbound->owned_attachment_media_type);
        inbound->attachment_storage_path = sc_string_as_str(&inbound->owned_attachment_storage_path);
        inbound->attachment_size_bytes = size;
    }
    sc_string_clear(&path);
    return status;
}

static void mail_destroy(void *impl)
{
    mail_channel *mail = impl;
    if (mail == nullptr) {
        return;
    }
    sc_string_clear(&mail->inbox_url);
    sc_string_clear(&mail->smtp_url);
    sc_string_clear(&mail->username);
    sc_string_secure_clear(&mail->password);
    sc_string_clear(&mail->from);
    sc_string_clear(&mail->to);
    sc_string_clear(&mail->mailbox);
    sc_string_secure_clear(&mail->oauth_token);
    sc_string_clear(&mail->subject_prefix);
    sc_string_clear(&mail->attachment_dir);
    string_vec_clear(&mail->allowed_senders);
    for (size_t i = 0; i < mail->inbox.len; i += 1) {
        stored_inbound *message = sc_vec_at(&mail->inbox, i);
        stored_inbound_clear(message);
    }
    for (size_t i = 0; i < mail->sent.len; i += 1) {
        sc_string *message = sc_vec_at(&mail->sent, i);
        sc_string_clear(message);
    }
    sc_vec_clear(&mail->inbox);
    sc_vec_clear(&mail->sent);
    sc_free(mail->alloc, mail, sizeof(*mail), _Alignof(mail_channel));
}

static sc_status mail_fetch(mail_channel *mail, sc_allocator *alloc, sc_string *out)
{
    if (mail == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_mail.fetch_invalid_argument");
    }
    if (mail->request != nullptr) {
        return mail->request(mail->request_user,
                             sc_str_from_cstr("fetch"),
                             sc_string_as_str(&mail->inbox_url),
                             sc_string_as_str(&mail->username),
                             mail->oauth_token.len > 0 ? sc_string_as_str(&mail->oauth_token) : sc_string_as_str(&mail->password),
                             sc_str_from_cstr(""),
                             alloc,
                             out);
    }
#ifdef SC_HAVE_LIBCURL
    return mail_curl_request(mail, sc_str_from_cstr("fetch"), sc_string_as_str(&mail->inbox_url), sc_str_from_cstr(""), alloc, out);
#else
    return sc_status_unsupported("sc.channel_mail.libcurl_unavailable");
#endif
}

static sc_status mail_deliver(mail_channel *mail, sc_str payload, sc_allocator *alloc, sc_string *out)
{
    if (mail == nullptr || out == nullptr || mail->smtp_url.len == 0) {
        return sc_status_invalid_argument("sc.channel_mail.deliver_invalid_argument");
    }
    if (mail->request != nullptr) {
        return mail->request(mail->request_user,
                             sc_str_from_cstr("send"),
                             sc_string_as_str(&mail->smtp_url),
                             sc_string_as_str(&mail->username),
                             mail->oauth_token.len > 0 ? sc_string_as_str(&mail->oauth_token) : sc_string_as_str(&mail->password),
                             payload,
                             alloc,
                             out);
    }
#ifdef SC_HAVE_LIBCURL
    return mail_curl_request(mail, sc_str_from_cstr("send"), sc_string_as_str(&mail->smtp_url), payload, alloc, out);
#else
    return sc_status_unsupported("sc.channel_mail.libcurl_unavailable");
#endif
}

static sc_status mail_parse_message(sc_allocator *alloc, sc_str raw, sc_str inbox_url, sc_channel_inbound *out)
{
    sc_str message_id = trim_ascii(mail_header_value(raw, "Message-ID"));
    sc_str from = trim_ascii(mail_header_value(raw, "From"));
    sc_str subject = trim_ascii(mail_header_value(raw, "Subject"));
    sc_str body = trim_ascii(mail_body(raw));
    sc_string text = {0};
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || raw.ptr == nullptr || raw.len == 0) {
        return sc_status_invalid_argument("sc.channel_mail.parse_invalid_argument");
    }
    if (body.len == 0 && subject.len == 0) {
        return sc_status_parse("sc.channel_mail.empty_message");
    }
    sc_string_builder_init(&builder, alloc);
    if (subject.len > 0) {
        status = sc_string_builder_append(&builder, subject);
        if (sc_status_is_ok(status) && body.len > 0) {
            status = sc_string_builder_append_cstr(&builder, "\n\n");
        }
    } else {
        status = sc_status_ok();
    }
    if (sc_status_is_ok(status) && body.len > 0) {
        status = sc_string_builder_append(&builder, body);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        *out = (sc_channel_inbound){.struct_size = sizeof(*out)};
        status = message_id.len == 0 ? sc_string_from_cstr(alloc, "mail", &out->owned_message_id)
                                     : sc_string_from_str(alloc, message_id, &out->owned_message_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "mail", &out->owned_channel_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, inbox_url, &out->owned_conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "", &out->owned_thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = from.len == 0 ? sc_string_from_cstr(alloc, "mail", &out->owned_sender_id)
                               : sc_string_from_str(alloc, from, &out->owned_sender_id);
    }
    if (sc_status_is_ok(status)) {
        out->owned_text = text;
        text = (sc_string){0};
        out->message_id = sc_string_as_str(&out->owned_message_id);
        out->channel_name = sc_string_as_str(&out->owned_channel_name);
        out->conversation_id = sc_string_as_str(&out->owned_conversation_id);
        out->thread_id = sc_string_as_str(&out->owned_thread_id);
        out->sender_id = sc_string_as_str(&out->owned_sender_id);
        out->text = sc_string_as_str(&out->owned_text);
    } else {
        sc_channel_inbound_clear(out);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status mail_build_payload(mail_channel *mail, const sc_channel_message *message, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "From: ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&mail->from));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\nTo: ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&mail->to));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\nSubject: ");
    }
    if (sc_status_is_ok(status) && mail->subject_prefix.len > 0) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&mail->subject_prefix));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " ");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "SmolClaw reply");
    }
    if (sc_status_is_ok(status) && message->reply_to_message_id.len > 0) {
        status = sc_string_builder_append_cstr(&builder, "\r\nIn-Reply-To: ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, message->reply_to_message_id);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\r\nReferences: ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, message->reply_to_message_id);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\n\r\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, message->text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\n");
    }
    if (sc_status_is_ok(status)) {
        return sc_string_builder_finish(&builder, out);
    }
    sc_string_builder_clear(&builder);
    return status;
}

static sc_str mail_header_value(sc_str raw, const char *name)
{
    size_t name_len = strlen(name);
    size_t i = 0;
    while (i < raw.len) {
        size_t line_start = i;
        size_t line_end = i;
        while (line_end < raw.len && raw.ptr[line_end] != '\n' && raw.ptr[line_end] != '\r') {
            line_end += 1;
        }
        if (line_end > line_start + name_len && memcmp(raw.ptr + line_start, name, name_len) == 0 &&
            raw.ptr[line_start + name_len] == ':') {
            return sc_str_from_parts(raw.ptr + line_start + name_len + 1u, line_end - line_start - name_len - 1u);
        }
        i = line_end;
        while (i < raw.len && (raw.ptr[i] == '\n' || raw.ptr[i] == '\r')) {
            i += 1;
        }
    }
    return sc_str_from_cstr("");
}

static sc_str mail_body(sc_str raw)
{
    for (size_t i = 0; i + 1u < raw.len; i += 1) {
        if (raw.ptr[i] == '\n' && raw.ptr[i + 1u] == '\n') {
            return sc_str_from_parts(raw.ptr + i + 2u, raw.len - i - 2u);
        }
        if (i + 3u < raw.len && raw.ptr[i] == '\r' && raw.ptr[i + 1u] == '\n' && raw.ptr[i + 2u] == '\r' && raw.ptr[i + 3u] == '\n') {
            return sc_str_from_parts(raw.ptr + i + 4u, raw.len - i - 4u);
        }
    }
    return sc_str_from_cstr("");
}

static sc_str trim_ascii(sc_str value)
{
    while (value.len > 0 && (value.ptr[0] == ' ' || value.ptr[0] == '\t' || value.ptr[0] == '\r' || value.ptr[0] == '\n')) {
        value.ptr += 1;
        value.len -= 1;
    }
    while (value.len > 0 &&
           (value.ptr[value.len - 1u] == ' ' || value.ptr[value.len - 1u] == '\t' ||
            value.ptr[value.len - 1u] == '\r' || value.ptr[value.len - 1u] == '\n')) {
        value.len -= 1;
    }
    return value;
}

#ifdef SC_HAVE_LIBCURL
static sc_status mail_curl_request(mail_channel *mail,
                                   sc_str operation,
                                   sc_str url,
                                   sc_str payload,
                                   sc_allocator *alloc,
                                   sc_string *out)
{
    CURL *curl = nullptr;
    CURLcode code;
    mail_curl_buffer response = {0};
    mail_upload upload = {.ptr = payload.ptr, .len = payload.len, .off = 0};
    struct curl_slist *recipients = nullptr;
    sc_status status = sc_status_ok();

    status = sc_curl_global_init("sc.channel_mail.curl_init_failed");
    if (!sc_status_is_ok(status)) {
        return sc_status_unsupported("sc.channel_mail.curl_init_failed");
    }
    curl = curl_easy_init();
    if (curl == nullptr) {
        return sc_status_unsupported("sc.channel_mail.curl_init_failed");
    }
    response.max_bytes = mail->max_message_bytes;
    sc_bytes_init(&response.bytes, alloc);
    (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
    if (mail->username.len > 0) {
        (void)curl_easy_setopt(curl, CURLOPT_USERNAME, mail->username.ptr);
    }
    if (mail->password.len > 0) {
        (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, mail->password.ptr);
    }
    if (mail->oauth_token.len > 0) {
        (void)curl_easy_setopt(curl, CURLOPT_XOAUTH2_BEARER, mail->oauth_token.ptr);
    }
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mail_curl_write_callback);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (sc_str_equal(operation, sc_str_from_cstr("send"))) {
        if (mail->from.len > 0) {
            (void)curl_easy_setopt(curl, CURLOPT_MAIL_FROM, mail->from.ptr);
        }
        if (mail->to.len > 0) {
            recipients = curl_slist_append(recipients, mail->to.ptr);
            (void)curl_easy_setopt(curl, CURLOPT_MAIL_RCPT, recipients);
        }
        (void)curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_READFUNCTION, mail_curl_read_callback);
        (void)curl_easy_setopt(curl, CURLOPT_READDATA, &upload);
        (void)curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)payload.len);
    }
    code = curl_easy_perform(curl);
    if (code != CURLE_OK || response.too_large) {
        status = response.too_large ? sc_status_invalid_argument("sc.channel_mail.message_too_large")
                                    : sc_status_io("sc.channel_mail.curl_failed");
    } else {
        status = sc_string_from_str(alloc, sc_str_from_parts((const char *)response.bytes.ptr, response.bytes.len), out);
    }
    if (recipients != nullptr) {
        curl_slist_free_all(recipients);
    }
    curl_easy_cleanup(curl);
    sc_bytes_clear(&response.bytes);
    return status;
}

static size_t mail_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mail_curl_buffer *response = userdata;
    size_t total = size * nmemb;
    if (response == nullptr || response->bytes.len + total > response->max_bytes) {
        if (response != nullptr) {
            response->too_large = true;
        }
        return 0;
    }
    return sc_bytes_append(&response->bytes, sc_buf_from_parts((const unsigned char *)ptr, total)).code == SC_OK ? total : 0;
}

static size_t mail_curl_read_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mail_upload *upload = userdata;
    size_t room = size * nmemb;
    size_t remaining;
    size_t count;
    if (upload == nullptr || ptr == nullptr || room == 0 || upload->off >= upload->len) {
        return 0;
    }
    remaining = upload->len - upload->off;
    count = remaining < room ? remaining : room;
    (void)memcpy(ptr, upload->ptr + upload->off, count);
    upload->off += count;
    return count;
}
#endif

static sc_status queue_listen(sc_vec *inbox, sc_allocator *owner_alloc, sc_channel_inbound *out, const char *empty_code)
{
    stored_inbound *stored = nullptr;
    sc_status status = sc_status_ok();
    if (inbox == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.channel_queue.listen_invalid_argument");
    }
    if (inbox->len == 0) {
        return sc_status_timeout(empty_code);
    }
    stored = sc_vec_at(inbox, 0);
    status = public_inbound_from_stored(owner_alloc, stored, out);
    if (sc_status_is_ok(status)) {
        stored_inbound_clear(stored);
        if (inbox->len > 1) {
            (void)memmove(inbox->ptr,
                          (unsigned char *)inbox->ptr + inbox->item_size,
                          (inbox->len - 1) * inbox->item_size);
        }
        inbox->len -= 1;
    }
    return status;
}

static sc_status parse_webhook_body(sc_allocator *alloc,
                                    sc_str body,
                                    sc_str remote_addr,
                                    size_t max_media_bytes,
                                    sc_str media_temp_dir,
                                    // cppcheck-suppress constParameterPointer
                                    sc_string *nonce,
                                    int64_t *timestamp_unix_secs,
                                    sc_channel_inbound *out)
{
    sc_status status = sc_status_ok();
    if (out == nullptr || nonce == nullptr || timestamp_unix_secs == nullptr) {
        return sc_status_invalid_argument("sc.channel_webhook.parse_invalid_argument");
    }
    *out = (sc_channel_inbound){.struct_size = sizeof(*out)};
    *timestamp_unix_secs = 0;
#ifdef SC_HAVE_PARSON
    char *json_text = nullptr;
    JSON_Value *value = nullptr;
    JSON_Object *object = nullptr;
    JSON_Object *attachment = nullptr;
    const char *event_type = nullptr;
    const char *message_id = nullptr;
    const char *platform_account_id = nullptr;
    const char *conversation_id = nullptr;
    const char *thread_id = nullptr;
    const char *sender_id = nullptr;
    const char *text = nullptr;
    const char *reply_to_message_id = nullptr;
    const char *attachment_media_type = nullptr;
    const char *attachment_name = nullptr;
    const char *attachment_id = nullptr;
    const char *attachment_data_base64 = nullptr;
    const char *nonce_text = nullptr;
    sc_string storage_path = {0};
    double attachment_size = 0.0;
    double timestamp = 0.0;

    json_text = sc_alloc(alloc, body.len + 1u, _Alignof(char));
    if (json_text == nullptr) {
        return sc_status_no_memory();
    }
    (void)memcpy(json_text, body.ptr, body.len);
    json_text[body.len] = '\0';
    value = json_parse_string(json_text);
    sc_free(alloc, json_text, body.len + 1u, _Alignof(char));
    if (value == nullptr || json_value_get_type(value) != JSONObject) {
        if (value != nullptr) {
            json_value_free(value);
        }
        return sc_status_parse("sc.channel_webhook.invalid_json");
    }
    object = json_value_get_object(value);
    event_type = json_object_get_string(object, "event_type");
    if (event_type == nullptr || event_type[0] == '\0') {
        event_type = "message";
    }
    if (strcmp(event_type, "message") != 0 && strcmp(event_type, "media") != 0) {
        json_value_free(value);
        return sc_status_unsupported("sc.channel_webhook.unsupported_event_type");
    }
    message_id = json_object_get_string(object, "message_id");
    platform_account_id = json_object_get_string(object, "platform_account_id");
    conversation_id = json_object_get_string(object, "conversation_id");
    thread_id = json_object_get_string(object, "thread_id");
    sender_id = json_object_get_string(object, "sender_id");
    text = json_object_get_string(object, "text");
    reply_to_message_id = json_object_get_string(object, "reply_to_message_id");
    nonce_text = json_object_get_string(object, "nonce");
    timestamp = json_object_get_number(object, "timestamp_unix_secs");
    if (timestamp > 0.0) {
        *timestamp_unix_secs = (int64_t)timestamp;
    }
    attachment = json_object_get_object(object, "attachment");
    if (attachment != nullptr) {
        attachment_id = json_object_get_string(attachment, "id");
        attachment_media_type = json_object_get_string(attachment, "media_type");
        attachment_name = json_object_get_string(attachment, "name");
        attachment_data_base64 = json_object_get_string(attachment, "data_base64");
        attachment_size = json_object_get_number(attachment, "size_bytes");
    } else {
        attachment_id = json_object_get_string(object, "attachment_id");
        attachment_media_type = json_object_get_string(object, "attachment_media_type");
        attachment_name = json_object_get_string(object, "attachment_name");
        attachment_data_base64 = json_object_get_string(object, "attachment_data_base64");
        attachment_size = json_object_get_number(object, "attachment_size_bytes");
    }
    if (attachment_size < 0.0) {
        json_value_free(value);
        return sc_status_parse("sc.channel_webhook.attachment_size_invalid");
    }
    if (max_media_bytes > 0 && attachment_size > (double)max_media_bytes) {
        json_value_free(value);
        return sc_status_invalid_argument("sc.channel_webhook.media_too_large");
    }
    if ((strcmp(event_type, "media") == 0 || attachment_size > 0.0) &&
        (attachment_media_type == nullptr || attachment_media_type[0] == '\0')) {
        json_value_free(value);
        return sc_status_parse("sc.channel_webhook.media_type_missing");
    }
    if (attachment_data_base64 != nullptr && attachment_data_base64[0] != '\0' &&
        (attachment_media_type == nullptr || attachment_media_type[0] == '\0')) {
        json_value_free(value);
        return sc_status_parse("sc.channel_webhook.media_type_missing");
    }
    if (attachment_data_base64 != nullptr && attachment_data_base64[0] != '\0') {
        sc_str media_id = attachment_id == nullptr ? sc_str_from_cstr("") : sc_str_from_cstr(attachment_id);
        status = webhook_store_media(alloc,
                                     media_temp_dir,
                                     media_id,
                                     sc_str_from_cstr(attachment_media_type),
                                     attachment_name == nullptr ? sc_str_from_cstr("") : sc_str_from_cstr(attachment_name),
                                     sc_str_from_cstr(attachment_data_base64),
                                     max_media_bytes,
                                     &storage_path,
                                     &out->attachment_size_bytes);
        if (!sc_status_is_ok(status)) {
            json_value_free(value);
            return status;
        }
        attachment_size = (double)out->attachment_size_bytes;
    }
    if (text == nullptr || text[0] == '\0') {
        if (strcmp(event_type, "media") == 0) {
            text = "[media]";
        } else {
            json_value_free(value);
            return sc_status_parse("sc.channel_webhook.text_missing");
        }
    }
    if (text == nullptr || text[0] == '\0') {
        json_value_free(value);
        return sc_status_parse("sc.channel_webhook.text_missing");
    }
    if (message_id == nullptr || message_id[0] == '\0') {
        message_id = "webhook";
    }
    if (conversation_id == nullptr || conversation_id[0] == '\0') {
        conversation_id = "webhook";
    }
    if (sender_id == nullptr || sender_id[0] == '\0') {
        sender_id = remote_addr.ptr == nullptr || remote_addr.len == 0 ? "webhook" : nullptr;
    }
    status = sc_string_from_cstr(alloc, message_id, &out->owned_message_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "webhook", &out->owned_channel_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, platform_account_id == nullptr ? "" : platform_account_id, &out->owned_platform_account_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, conversation_id, &out->owned_conversation_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, thread_id == nullptr ? "" : thread_id, &out->owned_thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = sender_id == nullptr ? sc_string_from_str(alloc, remote_addr, &out->owned_sender_id)
                                   : sc_string_from_cstr(alloc, sender_id, &out->owned_sender_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, text, &out->owned_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, reply_to_message_id == nullptr ? "" : reply_to_message_id, &out->owned_reply_to_message_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, attachment_media_type == nullptr ? "" : attachment_media_type, &out->owned_attachment_media_type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, attachment_name == nullptr ? "" : attachment_name, &out->owned_attachment_name);
    }
    if (sc_status_is_ok(status)) {
        out->owned_attachment_storage_path = storage_path;
        storage_path = (sc_string){0};
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, nonce_text == nullptr ? "" : nonce_text, nonce);
    }
    if (sc_status_is_ok(status)) {
        if (out->attachment_size_bytes == 0) {
            out->attachment_size_bytes = (size_t)attachment_size;
        }
        out->timestamp_unix_secs = *timestamp_unix_secs;
        out->timestamp_trusted = *timestamp_unix_secs > 0;
        out->attachment_temporary = out->owned_attachment_storage_path.len > 0;
    }
    json_value_free(value);
    sc_string_clear(&storage_path);
#else
    (void)alloc;
    (void)body;
    (void)remote_addr;
    (void)max_media_bytes;
    (void)media_temp_dir;
    status = sc_status_unsupported("sc.channel_webhook.parson_unavailable");
#endif
    if (sc_status_is_ok(status)) {
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
    } else {
        sc_channel_inbound_clear(out);
    }
    return status;
}

static void webhook_async_cb(uv_async_t *handle)
{
    (void)handle;
}

static sc_status cli_send(void *impl, const sc_channel_message *message)
{
    (void)impl;
    if (message == nullptr || message->text.ptr == nullptr) {
        return sc_status_invalid_argument("sc.channel_cli.send_invalid_argument");
    }
    if (fwrite(message->text.ptr, 1, message->text.len, stdout) != message->text.len || fputc('\n', stdout) == EOF) {
        return sc_status_io("sc.channel_cli.write_failed");
    }
    return sc_status_ok();
}

static sc_status cli_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    (void)impl;
    (void)alloc;
    (void)out;
    return sc_status_unsupported("sc.channel_cli.listen_unsupported");
}

static sc_status cli_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    (void)impl;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_cli.health_invalid_argument");
    }
    *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
    return sc_string_from_cstr(alloc, "cli ok", &out->message);
}

static sc_status cli_request_approval(void *impl,
                                      const sc_channel_approval_request *request,
                                      sc_allocator *alloc,
                                      sc_channel_approval_response *out)
{
    (void)impl;
    (void)request;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.channel_cli.approval_invalid_argument");
    }
    *out = (sc_channel_approval_response){.struct_size = sizeof(*out), .decision = SC_CHANNEL_APPROVAL_DENIED};
    return sc_string_from_cstr(alloc, "cli approval prompts are non-interactive", &out->reason);
}

static void cli_destroy(void *impl)
{
    cli_channel *cli = impl;
    if (cli == nullptr) {
        return;
    }
    sc_free(cli->alloc, cli, sizeof(*cli), _Alignof(cli_channel));
}

static sc_status channel_delivery_deliver(void *impl, const sc_delivery_message *message)
{
    channel_delivery_impl *delivery = impl;
    sc_channel *channel = delivery == nullptr ? nullptr : first_channel(delivery->orchestrator);
    sc_channel_message outbound = {0};
    if (delivery == nullptr || message == nullptr || channel == nullptr) {
        return sc_status_invalid_argument("sc.channel_delivery.invalid_argument");
    }
    outbound = (sc_channel_message){
        .struct_size = sizeof(outbound),
        .conversation_id = message->target,
        .text = message->content,
        .draft = false,
    };
    return sc_channel_send(channel, &outbound);
}

static void channel_delivery_destroy(void *impl)
{
    channel_delivery_impl *delivery = impl;
    if (delivery == nullptr) {
        return;
    }
    sc_free(delivery->alloc, delivery, sizeof(*delivery), _Alignof(channel_delivery_impl));
}
