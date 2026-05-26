#include "sc/autonomy.h"
#include "sc/channel.h"
#include "sc/config.h"
#include "sc/gateway.h"
#include "sc/hardware.h"
#include "sc/i18n.h"
#include "sc/media.h"
#include "sc/memory.h"
#include "sc/observer.h"
#include "sc/peripheral.h"
#include "sc/plugin.h"
#include "sc/provider.h"
#include "sc/runtime.h"
#include "sc/sandbox.h"
#include "sc/toml.h"
#include "sc/tool.h"
#include "sc/url.h"
#include "test_helpers.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int test_abi_layouts(void);
static int test_parser_fuzz_smoke(void);
static int test_gateway_parser_smoke(void);
static int test_generated_docs_and_l10n_coverage(void);
static int test_mvp_runtime_smoke(void);
static void clear_status(sc_status status);

static sc_serial_transport *volatile validation_serial_seed;

int main(void)
{
    int failures = 0;

    failures += test_abi_layouts();
    failures += test_parser_fuzz_smoke();
    failures += test_gateway_parser_smoke();
    failures += test_generated_docs_and_l10n_coverage();
    failures += test_mvp_runtime_smoke();

    return failures == 0 ? 0 : 1;
}

static int test_abi_layouts(void)
{
    int failures = 0;

    failures += sc_test_expect_true("provider vtab struct_size first", offsetof(sc_provider_vtab, struct_size) == 0);
    failures += sc_test_expect_true("channel vtab struct_size first", offsetof(sc_channel_vtab, struct_size) == 0);
    failures += sc_test_expect_true("tool vtab struct_size first", offsetof(sc_tool_vtab, struct_size) == 0);
    failures += sc_test_expect_true("memory vtab struct_size first", offsetof(sc_memory_vtab, struct_size) == 0);
    failures += sc_test_expect_true("observer vtab struct_size first", offsetof(sc_observer_vtab, struct_size) == 0);
    failures += sc_test_expect_true("runtime vtab struct_size first", offsetof(sc_runtime_vtab, struct_size) == 0);
    failures += sc_test_expect_true("peripheral vtab struct_size first", offsetof(sc_peripheral_vtab, struct_size) == 0);
    failures += sc_test_expect_true("sandbox vtab struct_size first", offsetof(sc_sandbox_vtab, struct_size) == 0);
    failures += sc_test_expect_true("plugin descriptor struct_size first", offsetof(sc_plugin_descriptor, struct_size) == 0);
    failures += sc_test_expect_true("serial vtab struct_size first", offsetof(sc_serial_transport_vtab, struct_size) == 0);

    failures += sc_test_expect_true("provider ABI slot", offsetof(sc_provider_vtab, abi_major) == sizeof(size_t));
    failures += sc_test_expect_true("plugin descriptor has init", offsetof(sc_plugin_descriptor, init) > offsetof(sc_plugin_descriptor, version));
    failures += sc_test_expect_true("plugin descriptor append-only",
                            offsetof(sc_plugin_descriptor, requested_capabilities) > offsetof(sc_plugin_descriptor, metadata_json) &&
                                offsetof(sc_plugin_descriptor, release_memory) > offsetof(sc_plugin_descriptor, manifest_schema_ref));
    failures += sc_test_expect_true("plugin host API versioned", offsetof(sc_plugin_host_api, abi_major) > offsetof(sc_plugin_host_api, userdata));
    failures += sc_test_expect_true("plugin host API append-only",
                            offsetof(sc_plugin_host_api, register_observer) > offsetof(sc_plugin_host_api, register_memory) &&
                                offsetof(sc_plugin_host_api, register_peripheral) > offsetof(sc_plugin_host_api, register_observer));
    failures += sc_test_expect_true("public ABI major pre-1", SC_ABI_VERSION_MAJOR == 0);
    failures += sc_test_expect_true("config schema version struct", sizeof(sc_config) > sizeof(size_t));
    failures += sc_test_expect_true("media attachment struct", offsetof(sc_media_attachment, struct_size) == 0);

    return failures;
}

