#include "sc/channel.h"
#include "sc/provider.h"
#include "test_helpers.h"

#include "media/media_encoder_internal.h"

#include <sys/stat.h>
#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INBOUND(id_value, channel_value, conversation_value, thread_value, sender_value, text_value, cancel_value) \
    (&(sc_channel_inbound){                                                                                       \
        .struct_size = sizeof(sc_channel_inbound),                                                                \
        .message_id = sc_str_from_cstr(id_value),                                                                 \
        .channel_name = sc_str_from_cstr(channel_value),                                                          \
        .conversation_id = sc_str_from_cstr(conversation_value),                                                   \
        .thread_id = sc_str_from_cstr(thread_value),                                                              \
        .sender_id = sc_str_from_cstr(sender_value),                                                              \
        .text = sc_str_from_cstr(text_value),                                                                     \
        .cancel_previous = (cancel_value),                                                                        \
    })

#ifdef SC_HAVE_PARSON
#endif
static sc_status make_agent(const char *text, sc_provider **provider, sc_agent **agent);
static int test_fake_and_cli_channels(void);
static int test_webhook_channel_ingestion(void);
static int test_webhook_channel_rejections(void);
static int test_webhook_media_download_to_filesystem(void);
static int test_mail_channel_libcurl_fixture(void);
static int test_websocket_mqtt_rabbitmq_transports(void);
static int test_orchestrator_process_dedupe_allowlist_history(void);
static int test_orchestrator_bounded_dedupe(void);
static int test_provider_route_selection_per_channel(void);
static int test_streaming_approval_cancellation_poll_and_delivery(void);
static int test_runtime_failure_response(void);
static int test_telegram_channel_stub(void);
static sc_status route_provider_generate(void *impl,
                                         const sc_provider_request *request,
                                         sc_allocator *alloc,
                                         sc_provider_response *out);
static void route_provider_destroy(void *impl);
static sc_status fail_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out);
static void fail_provider_destroy(void *impl);
static int file_exists(const char *path);

static const sc_provider_vtab route_provider_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "route-provider",
    .display_name = "Route provider",
    .feature_flag = "SC_TEST_ROUTE_PROVIDER",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = route_provider_generate,
    .destroy = route_provider_destroy,
};

static const sc_provider_vtab fail_provider_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "fail-provider",
    .display_name = "Fail Provider",
    .feature_flag = "SC_TEST_FAIL_PROVIDER",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = fail_provider_generate,
    .destroy = fail_provider_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_fake_and_cli_channels();
    failures += test_webhook_channel_ingestion();
    failures += test_webhook_channel_rejections();
    failures += test_webhook_media_download_to_filesystem();
    failures += test_mail_channel_libcurl_fixture();
    failures += test_websocket_mqtt_rabbitmq_transports();
    failures += test_telegram_channel_stub();
    failures += test_orchestrator_process_dedupe_allowlist_history();
    failures += test_orchestrator_bounded_dedupe();
    failures += test_provider_route_selection_per_channel();
    failures += test_streaming_approval_cancellation_poll_and_delivery();
    failures += test_runtime_failure_response();

    return failures == 0 ? 0 : 1;
}

static int test_fake_and_cli_channels(void)
{
    int failures = 0;
    sc_channel *fake = nullptr;
    sc_channel *cli = nullptr;
    sc_channel_health health = {0};
    sc_channel_inbound inbound = {0};
    const sc_channel_vtab *vtab = nullptr;

    failures += sc_test_expect_status("fake new", sc_channel_fake_new(sc_allocator_heap(), 0, &fake), SC_OK);
    failures += sc_test_expect_status("fake health", sc_channel_health_check(fake, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("fake healthy", health.healthy && strcmp(health.message.ptr, "fake ok") == 0);
    sc_channel_health_clear(&health);
    failures += sc_test_expect_status("fake push",
                              sc_channel_fake_push_inbound(fake,
                                                           INBOUND("m1", "fake", "c1", "t1", "sender", "hello", false)),
                              SC_OK);
    failures += sc_test_expect_status("fake listen", sc_channel_listen(fake, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("fake listened text", strcmp(inbound.text.ptr, "hello") == 0);
    sc_channel_inbound_clear(&inbound);
    vtab = sc_channel_vtab_of(fake);
    failures += sc_test_expect_true("fake caps", vtab != nullptr && (vtab->capabilities & SC_CHANNEL_CAP_DRAFT_UPDATES) != 0);
    failures += sc_test_expect_true("fake descriptor",
                            vtab != nullptr && vtab->description_key != nullptr && vtab->config_schema_ref != nullptr &&
                                (vtab->inbound_event_types & SC_CHANNEL_INBOUND_TEXT) != 0);

    failures += sc_test_expect_status("cli new", sc_channel_cli_new(sc_allocator_heap(), &cli), SC_OK);
    failures += sc_test_expect_status("cli health", sc_channel_health_check(cli, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("cli healthy", health.healthy);
    sc_channel_health_clear(&health);

    sc_channel_destroy(cli);
    sc_channel_destroy(fake);
    return failures;
}

static int test_webhook_channel_ingestion(void)
{
    int failures = 0;
    sc_channel *webhook = nullptr;
    sc_channel_health health = {0};
    sc_channel_inbound inbound = {0};
    sc_str allowed_ips[] = {sc_str_from_cstr("127.0.0.1")};
    const char *body = "{\"message_id\":\"wh1\",\"conversation_id\":\"conv\",\"thread_id\":\"thr\","
                       "\"sender_id\":\"sender\",\"text\":\"webhook hello\"}";

    failures += sc_test_expect_status("webhook new",
                              sc_channel_webhook_new(sc_allocator_heap(),
                                                     &(sc_webhook_channel_options){
                                                         .struct_size = sizeof(sc_webhook_channel_options),
                                                         .bind = sc_str_from_cstr("127.0.0.1"),
                                                         .port = 8081,
                                                         .path = sc_str_from_cstr("/hook"),
                                                         .auth_token = sc_str_from_cstr("secret"),
                                                         .max_body_bytes = 1024,
                                                         .allowed_ips = allowed_ips,
                                                         .allowed_ip_count = 1,
                                                     },
                                                     &webhook),
                              SC_OK);
    failures += sc_test_expect_status("webhook health", sc_channel_health_check(webhook, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("webhook health text", strstr(health.message.ptr, "webhook configured") != nullptr);
    sc_channel_health_clear(&health);
    failures += sc_test_expect_status("webhook auth reject",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("bad"),
                                                            .body = sc_str_from_cstr(body),
                                                            .remote_addr = sc_str_from_cstr("127.0.0.1"),
                                                        }),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("webhook ip reject",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr(body),
                                                            .remote_addr = sc_str_from_cstr("198.51.100.4"),
                                                        }),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("webhook ingest",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr(body),
                                                            .remote_addr = sc_str_from_cstr("127.0.0.1"),
                                                        }),
                              SC_OK);
    failures += sc_test_expect_status("webhook listen", sc_channel_listen(webhook, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("webhook message id", strcmp(inbound.message_id.ptr, "wh1") == 0);
    failures += sc_test_expect_true("webhook channel name", strcmp(inbound.channel_name.ptr, "webhook") == 0);
    failures += sc_test_expect_true("webhook text", strcmp(inbound.text.ptr, "webhook hello") == 0);
    sc_channel_inbound_clear(&inbound);

    sc_channel_destroy(webhook);
    return failures;
}

static int test_webhook_channel_rejections(void)
{
    int failures = 0;
    sc_channel *webhook = nullptr;
    sc_channel *limited = nullptr;
    sc_channel_inbound inbound = {0};
    const char *valid = "{\"message_id\":\"wh2\",\"conversation_id\":\"conv\",\"sender_id\":\"sender\","
                        "\"text\":\"hello\",\"platform_account_id\":\"workspace\","
                        "\"reply_to_message_id\":\"parent\",\"nonce\":\"n1\"}";
    const char *duplicate = "{\"message_id\":\"wh3\",\"conversation_id\":\"conv\",\"sender_id\":\"sender\","
                            "\"text\":\"hello again\",\"nonce\":\"n1\"}";
    const char *unsupported = "{\"event_type\":\"join\",\"text\":\"ignored\"}";
    const char *media_too_large = "{\"event_type\":\"media\",\"conversation_id\":\"conv\",\"sender_id\":\"sender\","
                                  "\"attachment\":{\"media_type\":\"image/png\",\"name\":\"private.png\","
                                  "\"size_bytes\":2048}}";

    failures += sc_test_expect_status("webhook reject new",
                              sc_channel_webhook_new(sc_allocator_heap(),
                                                     &(sc_webhook_channel_options){
                                                         .struct_size = sizeof(sc_webhook_channel_options),
                                                         .path = sc_str_from_cstr("/hook"),
                                                         .auth_token = sc_str_from_cstr("secret"),
                                                         .max_body_bytes = 512,
                                                         .max_media_bytes = 1024,
                                                     },
                                                     &webhook),
                              SC_OK);
    failures += sc_test_expect_status("webhook malformed",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr("{bad"),
                                                        }),
                              SC_ERR_PARSE);
    failures += sc_test_expect_status("webhook oversized",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
                                                                                     "x"),
                                                        }),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("webhook unsupported event",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr(unsupported),
                                                        }),
                              SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_status("webhook media limit",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr(media_too_large),
                                                        }),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("webhook valid metadata",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr(valid),
                                                        }),
                              SC_OK);
    failures += sc_test_expect_status("webhook replay",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/hook"),
                                                            .auth_token = sc_str_from_cstr("secret"),
                                                            .body = sc_str_from_cstr(duplicate),
                                                        }),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("webhook metadata listen", sc_channel_listen(webhook, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("webhook platform account", strcmp(inbound.platform_account_id.ptr, "workspace") == 0);
    failures += sc_test_expect_true("webhook reply context", strcmp(inbound.reply_to_message_id.ptr, "parent") == 0);
    sc_channel_inbound_clear(&inbound);

    failures += sc_test_expect_status("webhook limit new",
                              sc_channel_webhook_new(sc_allocator_heap(),
                                                     &(sc_webhook_channel_options){
                                                         .struct_size = sizeof(sc_webhook_channel_options),
                                                         .path = sc_str_from_cstr("/limited"),
                                                         .rate_limit_per_minute = 1,
                                                     },
                                                     &limited),
                              SC_OK);
    failures += sc_test_expect_status("webhook rate first",
                              sc_channel_webhook_ingest(limited,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/limited"),
                                                            .body = sc_str_from_cstr("{\"text\":\"one\"}"),
                                                        }),
                              SC_OK);
    failures += sc_test_expect_status("webhook rate second",
                              sc_channel_webhook_ingest(limited,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/limited"),
                                                            .body = sc_str_from_cstr("{\"text\":\"two\"}"),
                                                        }),
                              SC_ERR_SECURITY_DENIED);

    sc_channel_destroy(limited);
    sc_channel_destroy(webhook);
    return failures;
}

