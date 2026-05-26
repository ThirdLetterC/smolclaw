#include "arena/arena.h"
#include "clags/clags.h"
#include "hiredis/read.h"
#include "mqtt/mqtt.h"
#include "nanocron/nanocron.h"
#include "psimd.h"
#include "rabbitmq/amqp.h"
#include "toml/toml.h"
#include "ulog/ulog.h"
#include "url.h"
#include "websocket-server/websocket.h"
#include "test_helpers.h"

#ifdef SC_HAVE_MICROJSON
#include "mjson_write.h"
#endif

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
#include "websocket_client/websocket_client.h"
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int smoke_websocket_protocol(void);

typedef struct json_value_t JSON_Value;
JSON_Value *json_parse_string(const char *string);
void json_value_free(JSON_Value *value);

int main(void)
{
    int failures = 0;

    Arena *arena = arena_create(128);
    failures += sc_test_expect_true("arena_create", arena != nullptr);
    if (arena != nullptr) {
        failures += sc_test_expect_true("arena_alloc", arena_alloc(arena, 16) != nullptr);
        arena_destroy(arena);
    }

    JSON_Value *json = json_parse_string("{\"ok\":true}");
    failures += sc_test_expect_true("parson parse", json != nullptr);
    json_value_free(json);

    const char toml_text[] = "ok = true\n";
    toml_result_t toml = toml_parse(toml_text, (int)strlen(toml_text));
    failures += sc_test_expect_true("toml parse", toml.ok);
    toml_free(toml);

    cron_ctx_t *cron = cron_create();
    failures += sc_test_expect_true("nanocron create", cron != nullptr);
    cron_destroy(cron);

    redisReader *reader = redisReaderCreateWithFunctions(nullptr);
    failures += sc_test_expect_true("hiredis reader", reader != nullptr);
    redisReaderFree(reader);

    failures += sc_test_expect_true("mqtt error string", mqtt_error_str(MQTT_OK) != nullptr);
    failures += sc_test_expect_true("rabbitmq version", amqp_version() != nullptr);
    failures += sc_test_expect_true("clags error string", clags_error_description(Clags_Error_Ok) != nullptr);
    failures += sc_test_expect_true("ulog level string", ulog_level_to_string(ULOG_LEVEL_INFO) != nullptr);
    failures += sc_test_expect_true("psimd header", sizeof(psimd_f32x4) == 16U);

#ifdef SC_HAVE_MICROJSON
    mjson_writer measure = {0};
    mjson_writer writer = {0};
    char microjson_buffer[64] = {0};
    mjson_writer_init(&measure, nullptr, 0);
    failures += sc_test_expect_true("microjson measure",
                            mjson_write_string(&measure, "quoted \"line\"\n", 14) == MJSON_WRITE_OK &&
                                mjson_writer_length(&measure) == 19U);
    mjson_writer_init(&writer, microjson_buffer, sizeof(microjson_buffer));
    failures += sc_test_expect_true("microjson write",
                            mjson_write_string(&writer, "quoted \"line\"\n", 14) == MJSON_WRITE_OK &&
                                !mjson_writer_truncated(&writer) &&
                                strcmp(microjson_buffer, "\"quoted \\\"line\\\"\\n\"") == 0);
#endif

    char url_text[] = "https://example.com/path";
    URL parsed = {0};
    failures += sc_test_expect_true("url parse",
                            url_parse(url_text, (int)strlen(url_text), nullptr, &parsed, URL_FLAG_RFC3986) >= 0);

    failures += smoke_websocket_protocol();

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
    ws_client_t *client = ws_client_create();
    failures += sc_test_expect_true("websocket client create", client != nullptr);
    ws_client_destroy(client);
#endif

    return failures == 0 ? 0 : 1;
}

static void smoke_ws_send(ws_transport_t *self, const uint8_t *data, size_t len)
{
    (void)self;
    (void)data;
    (void)len;
}

static void smoke_ws_close(ws_transport_t *self)
{
    (void)self;
}

static int smoke_websocket_protocol(void)
{
    ws_transport_t transport = {
        .send_raw = smoke_ws_send,
        .close = smoke_ws_close,
    };
    ws_callbacks_t callbacks = {0};
    ws_conn_t *conn = ws_conn_new(transport, callbacks, nullptr);
    int failures = sc_test_expect_true("websocket protocol create", conn != nullptr);
    ws_conn_free(conn);
    return failures;
}