static int test_parser_fuzz_smoke(void)
{
    int failures = 0;
    const char *json_inputs[] = {"", "{", "{\"a\":1}", "[true,false,null]", "{\"x\":[{\"y\":\"z\"}]}", "{\"n\":1e9999}"};
    const char *toml_inputs[] = {"", "schema.version = 1\n", "[provider]\ndefault = \"mock\"\n", "bad", "x = \"unterminated"};
    const char *url_inputs[] = {"", "http://example.com/", "https://user:pass@example.com/p?q#f", "http://127.0.0.1:8080/", "://bad", "http://:999999/"};
    const char *sse_inputs[] = {"data: [DONE]\n\n",
                                "data: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\n",
                                "event: message\ndata: {\"error\":{\"message\":\"no\"}}\n\n",
                                "data: {\n\n"};
    const char *sop_inputs[] = {"# SOP\n\n- manual: approve\n", "# SOP\n\n- tool: file_read {}\n", "", "not a sop"};
    const char *provider_inputs[] = {"{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}",
                                     "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"1\",\"function\":{\"name\":\"file_read\",\"arguments\":\"{}\"}}]}}]}",
                                     "{\"error\":{\"message\":\"bad\"}}",
                                     "{"};

    for (size_t i = 0; i < SC_ARRAY_LEN(json_inputs); i += 1) {
        sc_json_value *value = nullptr;
        sc_json_parse_error error = {0};
        sc_status status = sc_json_parse(sc_allocator_heap(), sc_str_from_cstr(json_inputs[i]), &value, &error);
        sc_json_destroy(value);
        clear_status(status);
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(toml_inputs); i += 1) {
        sc_toml_source source = {0};
        sc_toml_diag diag = {0};
        sc_status status = sc_toml_parse_source(sc_allocator_heap(),
                                                sc_str_from_cstr("fuzz.toml"),
                                                sc_str_from_cstr(toml_inputs[i]),
                                                &source,
                                                &diag);
        sc_toml_source_clear(&source);
        sc_toml_diag_clear(&diag);
        clear_status(status);
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(url_inputs); i += 1) {
        sc_url url = {0};
        sc_status status = sc_url_parse(sc_allocator_heap(), sc_str_from_cstr(url_inputs[i]), &url);
        if (sc_status_is_ok(status)) {
            (void)sc_url_has_credentials(&url);
            (void)sc_url_host_is_private_address(&url);
        }
        sc_url_clear(&url);
        clear_status(status);
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(sse_inputs); i += 1) {
        sc_provider_stream_event event = {0};
        sc_status status = sc_provider_openai_parse_sse_event(sc_allocator_heap(),
                                                              sc_str_from_cstr(sse_inputs[i]),
                                                              &event);
        sc_provider_stream_event_clear(&event);
        clear_status(status);
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(provider_inputs); i += 1) {
        sc_provider_response response = {0};
        sc_status status = sc_provider_openai_parse_response(sc_allocator_heap(),
                                                             sc_str_from_cstr(provider_inputs[i]),
                                                             &response);
        sc_provider_response_clear(&response);
        clear_status(status);
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(sop_inputs); i += 1) {
        sc_sop_document document = {0};
        sc_status status = sc_sop_markdown_parse(sc_allocator_heap(), sc_str_from_cstr(sop_inputs[i]), &document);
        sc_sop_document_clear(&document);
        clear_status(status);
    }

    sc_serial_transport *serial = validation_serial_seed;
    sc_status serial_status = sc_hardware_fake_serial_new(sc_allocator_heap(),
                                                          &(sc_hardware_fake_serial_options){
                                                              .struct_size = sizeof(sc_hardware_fake_serial_options),
                                                              .capabilities = SC_HARDWARE_CAP_GPIO_READ | SC_HARDWARE_CAP_GPIO_WRITE,
                                                          },
                                                          &serial);
    bool serial_ready = sc_status_is_ok(serial_status);
    failures += sc_test_expect_status("fake serial", serial_status, SC_OK);
    if (serial_ready) {
        sc_string pong = {0};
        bool value = false;
        clear_status(sc_hardware_protocol_ping(serial, sc_allocator_heap(), &pong));
        clear_status(sc_hardware_protocol_gpio_read(serial, 1, &value));
        clear_status(sc_hardware_protocol_gpio_write(serial, 1, true));
        sc_string_clear(&pong);
        sc_serial_transport_destroy(serial);
    }

    return failures;
}