static int test_webhook_media_download_to_filesystem(void)
{
    int failures = 0;
    sc_string dir = {0};
    sc_string path_copy = {0};
    sc_channel *webhook = nullptr;
    sc_channel_inbound inbound = {0};
    const char *body = "{\"event_type\":\"media\",\"message_id\":\"media-1\","
                       "\"conversation_id\":\"conv\",\"sender_id\":\"sender\","
                       "\"attachment\":{\"id\":\"media1\",\"media_type\":\"image/png\","
                       "\"name\":\"private.png\",\"data_base64\":\"iVBORw0KGgo=\"}}";
    const char *bad_audio = "{\"event_type\":\"media\",\"message_id\":\"media-2\","
                            "\"conversation_id\":\"conv\",\"sender_id\":\"sender\","
                            "\"attachment\":{\"id\":\"media2\",\"media_type\":\"audio/wav\","
                            "\"name\":\"private.wav\",\"data_base64\":\"AAAA\"}}";

    failures += sc_test_expect_status("media temp dir", sc_test_make_temp_dir("channels", &dir), SC_OK);
    failures += sc_test_expect_status("media webhook new",
                              sc_channel_webhook_new(sc_allocator_heap(),
                                                     &(sc_webhook_channel_options){
                                                         .struct_size = sizeof(sc_webhook_channel_options),
                                                         .path = sc_str_from_cstr("/media"),
                                                         .media_temp_dir = sc_string_as_str(&dir),
                                                         .max_media_bytes = 64,
                                                     },
                                                     &webhook),
                              SC_OK);
    failures += sc_test_expect_status("media webhook ingest",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/media"),
                                                            .body = sc_str_from_cstr(body),
                                                        }),
                              SC_OK);
    failures += sc_test_expect_status("media webhook listen", sc_channel_listen(webhook, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("media text normalized", strcmp(inbound.text.ptr, "[media]") == 0);
    failures += sc_test_expect_true("media content type", strcmp(inbound.attachment_media_type.ptr, "image/png") == 0);
    failures += sc_test_expect_true("media storage path", strstr(inbound.attachment_storage_path.ptr, "sc-media-media1.bin") != nullptr);
    failures += sc_test_expect_true("media size", inbound.attachment_size_bytes == 8);
    failures += sc_test_expect_true("media file exists", file_exists(inbound.attachment_storage_path.ptr));
    failures += sc_test_expect_status("media path copy", sc_string_from_str(sc_allocator_heap(), inbound.attachment_storage_path, &path_copy), SC_OK);
    sc_channel_inbound_clear(&inbound);
    failures += sc_test_expect_true("media cleanup on clear", !file_exists(path_copy.ptr));
    failures += sc_test_expect_status("media audio validation",
                              sc_channel_webhook_ingest(webhook,
                                                        &(sc_webhook_ingest_request){
                                                            .struct_size = sizeof(sc_webhook_ingest_request),
                                                            .method = sc_str_from_cstr("POST"),
                                                            .path = sc_str_from_cstr("/media"),
                                                            .body = sc_str_from_cstr(bad_audio),
                                                        }),
                              SC_ERR_PARSE);

    sc_string_clear(&path_copy);
    sc_channel_destroy(webhook);
    (void)rmdir(dir.ptr);
    sc_string_clear(&dir);
    return failures;
}

typedef struct mail_stub {
    sc_string operation;
    sc_string url;
    sc_string username;
    sc_string password;
    sc_string payload;
    const char *fetch_response;
    int calls;
} mail_stub;

static void mail_stub_clear(mail_stub *stub)
{
    if (stub == nullptr) {
        return;
    }
    sc_string_clear(&stub->operation);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->username);
    sc_string_secure_clear(&stub->password);
    sc_string_clear(&stub->payload);
}

static sc_status mail_stub_request(void *user,
                                   sc_str operation,
                                   sc_str url,
                                   sc_str username,
                                   sc_str password,
                                   sc_str payload,
                                   sc_allocator *alloc,
                                   sc_string *out)
{
    mail_stub *stub = user;
    sc_status status;
    if (stub == nullptr || out == nullptr) {
        return sc_status_invalid_argument("mail.stub.invalid_argument");
    }
    mail_stub_clear(stub);
    stub->calls += 1;
    status = sc_string_from_str(sc_allocator_heap(), operation, &stub->operation);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), url, &stub->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), username, &stub->username);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), password, &stub->password);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), payload, &stub->payload);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc,
                                     sc_str_equal(operation, sc_str_from_cstr("fetch")) ? stub->fetch_response : "OK",
                                     out);
    }
    return status;
}