static int test_gateway_parser_smoke(void)
{
    int failures = 0;
    sc_gateway_server *server = nullptr;
    sc_gateway_options options = {
        .struct_size = sizeof(options),
        .bind = sc_str_from_cstr("127.0.0.1"),
        .pairing_code = sc_str_from_cstr("pair"),
        .auth_token = sc_str_from_cstr("token"),
        .max_body_bytes = 32,
        .rate_limit = 16,
    };
    sc_gateway_request requests[] = {
        {.struct_size = sizeof(sc_gateway_request), .method = SC_GATEWAY_GET, .path = sc_str_from_cstr("/status")},
        {.struct_size = sizeof(sc_gateway_request), .method = SC_GATEWAY_GET, .path = sc_str_from_cstr("/health")},
        {.struct_size = sizeof(sc_gateway_request), .method = SC_GATEWAY_GET, .path = sc_str_from_cstr("/sse/events"), .auth_token = sc_str_from_cstr("token")},
        {.struct_size = sizeof(sc_gateway_request), .method = SC_GATEWAY_POST, .path = sc_str_from_cstr("/config/set"), .auth_token = sc_str_from_cstr("token"), .idempotency_key = sc_str_from_cstr("k"), .body = sc_str_from_cstr("bad")},
        {.struct_size = sizeof(sc_gateway_request), .method = SC_GATEWAY_POST, .path = sc_str_from_cstr("/webhook/unknown"), .body = sc_str_from_cstr("{")},
    };

    failures += sc_test_expect_status("gateway new", sc_gateway_server_new(sc_allocator_heap(), &options, &server), SC_OK);
    failures += sc_test_expect_status("gateway start", sc_gateway_server_start(server), SC_OK);
    for (size_t i = 0; i < SC_ARRAY_LEN(requests); i += 1) {
        sc_gateway_response response = {0};
        sc_status status = sc_gateway_handle_request(server, &requests[i], sc_allocator_heap(), &response);
        sc_gateway_response_clear(&response);
        clear_status(status);
    }
    sc_gateway_server_destroy(server);
    return failures;
}