static int test_mail_channel_libcurl_fixture(void)
{
    int failures = 0;
    mail_stub stub = {
        .fetch_response = "Message-ID: <mail-1>\r\n"
                          "From: sender@example.test\r\n"
                          "Subject: Inbox prompt\r\n"
                          "\r\n"
                          "Please summarize the report.\r\n",
    };
    sc_channel *mail = nullptr;
    sc_channel_health health = {0};
    sc_channel_inbound inbound = {0};
    sc_channel_message outbound = {
        .struct_size = sizeof(outbound),
        .conversation_id = sc_str_from_cstr("imap://mail.example.test/INBOX;UID=1"),
        .sender_id = sc_str_from_cstr("sender@example.test"),
        .text = sc_str_from_cstr("Reply body"),
    };

    failures += sc_test_expect_status("mail new",
                              sc_channel_mail_new(sc_allocator_heap(),
                                                  &(sc_mail_channel_options){
                                                      .struct_size = sizeof(sc_mail_channel_options),
                                                      .inbox_url = sc_str_from_cstr("imap://mail.example.test/INBOX;UID=1"),
                                                      .smtp_url = sc_str_from_cstr("smtp://mail.example.test"),
                                                      .username = sc_str_from_cstr("agent"),
                                                      .password = sc_str_from_cstr("secret"),
                                                      .from = sc_str_from_cstr("agent@example.test"),
                                                      .to = sc_str_from_cstr("sender@example.test"),
                                                      .max_message_bytes = 4096,
                                                      .request = mail_stub_request,
                                                      .request_user = &stub,
                                                  },
                                                  &mail),
                              SC_OK);
    failures += sc_test_expect_status("mail health", sc_channel_health_check(mail, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("mail health text", strstr(health.message.ptr, "mail configured") != nullptr);
    sc_channel_health_clear(&health);
    failures += sc_test_expect_status("mail listen", sc_channel_listen(mail, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("mail operation", strcmp(stub.operation.ptr, "fetch") == 0);
    failures += sc_test_expect_true("mail url", strcmp(stub.url.ptr, "imap://mail.example.test/INBOX;UID=1") == 0);
    failures += sc_test_expect_true("mail message id", strcmp(inbound.message_id.ptr, "<mail-1>") == 0);
    failures += sc_test_expect_true("mail channel", strcmp(inbound.channel_name.ptr, "mail") == 0);
    failures += sc_test_expect_true("mail sender", strcmp(inbound.sender_id.ptr, "sender@example.test") == 0);
    failures += sc_test_expect_true("mail text",
                            strcmp(inbound.text.ptr, "Inbox prompt\n\nPlease summarize the report.") == 0);
    sc_channel_inbound_clear(&inbound);

    failures += sc_test_expect_status("mail send", sc_channel_send(mail, &outbound), SC_OK);
    failures += sc_test_expect_true("mail send operation", strcmp(stub.operation.ptr, "send") == 0);
    failures += sc_test_expect_true("mail smtp url", strcmp(stub.url.ptr, "smtp://mail.example.test") == 0);
    failures += sc_test_expect_true("mail payload from", strstr(stub.payload.ptr, "From: agent@example.test") != nullptr);
    failures += sc_test_expect_true("mail payload body", strstr(stub.payload.ptr, "Reply body") != nullptr);
    failures += sc_test_expect_true("mail sent saved", strstr(sc_channel_fake_last_sent_text(mail).ptr, "Reply body") != nullptr);

    mail_stub_clear(&stub);
    sc_channel_destroy(mail);
    return failures;
}

static int test_websocket_mqtt_rabbitmq_transports(void)
{
    int failures = 0;
    sc_channel *websocket = nullptr;
    sc_channel *mqtt = nullptr;
    sc_channel *rabbitmq = nullptr;
    sc_channel *rabbitmq_vendor = nullptr;
    sc_channel_health health = {0};
    sc_channel_inbound inbound = {0};
    sc_channel_message outbound = {
        .struct_size = sizeof(outbound),
        .conversation_id = sc_str_from_cstr("c"),
        .text = sc_str_from_cstr("outbound"),
    };

    failures += sc_test_expect_status("websocket new",
                              sc_channel_websocket_client_new(sc_allocator_heap(),
                                                              &(sc_channel_transport_options){
                                                                  .struct_size = sizeof(sc_channel_transport_options),
                                                                  .endpoint = sc_str_from_cstr("wss://example.test/chat"),
                                                                  .client_id = sc_str_from_cstr("sc"),
                                                              },
                                                              &websocket),
                              SC_OK);
    failures += sc_test_expect_status("mqtt new",
                              sc_channel_mqtt_new(sc_allocator_heap(),
                                                  &(sc_channel_transport_options){
                                                      .struct_size = sizeof(sc_channel_transport_options),
                                                      .endpoint = sc_str_from_cstr("mqtt://broker.test"),
                                                      .topic = sc_str_from_cstr("smolclaw/in"),
                                                  },
                                                  &mqtt),
                              SC_OK);
    failures += sc_test_expect_status("rabbitmq new",
                              sc_channel_rabbitmq_new(sc_allocator_heap(),
                                                      &(sc_channel_transport_options){
                                                          .struct_size = sizeof(sc_channel_transport_options),
                                                          .endpoint = sc_str_from_cstr("amqp://broker.test"),
                                                          .queue = sc_str_from_cstr("smolclaw"),
                                                      },
                                                      &rabbitmq),
                              SC_OK);
    failures += sc_test_expect_status("rabbitmq vendor new",
                              sc_channel_rabbitmq_vendor_new(sc_allocator_heap(),
                                                             &(sc_rabbitmq_channel_options){
                                                                 .struct_size = sizeof(sc_rabbitmq_channel_options),
                                                                 .url = sc_str_from_cstr("amqp://guest:guest@broker.test:5672/%2f"),
                                                                 .exchange = sc_str_from_cstr("smolclaw"),
                                                                 .routing_key = sc_str_from_cstr("smolclaw.outbound"),
                                                                 .queue = sc_str_from_cstr("smolclaw"),
                                                                 .consumer_tag = sc_str_from_cstr("test"),
                                                                 .prefetch = 2,
                                                                 .durable = true,
                                                             },
                                                             &rabbitmq_vendor),
                              SC_OK);
    failures += sc_test_expect_status("websocket health", sc_channel_health_check(websocket, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("websocket health text", strstr(health.message.ptr, "websocket-client configured") != nullptr);
    sc_channel_health_clear(&health);
    failures += sc_test_expect_status("mqtt health", sc_channel_health_check(mqtt, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("mqtt health text", strstr(health.message.ptr, "mqtt configured") != nullptr);
    sc_channel_health_clear(&health);
    failures += sc_test_expect_status("rabbitmq health", sc_channel_health_check(rabbitmq, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("rabbitmq health text", strstr(health.message.ptr, "rabbitmq configured") != nullptr);
    sc_channel_health_clear(&health);
    failures += sc_test_expect_status("rabbitmq vendor health", sc_channel_health_check(rabbitmq_vendor, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("rabbitmq vendor health text", strstr(health.message.ptr, "rabbitmq configured") != nullptr);
    sc_channel_health_clear(&health);

    failures += sc_test_expect_status("websocket push",
                              sc_channel_transport_push_inbound(websocket,
                                                                INBOUND("ws1", "websocket-client", "c", "t", "sender", "ws hello", false)),
                              SC_OK);
    failures += sc_test_expect_status("websocket listen", sc_channel_listen(websocket, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("websocket inbound", strcmp(inbound.text.ptr, "ws hello") == 0);
    sc_channel_inbound_clear(&inbound);
    failures += sc_test_expect_status("mqtt send", sc_channel_send(mqtt, &outbound), SC_OK);
    failures += sc_test_expect_status("rabbitmq send", sc_channel_send(rabbitmq, &outbound), SC_OK);
    failures += sc_test_expect_status("rabbitmq vendor send", sc_channel_send(rabbitmq_vendor, &outbound), SC_OK);
    failures += sc_test_expect_true("mqtt sent", strcmp(sc_channel_fake_last_sent_text(mqtt).ptr, "outbound") == 0);
    failures += sc_test_expect_true("rabbitmq sent", strcmp(sc_channel_fake_last_sent_text(rabbitmq).ptr, "outbound") == 0);
    failures += sc_test_expect_true("rabbitmq vendor sent", strcmp(sc_channel_fake_last_sent_text(rabbitmq_vendor).ptr, "outbound") == 0);

    sc_channel_destroy(rabbitmq_vendor);
    sc_channel_destroy(rabbitmq);
    sc_channel_destroy(mqtt);
    sc_channel_destroy(websocket);
    return failures;
}

#ifdef SC_HAVE_PARSON
typedef struct telegram_stub_http {
    const char *response;
    const char *send_response;
    const char *photo_response;
    const char *document_response;
    const char *delete_response;
    const char *get_file_response;
    sc_str file_response;
    sc_string method;
    sc_string url;
    sc_string body;
    sc_string last_send_body;
    int calls;
    int fail_count;
    int fail_draft_count;
    int fail_edit_count;
} telegram_stub_http;

static void telegram_stub_clear(telegram_stub_http *stub)
{
    if (stub == nullptr) {
        return;
    }
    sc_string_clear(&stub->method);
    sc_string_clear(&stub->url);
    sc_string_clear(&stub->body);
}

static sc_status telegram_stub_request(void *user,
                                       sc_str method,
                                       sc_str url,
                                       sc_str body,
                                       sc_allocator *alloc,
                                       sc_string *out)
{
    telegram_stub_http *stub = user;
    sc_status status;
    if (stub == nullptr || out == nullptr) {
        return sc_status_invalid_argument("telegram.stub.invalid_argument");
    }
    telegram_stub_clear(stub);
    stub->calls += 1;
    status = sc_string_from_str(sc_allocator_heap(), method, &stub->method);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), url, &stub->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), body, &stub->body);
    }
    if (sc_status_is_ok(status) && strstr(url.ptr, "/sendMessage") != nullptr) {
        sc_string_clear(&stub->last_send_body);
        status = sc_string_from_str(sc_allocator_heap(), body, &stub->last_send_body);
    }
    if (sc_status_is_ok(status) && stub->fail_count > 0) {
        stub->fail_count -= 1;
        return sc_status_http("telegram.stub.retryable");
    }
    if (sc_status_is_ok(status) && stub->fail_draft_count > 0 && strstr(url.ptr, "/sendMessageDraft") != nullptr) {
        stub->fail_draft_count -= 1;
        return sc_status_http("telegram.stub.draft_failed");
    }
    if (sc_status_is_ok(status) && stub->fail_edit_count > 0 && strstr(url.ptr, "/editMessageText") != nullptr) {
        stub->fail_edit_count -= 1;
        return sc_status_http("telegram.stub.edit_failed");
    }
    if (sc_status_is_ok(status) && strstr(url.ptr, "/deleteMessage") != nullptr) {
        status = sc_string_from_cstr(alloc,
                                     stub->delete_response == nullptr ? "{\"ok\":true,\"result\":true}" :
                                                                        stub->delete_response,
                                     out);
    } else if (sc_status_is_ok(status) && strstr(url.ptr, "/sendMessage") != nullptr &&
               strstr(url.ptr, "/sendMessageDraft") == nullptr && stub->send_response != nullptr) {
        status = sc_string_from_cstr(alloc, stub->send_response, out);
    } else if (sc_status_is_ok(status) && strstr(url.ptr, "/sendPhoto") != nullptr && stub->photo_response != nullptr) {
        status = sc_string_from_cstr(alloc, stub->photo_response, out);
    } else if (sc_status_is_ok(status) && strstr(url.ptr, "/sendDocument") != nullptr && stub->document_response != nullptr) {
        status = sc_string_from_cstr(alloc, stub->document_response, out);
    } else if (sc_status_is_ok(status) && strstr(url.ptr, "/file/bot") != nullptr) {
        status = sc_string_from_str(alloc, stub->file_response, out);
    } else if (sc_status_is_ok(status) && strstr(url.ptr, "/getFile") != nullptr && stub->get_file_response != nullptr) {
        status = sc_string_from_cstr(alloc, stub->get_file_response, out);
    } else if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, stub->response, out);
    }
    return status;
}
#endif

static int test_telegram_channel_stub(void)
{
    int failures = 0;
#ifdef SC_HAVE_PARSON
    telegram_stub_http stub = {
        .response = "{\"ok\":true,\"result\":[{\"update_id\":42,\"message\":{\"message_id\":5,"
                    "\"message_thread_id\":9,\"chat\":{\"id\":-1001},\"from\":{\"id\":7},"
                    "\"reply_to_message\":{\"message_id\":4},\"text\":\"hello bot\"}}]}",
    };
    sc_channel *telegram = nullptr;
    sc_channel *telegram_ack = nullptr;
    sc_channel *telegram_edit = nullptr;
    sc_channel *telegram_legacy = nullptr;
    sc_channel *telegram_limited = nullptr;
    sc_channel *telegram_mention_private = nullptr;
    sc_channel *telegram_mention_group = nullptr;
    sc_channel *telegram_mention_reply = nullptr;
    sc_channel *telegram_media = nullptr;
    sc_channel *telegram_split = nullptr;
    sc_channel *telegram_approval = nullptr;
    sc_channel *telegram_post_default = nullptr;
    sc_channel *telegram_post_react = nullptr;
    sc_channel_inbound inbound = {0};
    sc_channel_approval_response approval = {0};
    sc_channel_health health = {0};
    sc_string media_dir = {0};
    sc_string media_path = {0};
    const char png_bytes[] = "\x89PNG\r\n\x1A\n";
    const char jpeg_bytes[] = {(char)0xFF, (char)0xD8, (char)0xFF};
    const char pdf_bytes[] = "%PDF";
    const unsigned char wav_bytes[] = {
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
    const unsigned char mp3_bytes[] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0};
    const char *send_response = "{\"ok\":true,\"result\":{\"message_id\":99}}";
    const char *empty_response = "{\"ok\":true,\"result\":[]}";
    char expanding_text[2001] = {0};
    sc_channel_message outbound = {
        .struct_size = sizeof(sc_channel_message),
        .conversation_id = sc_str_from_cstr("-1001"),
        .thread_id = sc_str_from_cstr("9"),
        .sender_id = sc_str_from_cstr("7"),
        .text = sc_str_from_cstr("hello **world**"),
        .reply_to_message_id = sc_str_from_cstr("5"),
    };
    sc_channel_message draft = outbound;
    sc_channel_message draft_update = outbound;
    sc_channel_message audio = outbound;
    sc_channel_message non_wav_audio = outbound;
    sc_channel_message image = outbound;
    sc_channel_message image_document = outbound;
    sc_channel_message document = outbound;
    sc_bytes opus_probe = {0};
    sc_status opus_probe_status = sc_media_wav_to_ogg_opus(sc_allocator_heap(), sc_buf_from_parts(wav_bytes, sizeof(wav_bytes)), 32'768, &opus_probe);
    bool opus_available = opus_probe_status.code != SC_ERR_UNSUPPORTED;
    sc_status_clear(&opus_probe_status);
    sc_bytes_clear(&opus_probe);
    draft.text = sc_str_from_cstr("...");
    draft.draft = true;
    draft_update.text = sc_str_from_cstr("partial reply");
    draft_update.draft = true;
    audio.text = sc_str_from_cstr("");
    audio.attachment_content_type = sc_str_from_cstr("audio/wav");
    audio.attachment_filename = sc_str_from_cstr("smolclaw-reply.wav");
    audio.attachment_bytes = sc_buf_from_parts(wav_bytes, sizeof(wav_bytes));
    non_wav_audio.text = sc_str_from_cstr("");
    non_wav_audio.attachment_content_type = sc_str_from_cstr("audio/mpeg");
    non_wav_audio.attachment_filename = sc_str_from_cstr("smolclaw-reply.mp3");
    non_wav_audio.attachment_bytes = sc_buf_from_parts(mp3_bytes, sizeof(mp3_bytes));
    image.text = sc_str_from_cstr("");
    image.attachment_content_type = sc_str_from_cstr("image/png");
    image.attachment_filename = sc_str_from_cstr("browser-screenshot.png");
    image.attachment_bytes = sc_buf_from_parts(png_bytes, 8);
    image_document = image;
    image_document.attachment_delivery = SC_ATTACHMENT_DELIVERY_DOCUMENT;
    document.text = sc_str_from_cstr("");
    document.attachment_content_type = sc_str_from_cstr("application/pdf");
    document.attachment_filename = sc_str_from_cstr("report.pdf");
    document.attachment_bytes = sc_buf_from_parts(pdf_bytes, sizeof(pdf_bytes) - 1U);

    failures += sc_test_expect_status_key("telegram missing token",
                                  sc_channel_telegram_new(sc_allocator_heap(),
                                                          &(sc_telegram_channel_options){
                                                              .struct_size = sizeof(sc_telegram_channel_options),
                                                              .bot_token = sc_str_from_cstr(""),
                                                              .http_request = telegram_stub_request,
                                                              .http_user = &stub,
                                                          },
                                                          &telegram),
                                  SC_ERR_INVALID_ARGUMENT,
                                  "sc.channel_telegram.token_missing");

    failures += sc_test_expect_status("telegram new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .bot_username = sc_str_from_cstr("smolclaw_bot"),
                                                          .poll_timeout_seconds = 1,
                                                          .max_retries = 1,
                                                          .retry_backoff_ms = 1,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram),
                              SC_OK);
    failures += sc_test_expect_status("telegram health", sc_channel_health_check(telegram, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("telegram healthy", health.healthy && strcmp(health.message.ptr, "telegram configured") == 0);
    sc_channel_health_clear(&health);

    failures += sc_test_expect_status("telegram listen", sc_channel_listen(telegram, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("telegram update id", strcmp(inbound.message_id.ptr, "42") == 0);
    failures += sc_test_expect_true("telegram channel name", strcmp(inbound.channel_name.ptr, "telegram") == 0);
    failures += sc_test_expect_true("telegram chat", strcmp(inbound.conversation_id.ptr, "-1001") == 0);
    failures += sc_test_expect_true("telegram thread", strcmp(inbound.thread_id.ptr, "9") == 0);
    failures += sc_test_expect_true("telegram sender", strcmp(inbound.sender_id.ptr, "7") == 0);
    failures += sc_test_expect_true("telegram text", strcmp(inbound.text.ptr, "hello bot") == 0);
    failures += sc_test_expect_true("telegram memory enabled normally", !inbound.disable_memory);
    failures += sc_test_expect_true("telegram platform account", strcmp(inbound.platform_account_id.ptr, "smolclaw_bot") == 0);
    failures += sc_test_expect_true("telegram reply context", strcmp(inbound.reply_to_message_id.ptr, "4") == 0);
    failures += sc_test_expect_true("telegram get url", strstr(stub.url.ptr, "/bot123:secret/getUpdates?timeout=1") != nullptr);
    sc_channel_inbound_clear(&inbound);

    stub.response = send_response;
    failures += sc_test_expect_status("telegram typing action", sc_channel_send(telegram, &draft), SC_OK);
    failures += sc_test_expect_true("telegram typing method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("telegram typing url", strstr(stub.url.ptr, "/bot123:secret/sendChatAction") != nullptr);
    failures += sc_test_expect_true("telegram typing body",
                            strcmp(stub.body.ptr, "chat_id=-1001&action=typing&message_thread_id=9") == 0);

    stub.response = send_response;
    failures += sc_test_expect_status("telegram edit new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .stream_mode = sc_str_from_cstr("draft"),
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_edit),
                              SC_OK);
    failures += sc_test_expect_status("telegram placeholder draft sends typing", sc_channel_send(telegram_edit, &draft), SC_OK);
    failures += sc_test_expect_true("telegram placeholder draft method", strstr(stub.url.ptr, "/bot123:secret/sendChatAction") != nullptr);
    failures += sc_test_expect_true("telegram placeholder draft body",
                            strcmp(stub.body.ptr, "chat_id=-1001&action=typing&message_thread_id=9") == 0);
    failures += sc_test_expect_status("telegram draft sends message draft", sc_channel_send(telegram_edit, &draft_update), SC_OK);
    failures += sc_test_expect_true("telegram draft method", strstr(stub.url.ptr, "/bot123:secret/sendMessageDraft") != nullptr);
    failures += sc_test_expect_true("telegram draft body", strstr(stub.body.ptr, "draft_id=1") != nullptr);
    stub.calls = 0;
    failures += sc_test_expect_status("telegram final after message draft", sc_channel_send(telegram_edit, &outbound), SC_OK);
    failures += sc_test_expect_true("telegram final sends message", stub.calls == 1 && strstr(stub.url.ptr, "/bot123:secret/sendMessage") != nullptr);

    stub.response = send_response;
    failures += sc_test_expect_status("telegram legacy new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .stream_mode = sc_str_from_cstr("draft"),
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_legacy),
                              SC_OK);
    stub.calls = 0;
    stub.fail_draft_count = 1;
    failures += sc_test_expect_status("telegram draft fallback stores message", sc_channel_send(telegram_legacy, &draft_update), SC_OK);
    failures += sc_test_expect_true("telegram draft fallback attempted", stub.calls == 2);
    failures += sc_test_expect_true("telegram draft fallback sends message", strstr(stub.url.ptr, "/bot123:secret/sendMessage") != nullptr);
    stub.calls = 0;
    stub.fail_edit_count = 1;
    failures += sc_test_expect_status("telegram edit fallback sends new message", sc_channel_send(telegram_legacy, &outbound), SC_OK);
    failures += sc_test_expect_true("telegram edit fallback attempted", stub.calls == 2);
    failures += sc_test_expect_true("telegram edit fallback sends message", strstr(stub.url.ptr, "/bot123:secret/sendMessage") != nullptr);

    stub.response = send_response;
    stub.calls = 0;
    stub.fail_count = 1;
    failures += sc_test_expect_status("telegram retry send", sc_channel_send(telegram, &outbound), SC_OK);
    failures += sc_test_expect_true("telegram retry attempts", stub.calls == 2);

    stub.response = send_response;
    failures += sc_test_expect_status("telegram send", sc_channel_send(telegram, &outbound), SC_OK);
    failures += sc_test_expect_true("telegram send response configured", stub.response == send_response);
    failures += sc_test_expect_true("telegram post method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("telegram send url", strstr(stub.url.ptr, "/bot123:secret/sendMessage") != nullptr);
#ifdef SC_HAVE_LIBCMARK
    failures += sc_test_expect_true("telegram encoded body",
                            strcmp(stub.body.ptr,
                                   "chat_id=-1001&text=hello%20%3Cstrong%3Eworld%3C%2Fstrong%3E&parse_mode=HTML&message_thread_id=9&reply_parameters=%7B%22message_id%22%3A5%7D") ==
                                0);
#else
    failures += sc_test_expect_true("telegram encoded body",
                            strcmp(stub.body.ptr,
                                   "chat_id=-1001&text=hello%20%2A%2Aworld%2A%2A&message_thread_id=9&reply_parameters=%7B%22message_id%22%3A5%7D") == 0);
#endif
    if (opus_available) {
        failures += sc_test_expect_status("telegram voice send", sc_channel_send(telegram, &audio), SC_OK);
        failures += sc_test_expect_true("telegram voice method", strcmp(stub.method.ptr, "POST") == 0);
        failures += sc_test_expect_true("telegram voice url", strstr(stub.url.ptr, "/bot123:secret/sendVoice") != nullptr);
        failures += sc_test_expect_true("telegram voice body", strstr(stub.body.ptr, "voice_filename=smolclaw-reply.ogg") != nullptr &&
                                                      strstr(stub.body.ptr, "voice_content_type=audio%2Fogg") != nullptr &&
                                                      strstr(stub.body.ptr, "voice_bytes=") != nullptr &&
                                                      strstr(stub.body.ptr, "duration=0") != nullptr);
    }
    failures += sc_test_expect_status("telegram non-wav audio send", sc_channel_send(telegram, &non_wav_audio), SC_OK);
    failures += sc_test_expect_true("telegram audio method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("telegram audio url", strstr(stub.url.ptr, "/bot123:secret/sendAudio") != nullptr);
    failures += sc_test_expect_true("telegram audio body", strstr(stub.body.ptr, "audio_filename=smolclaw-reply.mp3") != nullptr &&
                                                  strstr(stub.body.ptr, "audio_content_type=audio%2Fmpeg") != nullptr &&
                                                  strstr(stub.body.ptr, "audio_bytes=10") != nullptr);
    failures += sc_test_expect_status("telegram image send", sc_channel_send(telegram, &image), SC_OK);
    failures += sc_test_expect_true("telegram image method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("telegram image url", strstr(stub.url.ptr, "/bot123:secret/sendPhoto") != nullptr);
    failures += sc_test_expect_true("telegram image body", strstr(stub.body.ptr, "photo_filename=browser-screenshot.png") != nullptr &&
                                                  strstr(stub.body.ptr, "photo_content_type=image%2Fpng") != nullptr &&
                                                  strstr(stub.body.ptr, "photo_bytes=8") != nullptr);
    failures += sc_test_expect_status("telegram image document send", sc_channel_send(telegram, &image_document), SC_OK);
    failures += sc_test_expect_true("telegram image document url", strstr(stub.url.ptr, "/bot123:secret/sendDocument") != nullptr);
    failures += sc_test_expect_true("telegram image document body", strstr(stub.body.ptr, "document_filename=browser-screenshot.png") != nullptr &&
                                                           strstr(stub.body.ptr, "document_content_type=image%2Fpng") != nullptr &&
                                                           strstr(stub.body.ptr, "document_bytes=8") != nullptr);
    failures += sc_test_expect_status("telegram document send", sc_channel_send(telegram, &document), SC_OK);
    failures += sc_test_expect_true("telegram document method", strcmp(stub.method.ptr, "POST") == 0);
    failures += sc_test_expect_true("telegram document url", strstr(stub.url.ptr, "/bot123:secret/sendDocument") != nullptr);
    failures += sc_test_expect_true("telegram document body", strstr(stub.body.ptr, "document_filename=report.pdf") != nullptr &&
                                                     strstr(stub.body.ptr, "document_content_type=application%2Fpdf") != nullptr &&
                                                     strstr(stub.body.ptr, "document_bytes=4") != nullptr);
    stub.photo_response = "{\"ok\":false,\"description\":\"IMAGE_PROCESS_FAILED\"}";
    stub.document_response = send_response;
    stub.calls = 0;
    failures += sc_test_expect_status("telegram photo fallback document", sc_channel_send(telegram, &image), SC_OK);
    failures += sc_test_expect_true("telegram photo fallback attempts", stub.calls == 2);
    failures += sc_test_expect_true("telegram photo fallback url", strstr(stub.url.ptr, "/bot123:secret/sendDocument") != nullptr);
    failures += sc_test_expect_true("telegram photo fallback body", strstr(stub.body.ptr, "document_filename=browser-screenshot.png") != nullptr &&
                                                         strstr(stub.body.ptr, "document_content_type=image%2Fpng") != nullptr &&
                                                         strstr(stub.body.ptr, "document_bytes=8") != nullptr);
    stub.photo_response = nullptr;
    stub.document_response = nullptr;

    stub.response = empty_response;
    failures += sc_test_expect_status("telegram empty poll", sc_channel_listen(telegram, sc_allocator_heap(), &inbound), SC_ERR_TIMEOUT);
    failures += sc_test_expect_true("telegram offset", strstr(stub.url.ptr, "offset=43") != nullptr);

    failures += sc_test_expect_status("telegram media temp dir", sc_test_make_temp_dir("channels", &media_dir), SC_OK);
    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":60,\"message\":{\"message_id\":11,"
                    "\"chat\":{\"id\":7,\"type\":\"private\"},\"from\":{\"id\":7},"
                    "\"text\":\"private hello\"}}]}";
    failures += sc_test_expect_status("telegram mention private new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .bot_username = sc_str_from_cstr("smolclaw_bot"),
                                                          .poll_timeout_seconds = 1,
                                                          .mention_only = true,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_mention_private),
                              SC_OK);
    failures += sc_test_expect_status("telegram mention private listen",
                              sc_channel_listen(telegram_mention_private, sc_allocator_heap(), &inbound),
                              SC_OK);
    failures += sc_test_expect_true("telegram mention private text", strcmp(inbound.text.ptr, "private hello") == 0);
    failures += sc_test_expect_true("telegram mention disables memory", inbound.disable_memory);
    sc_channel_inbound_clear(&inbound);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":61,\"message\":{\"message_id\":12,"
                    "\"chat\":{\"id\":-1001,\"type\":\"supergroup\"},\"from\":{\"id\":7},"
                    "\"text\":\"ambient group chatter\"}}]}";
    failures += sc_test_expect_status("telegram mention group new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .bot_username = sc_str_from_cstr("smolclaw_bot"),
                                                          .poll_timeout_seconds = 1,
                                                          .mention_only = true,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_mention_group),
                              SC_OK);
    failures += sc_test_expect_status("telegram mention group skip",
                              sc_channel_listen(telegram_mention_group, sc_allocator_heap(), &inbound),
                              SC_ERR_TIMEOUT);
    failures += sc_test_expect_true("telegram mention group offset", strstr(stub.url.ptr, "offset=62") != nullptr);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":62,\"message\":{\"message_id\":13,"
                    "\"chat\":{\"id\":-1001,\"type\":\"group\"},\"from\":{\"id\":7},"
                    "\"reply_to_message\":{\"message_id\":12,\"text\":\"@smolclaw_bot summarize this\"},"
                    "\"text\":\"what about this part?\"}}]}";
    failures += sc_test_expect_status("telegram mention reply new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .bot_username = sc_str_from_cstr("smolclaw_bot"),
                                                          .poll_timeout_seconds = 1,
                                                          .mention_only = true,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_mention_reply),
                              SC_OK);
    failures += sc_test_expect_status("telegram mention reply listen",
                              sc_channel_listen(telegram_mention_reply, sc_allocator_heap(), &inbound),
                              SC_OK);
    failures += sc_test_expect_true("telegram mention reply context",
                            strstr(inbound.text.ptr, "Replied message context:\n@smolclaw_bot summarize this") != nullptr &&
                                strstr(inbound.text.ptr, "User message:\nwhat about this part?") != nullptr);
    sc_channel_inbound_clear(&inbound);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":70,\"message\":{\"message_id\":6,"
                    "\"chat\":{\"id\":-1001},\"from\":{\"id\":7},"
                    "\"photo\":[{\"file_id\":\"photo1\",\"file_size\":8}]}}]}";
    stub.get_file_response = "{\"ok\":true,\"result\":{\"file_path\":\"photos/photo1.png\"}}";
    stub.file_response = sc_str_from_parts(png_bytes, 8);
    failures += sc_test_expect_status("telegram media new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .media_temp_dir = sc_string_as_str(&media_dir),
                                                          .poll_timeout_seconds = 1,
                                                          .max_media_bytes = 64,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_media),
                              SC_OK);
    failures += sc_test_expect_status("telegram media listen", sc_channel_listen(telegram_media, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("telegram media text", strcmp(inbound.text.ptr, "[media]") == 0);
    failures += sc_test_expect_true("telegram media type", strcmp(inbound.attachment_media_type.ptr, "image/png") == 0);
    failures += sc_test_expect_true("telegram media path", strstr(inbound.attachment_storage_path.ptr, "sc-media-photo1.bin") != nullptr);
    failures += sc_test_expect_true("telegram media size", inbound.attachment_size_bytes == 8);
    failures += sc_test_expect_true("telegram media file exists", file_exists(inbound.attachment_storage_path.ptr));
    failures += sc_test_expect_true("telegram media download url", strstr(stub.url.ptr, "/file/bot123:secret/photos/photo1.png") != nullptr);
    failures += sc_test_expect_status("telegram media path copy", sc_string_from_str(sc_allocator_heap(), inbound.attachment_storage_path, &media_path), SC_OK);
    sc_channel_inbound_clear(&inbound);
    failures += sc_test_expect_true("telegram media cleanup", !file_exists(media_path.ptr));
    sc_string_clear(&media_path);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":71,\"message\":{\"message_id\":7,"
                    "\"chat\":{\"id\":-1001},\"from\":{\"id\":7},"
                    "\"photo\":[{\"file_id\":\"photo_jpeg\",\"file_size\":3}]}}]}";
    stub.get_file_response = "{\"ok\":true,\"result\":{\"file_path\":\"photos/photo_jpeg.jpg\"}}";
    stub.file_response = sc_str_from_parts(jpeg_bytes, sizeof(jpeg_bytes));
    failures += sc_test_expect_status("telegram jpeg media listen", sc_channel_listen(telegram_media, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("telegram jpeg media type", strcmp(inbound.attachment_media_type.ptr, "image/jpeg") == 0);
    sc_channel_inbound_clear(&inbound);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":90,\"message\":{\"message_id\":10,"
                    "\"chat\":{\"id\":-1001},\"from\":{\"id\":7},"
                    "\"document\":{\"file_id\":\"doc1\",\"file_name\":\"doc1.png\",\"mime_type\":\"image/png\",\"file_size\":4}}}]}";
    stub.get_file_response = "{\"ok\":true,\"result\":{\"file_path\":\"docs/doc1.pdf\"}}";
    stub.file_response = sc_str_from_parts(pdf_bytes, sizeof(pdf_bytes) - 1U);
    failures += sc_test_expect_status("telegram mismatched media listen",
                              sc_channel_listen(telegram_media, sc_allocator_heap(), &inbound),
                              SC_ERR_SECURITY_DENIED);
    sc_channel_inbound_clear(&inbound);
    stub.response = empty_response;
    failures += sc_test_expect_status("telegram mismatched media skipped",
                              sc_channel_listen(telegram_media, sc_allocator_heap(), &inbound),
                              SC_ERR_TIMEOUT);
    failures += sc_test_expect_true("telegram mismatched media offset", strstr(stub.url.ptr, "offset=91") != nullptr);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":80,\"message\":{\"message_id\":8,"
                    "\"chat\":{\"id\":-1001},\"from\":{\"id\":7},\"text\":\"limited one\"}}]}";
    failures += sc_test_expect_status("telegram limited new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .poll_timeout_seconds = 1,
                                                          .rate_limit_per_minute = 1,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_limited),
                              SC_OK);
    failures += sc_test_expect_status("telegram limited first", sc_channel_listen(telegram_limited, sc_allocator_heap(), &inbound), SC_OK);
    sc_channel_inbound_clear(&inbound);
    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":81,\"message\":{\"message_id\":9,"
                    "\"chat\":{\"id\":-1001},\"from\":{\"id\":7},"
                    "\"photo\":[{\"file_id\":\"rate_photo\",\"file_size\":8}]}}]}";
    stub.get_file_response = "{\"ok\":true,\"result\":{\"file_path\":\"photos/rate.png\"}}";
    stub.file_response = sc_str_from_parts(png_bytes, 8);
    failures += sc_test_expect_status("telegram limited second", sc_channel_listen(telegram_limited, sc_allocator_heap(), &inbound), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("telegram rate before download", strstr(stub.url.ptr, "/getUpdates") != nullptr);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":50,\"message\":{\"message_id\":5,"
                    "\"message_thread_id\":9,\"chat\":{\"id\":-1001},\"from\":{\"id\":7},"
                    "\"text\":\"ack me\"}}]}";
    stub.get_file_response = nullptr;
    stub.file_response = sc_str_from_cstr("");
    failures += sc_test_expect_status("telegram ack new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .poll_timeout_seconds = 1,
                                                          .ack_reactions = true,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_ack),
                              SC_OK);
    failures += sc_test_expect_status("telegram reaction listen", sc_channel_listen(telegram_ack, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("telegram reaction inbound", strcmp(inbound.text.ptr, "ack me") == 0);
    failures += sc_test_expect_true("telegram reaction url", strstr(stub.url.ptr, "/bot123:secret/setMessageReaction") != nullptr);
    failures += sc_test_expect_true("telegram reaction body",
                            strcmp(stub.body.ptr,
                                   "chat_id=-1001&message_id=5&reaction=%5B%7B%22type%22%3A%22emoji%22%2C%22emoji%22%3A%22%5CuD83D%5CuDC4D%22%7D%5D") ==
                                0);
    sc_channel_inbound_clear(&inbound);

    telegram_stub_clear(&stub);
    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":44,\"channel_post\":{\"message_id\":7,"
                    "\"chat\":{\"id\":-1002},\"sender_chat\":{\"id\":-1002,\"username\":\"sc_posts\"},"
                    "\"text\":\"post\"}}]}";
    failures += sc_test_expect_status("telegram post default new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .poll_timeout_seconds = 1,
                                                          .ack_reactions = true,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_post_default),
                              SC_OK);
    failures += sc_test_expect_status("telegram post default listen", sc_channel_listen(telegram_post_default, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("telegram post default no reaction", strstr(stub.url.ptr, "/getUpdates") != nullptr);
    failures += sc_test_expect_true("telegram post sender", strcmp(inbound.sender_id.ptr, "sc_posts") == 0);
    sc_channel_inbound_clear(&inbound);

    telegram_stub_clear(&stub);
    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":45,\"channel_post\":{\"message_id\":8,"
                    "\"chat\":{\"id\":-1002},\"sender_chat\":{\"id\":-1002,\"username\":\"sc_posts\"},"
                    "\"text\":\"post\"}}]}";
    failures += sc_test_expect_status("telegram post react new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .poll_timeout_seconds = 1,
                                                          .ack_reactions = true,
                                                          .post_reactions = true,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_post_react),
                              SC_OK);
    failures += sc_test_expect_status("telegram post react listen", sc_channel_listen(telegram_post_react, sc_allocator_heap(), &inbound), SC_OK);
    failures += sc_test_expect_true("telegram post reaction url", strstr(stub.url.ptr, "/bot123:secret/setMessageReaction") != nullptr);
    failures += sc_test_expect_true("telegram post reaction body", strstr(stub.body.ptr, "chat_id=-1002&message_id=8&reaction=") != nullptr);
    sc_channel_inbound_clear(&inbound);

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":43,\"callback_query\":{\"id\":\"cb1\","
                    "\"from\":{\"id\":7},\"message\":{\"message_id\":6,\"message_thread_id\":9,"
                    "\"chat\":{\"id\":-1001}},\"data\":\"sc:approve:1\"}}]}";
    stub.send_response = send_response;
    stub.calls = 0;
    sc_string_clear(&stub.last_send_body);
    failures += sc_test_expect_status("telegram approval new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .poll_timeout_seconds = 1,
                                                          .approval_timeout_seconds = 1,
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_approval),
                              SC_OK);
    failures += sc_test_expect_status("telegram inline approval",
                              sc_channel_request_approval(telegram_approval,
                                                          &(sc_channel_approval_request){
                                                              .struct_size = sizeof(sc_channel_approval_request),
                                                              .conversation_id = sc_str_from_cstr("-1001"),
                                                              .thread_id = sc_str_from_cstr("9"),
                                                              .sender_id = sc_str_from_cstr("7"),
                                                              .summary = sc_str_from_cstr("Approve memory_store?"),
                                                          },
                                                          sc_allocator_heap(),
                                                          &approval),
                              SC_OK);
    failures += sc_test_expect_true("telegram approval decision", approval.decision == SC_CHANNEL_APPROVAL_APPROVED);
    failures += sc_test_expect_true("telegram approval markup", strstr(stub.last_send_body.ptr, "reply_markup=") != nullptr);
    failures += sc_test_expect_true("telegram approval approve button", strstr(stub.last_send_body.ptr, "sc%3Aapprove%3A1") != nullptr);
    failures += sc_test_expect_true("telegram approval deny button", strstr(stub.last_send_body.ptr, "sc%3Adeny%3A1") != nullptr);
    failures += sc_test_expect_true("telegram approval deleted", strstr(stub.url.ptr, "/bot123:secret/deleteMessage") != nullptr);
    failures += sc_test_expect_true("telegram approval delete body", strcmp(stub.body.ptr, "chat_id=-1001&message_id=99") == 0);
    sc_channel_approval_response_clear(&approval);
    stub.send_response = nullptr;

    stub.response = "{\"ok\":true,\"result\":[{\"update_id\":44,\"callback_query\":{\"id\":\"cb2\","
                    "\"from\":{\"id\":7},\"message\":{\"message_id\":7,\"message_thread_id\":9,"
                    "\"chat\":{\"id\":-1001}},\"data\":\"sc:deny:2\"}}]}";
    stub.send_response = send_response;
    failures += sc_test_expect_status("telegram inline denial",
                              sc_channel_request_approval(telegram_approval,
                                                          &(sc_channel_approval_request){
                                                              .struct_size = sizeof(sc_channel_approval_request),
                                                              .conversation_id = sc_str_from_cstr("-1001"),
                                                              .thread_id = sc_str_from_cstr("9"),
                                                              .sender_id = sc_str_from_cstr("7"),
                                                              .summary = sc_str_from_cstr("Approve file_write?"),
                                                          },
                                                          sc_allocator_heap(),
                                                          &approval),
                              SC_OK);
    failures += sc_test_expect_true("telegram denial decision", approval.decision == SC_CHANNEL_APPROVAL_DENIED);
    failures += sc_test_expect_true("telegram denial deleted", strstr(stub.url.ptr, "/bot123:secret/deleteMessage") != nullptr);
    failures += sc_test_expect_true("telegram denial delete body", strcmp(stub.body.ptr, "chat_id=-1001&message_id=99") == 0);
    sc_channel_approval_response_clear(&approval);
    stub.send_response = nullptr;

#ifdef SC_HAVE_LIBCMARK
    (void)memset(expanding_text, '<', sizeof(expanding_text) - 1U);
    stub.response = send_response;
    stub.calls = 0;
    failures += sc_test_expect_status("telegram split new",
                              sc_channel_telegram_new(sc_allocator_heap(),
                                                      &(sc_telegram_channel_options){
                                                          .struct_size = sizeof(sc_telegram_channel_options),
                                                          .bot_token = sc_str_from_cstr("123:secret"),
                                                          .api_base_url = sc_str_from_cstr("https://telegram.test/"),
                                                          .http_request = telegram_stub_request,
                                                          .http_user = &stub,
                                                      },
                                                      &telegram_split),
                              SC_OK);
    outbound.text = sc_str_from_cstr(expanding_text);
    failures += sc_test_expect_status("telegram splits formatted long text", sc_channel_send(telegram_split, &outbound), SC_OK);
    failures += sc_test_expect_true("telegram split calls", stub.calls > 1);
#endif

    telegram_stub_clear(&stub);
    sc_string_clear(&stub.last_send_body);
    sc_channel_destroy(telegram_legacy);
    sc_channel_destroy(telegram_edit);
    sc_channel_destroy(telegram_media);
    sc_channel_destroy(telegram_split);
    sc_channel_destroy(telegram_approval);
    sc_channel_destroy(telegram_post_react);
    sc_channel_destroy(telegram_post_default);
    sc_channel_destroy(telegram_mention_reply);
    sc_channel_destroy(telegram_mention_group);
    sc_channel_destroy(telegram_mention_private);
    sc_channel_destroy(telegram_ack);
    sc_channel_destroy(telegram_limited);
    sc_channel_destroy(telegram);
    (void)rmdir(media_dir.ptr);
    sc_string_clear(&media_dir);
#endif
    return failures;
}

static int test_orchestrator_process_dedupe_allowlist_history(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result result = {0};
    sc_str allowed[] = {sc_str_from_cstr("allowed")};
    sc_channel *channels[1] = {nullptr};

    failures += sc_test_expect_status("agent", make_agent("reply", &provider, &agent), SC_OK);
    failures += sc_test_expect_status("channel", sc_channel_fake_new(sc_allocator_heap(), 0, &channel), SC_OK);
    channels[0] = channel;
    failures += sc_test_expect_status("orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = channels,
                                                              .channel_count = 1,
                                                              .allowed_senders = allowed,
                                                              .allowed_sender_count = 1,
                                                              .max_history_messages = 4,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("m1", "fake", "c1", "thread-a", "allowed", "hello", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("processed", result.processed && strcmp(result.reply.ptr, "reply") == 0);
    failures += sc_test_expect_true("draft and final sent", sc_channel_fake_sent_count(channel) == 2);
    failures += sc_test_expect_true("history scoped", sc_channel_orchestrator_history_len(orchestrator,
                                                                                  sc_str_from_cstr("fake"),
                                                                                  sc_str_from_cstr("allowed"),
                                                                                  sc_str_from_cstr("thread-a")) == 2);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("duplicate",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("m1", "fake", "c1", "thread-a", "allowed", "hello again", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("duplicate skipped", result.duplicate && !result.processed);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("denied",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("m2", "fake", "c1", "thread-a", "blocked", "no", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("allowlist denied", result.denied && !result.processed);

    sc_channel_process_result_clear(&result);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(channel);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_orchestrator_bounded_dedupe(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result result = {0};
    sc_channel *channels[1] = {nullptr};

    failures += sc_test_expect_status("bounded agent", make_agent("reply", &provider, &agent), SC_OK);
    failures += sc_test_expect_status("bounded channel", sc_channel_fake_new(sc_allocator_heap(), 0, &channel), SC_OK);
    channels[0] = channel;
    failures += sc_test_expect_status("bounded orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = channels,
                                                              .channel_count = 1,
                                                              .max_seen_message_ids = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("bounded first",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("m1", "fake", "c", "t", "sender", "first", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("bounded first processed", result.processed);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("bounded second",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("m2", "fake", "c", "t", "sender", "second", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("bounded second processed", result.processed);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("bounded evicted first",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("m1", "fake", "c", "t", "sender", "first again", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("bounded first evicted", result.processed && !result.duplicate);

    sc_channel_process_result_clear(&result);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(channel);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_provider_route_selection_per_channel(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result result = {0};
    sc_channel *channels[1] = {nullptr};
    sc_channel_provider_route routes[] = {
        {
            .struct_size = sizeof(sc_channel_provider_route),
            .channel_name = sc_str_from_cstr("mqtt"),
            .provider_name = sc_str_from_cstr("route-provider"),
            .model = sc_str_from_cstr("mqtt-model"),
        },
    };

    failures += sc_test_expect_status("route provider", sc_provider_new(sc_allocator_heap(), &route_provider_vtab, nullptr, &provider), SC_OK);
    failures += sc_test_expect_status("route agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .model = sc_str_from_cstr("default-model"),
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("route channel", sc_channel_fake_new(sc_allocator_heap(), 0, &channel), SC_OK);
    channels[0] = channel;
    failures += sc_test_expect_status("route orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = channels,
                                                              .channel_count = 1,
                                                              .provider_routes = routes,
                                                              .provider_route_count = SC_ARRAY_LEN(routes),
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("route process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("route-1", "mqtt", "c", "t", "sender", "hello", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("route output", result.processed && strcmp(result.reply.ptr, "route:mqtt-model") == 0);

    sc_channel_process_result_clear(&result);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(channel);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_streaming_approval_cancellation_poll_and_delivery(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *draft_channel = nullptr;
    sc_channel *final_channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result result = {0};
    sc_channel_approval_response approval = {0};
    sc_delivery_target *delivery = nullptr;
    sc_channel *channels[1] = {nullptr};

    failures += sc_test_expect_status("agent", make_agent("stream-reply", &provider, &agent), SC_OK);
    failures += sc_test_expect_status("draft channel", sc_channel_fake_new(sc_allocator_heap(), SC_CHANNEL_CAP_DRAFT_UPDATES | SC_CHANNEL_CAP_APPROVAL_PROMPTS, &draft_channel), SC_OK);
    failures += sc_test_expect_status("final channel", sc_channel_fake_new(sc_allocator_heap(), SC_CHANNEL_CAP_APPROVAL_PROMPTS, &final_channel), SC_OK);
    channels[0] = draft_channel;
    failures += sc_test_expect_status("orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = channels,
                                                              .channel_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);

    failures += sc_test_expect_status("draft process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              draft_channel,
                                                              INBOUND("m10", "fake", "c1", "thread", "sender", "hello", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("draft supported sends two", sc_channel_fake_sent_count(draft_channel) == 2);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("final process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              final_channel,
                                                              INBOUND("m11", "fake", "c2", "thread", "sender", "hello", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("fallback sends final only", sc_channel_fake_sent_count(final_channel) == 1);
    sc_channel_process_result_clear(&result);

    sc_channel_fake_set_approval(draft_channel, SC_CHANNEL_APPROVAL_ALWAYS);
    failures += sc_test_expect_status("approval",
                              sc_channel_orchestrator_request_approval(orchestrator,
                                                                       draft_channel,
                                                                       INBOUND("m12", "fake", "c1", "thread", "sender", "approve", false),
                                                                       sc_str_from_cstr("approve action"),
                                                                       sc_allocator_heap(),
                                                                       &approval),
                              SC_OK);
    failures += sc_test_expect_true("approval always", approval.decision == SC_CHANNEL_APPROVAL_ALWAYS);
    sc_channel_approval_response_clear(&approval);

    failures += sc_test_expect_status("first history",
                              sc_channel_orchestrator_process(orchestrator,
                                                              draft_channel,
                                                              INBOUND("m13", "fake", "c1", "cancel-thread", "sender", "first", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    sc_channel_process_result_clear(&result);
    failures += sc_test_expect_status("cancel previous",
                              sc_channel_orchestrator_process(orchestrator,
                                                              draft_channel,
                                                              INBOUND("m14", "fake", "c1", "cancel-thread", "sender", "second", true),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_OK);
    failures += sc_test_expect_true("cancel marker", result.cancelled_previous);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("push inbound",
                              sc_channel_fake_push_inbound(draft_channel,
                                                           INBOUND("m15", "fake", "c3", "poll-thread", "sender", "poll", false)),
                              SC_OK);
    failures += sc_test_expect_status("poll",
                              sc_channel_orchestrator_poll(orchestrator, draft_channel, sc_allocator_heap(), &result),
                              SC_OK);
    failures += sc_test_expect_true("poll processed", result.processed);
    sc_channel_process_result_clear(&result);

    failures += sc_test_expect_status("delivery target", sc_channel_orchestrator_delivery_new(sc_allocator_heap(), orchestrator, &delivery), SC_OK);
    failures += sc_test_expect_status("delivery send",
                              sc_delivery_deliver(delivery,
                                                  &(sc_delivery_message){
                                                      .struct_size = sizeof(sc_delivery_message),
                                                      .kind = SC_DELIVERY_CHANNEL,
                                                      .target = sc_str_from_cstr("c-delivery"),
                                                      .content = sc_str_from_cstr("delivery message"),
                                                  }),
                              SC_OK);
    failures += sc_test_expect_true("delivery through channel", strcmp(sc_channel_fake_last_sent_text(draft_channel).ptr, "delivery message") == 0);

    sc_delivery_target_destroy(delivery);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(final_channel);
    sc_channel_destroy(draft_channel);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_runtime_failure_response(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result result = {0};
    sc_channel *channels[1] = {nullptr};

    failures += sc_test_expect_status("fail provider", sc_provider_new(sc_allocator_heap(), &fail_provider_vtab, nullptr, &provider), SC_OK);
    failures += sc_test_expect_status("fail agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("fail channel", sc_channel_fake_new(sc_allocator_heap(), 0, &channel), SC_OK);
    channels[0] = channel;
    failures += sc_test_expect_status("fail orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = channels,
                                                              .channel_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("runtime failure process",
                              sc_channel_orchestrator_process(orchestrator,
                                                              channel,
                                                              INBOUND("fail-1", "fake", "c", "t", "sender", "hello", false),
                                                              sc_allocator_heap(),
                                                              &result),
                              SC_ERR_IO);
    failures += sc_test_expect_true("runtime failure public response",
                            strcmp(sc_channel_fake_last_sent_text(channel).ptr,
                                   "Sorry, I couldn't process that request.") == 0);

    sc_channel_process_result_clear(&result);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(channel);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static sc_status make_agent(const char *text, sc_provider **provider, sc_agent **agent)
{
    sc_status status = sc_provider_mock_new(sc_allocator_heap(),
                                            SC_PROVIDER_MOCK_TEXT,
                                            sc_str_from_cstr(text),
                                            provider);
    if (sc_status_is_ok(status)) {
        status = sc_agent_new(sc_allocator_heap(),
                              &(sc_agent_options){
                                  .struct_size = sizeof(sc_agent_options),
                                  .provider = *provider,
                              },
                              agent);
    }
    return status;
}

static sc_status route_provider_generate(void *impl,
                                         const sc_provider_request *request,
                                         sc_allocator *alloc,
                                         sc_provider_response *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    (void)impl;
    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test.route_provider.invalid_argument");
    }
    *out = (sc_provider_response){.struct_size = sizeof(*out)};
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "route:");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, request->model);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &out->text);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static void route_provider_destroy(void *impl)
{
    (void)impl;
}

static sc_status fail_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out)
{
    (void)impl;
    (void)request;
    (void)alloc;
    (void)out;
    return sc_status_io("sc.test.provider.failed");
}

static void fail_provider_destroy(void *impl)
{
    (void)impl;
}

static int file_exists(const char *path)
{
    struct stat st = {0};
    return path != nullptr && stat(path, &st) == 0;
}