static int test_generated_docs_and_l10n_coverage(void)
{
    int failures = 0;
    sc_i18n_catalog catalog = {0};
    sc_string report = {0};
    sc_str required_keys[] = {
        sc_str_from_cstr("cli.usage"),
        sc_str_from_cstr("cli.version"),
        sc_str_from_cstr("cli.onboard.summary"),
        sc_str_from_cstr("cli.cron.summary"),
        sc_str_from_cstr("tool.file_read.description"),
        sc_str_from_cstr("tool.file_write.description"),
        sc_str_from_cstr("tool.file_list.description"),
        sc_str_from_cstr("tool.http.description"),
        sc_str_from_cstr("tool.web_search.description"),
        sc_str_from_cstr("tool.browser.description"),
        sc_str_from_cstr("tool.browser_screenshot.description"),
        sc_str_from_cstr("tool.pdf_extract.description"),
        sc_str_from_cstr("tool.time.description"),
        sc_str_from_cstr("tool.content_search.description"),
        sc_str_from_cstr("tool.glob_search.description"),
        sc_str_from_cstr("tool.mcp_server.description"),
        sc_str_from_cstr("tool.sop_inspect.description"),
        sc_str_from_cstr("tool.sop_advance.description"),
        sc_str_from_cstr("tool.cron_list.description"),
        sc_str_from_cstr("tool.cron_upsert.description"),
        sc_str_from_cstr("tool.cron_remove.description"),
        sc_str_from_cstr("tool.resource_usage.description"),
        sc_str_from_cstr("tool.hardware.gpio_read.description"),
        sc_str_from_cstr("gateway.health.ok"),
        sc_str_from_cstr("onboarding.welcome"),
    };

    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    failures += sc_test_expect_status("default catalog", sc_i18n_catalog_load_default_en(&catalog), SC_OK);
    failures += sc_test_expect_status("coverage report",
                              sc_i18n_coverage_report(&catalog,
                                                      required_keys,
                                                      SC_ARRAY_LEN(required_keys),
                                                      sc_allocator_heap(),
                                                      &report),
                              SC_OK);
    failures += sc_test_expect_true("no missing l10n keys", strstr(report.ptr, "\"missing_count\":0") != nullptr);
    failures += sc_test_expect_true("config schema generated", sc_config_field_count > 0);
    failures += sc_test_expect_true("config schema version", SC_CONFIG_SCHEMA_VERSION_CURRENT >= 1u);
    failures += sc_test_expect_true("schema has agent limit",
                            sc_config_field_lookup(sc_str_from_cstr("agent.max_tool_iterations")) != nullptr);
    failures += sc_test_expect_true("schema has consecutive message limit",
                            sc_config_field_lookup(sc_str_from_cstr("agent.consecutive_message_limit")) != nullptr);
    failures += sc_test_expect_true("schema has heartbeat",
                            sc_config_field_lookup(sc_str_from_cstr("heartbeat.state_path")) != nullptr);
    failures += sc_test_expect_true("schema has daemon logging",
                            sc_config_field_lookup(sc_str_from_cstr("logging.daemon.file_path")) != nullptr);
    failures += sc_test_expect_true("schema has telegram runtime knobs",
                            sc_config_field_lookup(sc_str_from_cstr("channels.telegram.poll_timeout_seconds")) != nullptr &&
                                sc_config_field_lookup(sc_str_from_cstr("channels.telegram.message_split_bytes")) != nullptr);
    failures += sc_test_expect_true("schema has browser screenshot limit",
                            sc_config_field_lookup(sc_str_from_cstr("browser.screenshot.max_bytes")) != nullptr);
    failures += sc_test_expect_true("schema has rtk tools",
                            sc_config_field_lookup(sc_str_from_cstr("tools.rtk.enabled")) != nullptr &&
                                sc_config_field_lookup(sc_str_from_cstr("tools.rtk.command")) != nullptr &&
                                sc_config_field_lookup(sc_str_from_cstr("tools.rtk.allowed_commands")) != nullptr);
    {
        sc_config config = {0};
        sc_config_diag diag = {0};
        sc_config_load_options options = {
            .explicit_file = {
                .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
                .source_path = sc_str_from_cstr("invalid-range.toml"),
                .body = sc_str_from_cstr("[agent]\nmax_tool_iterations = 0\n"),
                .present = true,
            },
        };
        sc_status invalid_status = sc_config_load(sc_allocator_heap(), &options, &config, &diag);
        failures += sc_test_expect_true("config invalid range status", invalid_status.code == SC_ERR_PARSE);
        failures += sc_test_expect_true("config invalid range key",
                                invalid_status.error_key != nullptr &&
                                    strcmp(invalid_status.error_key, "sc.config.invalid_range") == 0);
        failures += sc_test_expect_true("config invalid range path",
                                sc_str_equal(sc_string_as_str(&diag.path), sc_str_from_cstr("agent.max_tool_iterations")));
        sc_status_clear(&invalid_status);
        sc_config_diag_clear(&diag);
        sc_config_clear(&config);
    }

    sc_string_clear(&report);
    sc_i18n_catalog_clear(&catalog);
    return failures;
}

static int test_mvp_runtime_smoke(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_memory *memory = nullptr;
    sc_gateway_server *gateway = nullptr;
    sc_channel *channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result channel_result = {0};
    sc_gateway_response gateway_response = {0};
    sc_agent_turn_result turn_result = {0};
    sc_memory_record record = {
        .struct_size = sizeof(record),
        .namespace_name = sc_str_from_cstr("validation"),
        .key = sc_str_from_cstr("k"),
        .value = sc_str_from_cstr("v"),
    };
    sc_string recalled = {0};

    failures += sc_test_expect_status("mvp provider",
                              sc_provider_mock_new(sc_allocator_heap(), SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("mvp-output"), &provider),
                              SC_OK);
    failures += sc_test_expect_status("mvp memory", sc_memory_sqlite_open(sc_allocator_heap(), sc_str_from_cstr("/tmp/sc-validation-memory.sqlite"), &memory), SC_OK);
    failures += sc_test_expect_status("mvp memory put", sc_memory_put(memory, &record), SC_OK);
    sc_memory_destroy(memory);
    memory = nullptr;
    failures += sc_test_expect_status("mvp memory reopen", sc_memory_sqlite_open(sc_allocator_heap(), sc_str_from_cstr("/tmp/sc-validation-memory.sqlite"), &memory), SC_OK);
    failures += sc_test_expect_status("mvp memory recall", sc_memory_get(memory, sc_str_from_cstr("validation"), sc_str_from_cstr("k"), sc_allocator_heap(), &recalled), SC_OK);
    failures += sc_test_expect_true("mvp memory value", strcmp(recalled.ptr, "v") == 0);
    failures += sc_test_expect_status("mvp agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .memory = memory,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("mvp chat",
                              sc_agent_process_message(agent,
                                                       &(sc_turn){.struct_size = sizeof(sc_turn), .input = sc_str_from_cstr("hello")},
                                                       sc_allocator_heap(),
                                                       &turn_result),
                              SC_OK);
    failures += sc_test_expect_true("mvp chat output", strcmp(turn_result.output.ptr, "mvp-output") == 0);
    failures += sc_test_expect_status("mvp gateway",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .agent = agent,
                                                        .auth_token = sc_str_from_cstr("token"),
                                                        .rate_limit = 32,
                                                    },
                                                    &gateway),
                              SC_OK);
    failures += sc_test_expect_status("mvp gateway start", sc_gateway_server_start(gateway), SC_OK);
    failures += sc_test_expect_status("mvp ws chat",
                              sc_gateway_handle_request(gateway,
                                                        &(sc_gateway_request){
                                                            .struct_size = sizeof(sc_gateway_request),
                                                            .method = SC_GATEWAY_POST,
                                                            .path = sc_str_from_cstr("/ws/chat"),
                                                            .body = sc_str_from_cstr("hello"),
                                                            .auth_token = sc_str_from_cstr("token"),
                                                            .request_id = sc_str_from_cstr("validation-ws"),
                                                            .session_id = sc_str_from_cstr("validation"),
                                                        },
                                                        sc_allocator_heap(),
                                                        &gateway_response),
                              SC_OK);
    failures += sc_test_expect_true("mvp ws output", gateway_response.status == 200 && strcmp(gateway_response.body.ptr, "mvp-output") == 0);
    failures += sc_test_expect_status("mvp fake channel", sc_channel_fake_new(sc_allocator_heap(), SC_CHANNEL_CAP_MULTI_MESSAGE_STREAM, &channel), SC_OK);
    failures += sc_test_expect_status("mvp orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = &channel,
                                                              .channel_count = 1,
                                                          },
                                                          &orchestrator),
                              SC_OK);
    failures += sc_test_expect_status("mvp fake inbound",
                              sc_channel_fake_push_inbound(channel,
                                                           &(sc_channel_inbound){
                                                               .struct_size = sizeof(sc_channel_inbound),
                                                               .message_id = sc_str_from_cstr("m1"),
                                                               .channel_name = sc_str_from_cstr("fake"),
                                                               .conversation_id = sc_str_from_cstr("c"),
                                                               .thread_id = sc_str_from_cstr("t"),
                                                               .sender_id = sc_str_from_cstr("u"),
                                                               .text = sc_str_from_cstr("hello"),
                                                           }),
                              SC_OK);
    failures += sc_test_expect_status("mvp channel poll", sc_channel_orchestrator_poll(orchestrator, channel, sc_allocator_heap(), &channel_result), SC_OK);
    failures += sc_test_expect_true("mvp channel processed", channel_result.processed && strcmp(channel_result.reply.ptr, "mvp-output") == 0);

    sc_channel_process_result_clear(&channel_result);
    sc_gateway_response_clear(&gateway_response);
    sc_agent_turn_result_clear(&turn_result);
    sc_string_clear(&recalled);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(channel);
    sc_gateway_server_destroy(gateway);
    sc_agent_destroy(agent);
    sc_memory_destroy(memory);
    sc_provider_destroy(provider);
    return failures;
}

static void clear_status(sc_status status)
{
    sc_status_clear(&status);
}
