#include "sc/cli.h"
#include "sc/json.h"
#include "sc/log.h"
#include "sc/toml.h"
#include "sc/url.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int test_json(void);
static int test_toml(void);
static int test_cli(void);
static int test_log(void);
static int test_url(void);

int main(void)
{
    int failures = 0;

    failures += test_json();
    failures += test_toml();
    failures += test_cli();
    failures += test_log();
    failures += test_url();

    return failures == 0 ? 0 : 1;
}

static int test_json(void)
{
    int failures = 0;
    char deep_input[160] = {0};
    sc_json_value *root = nullptr;
    sc_json_value *payload = nullptr;
    sc_json_value *schema = nullptr;
    sc_json_value *clone = nullptr;
    sc_json_value *huge_array = nullptr;
    sc_json_value *huge_item = nullptr;
    sc_string text = {0};
    sc_json_parse_error error = {0};
    sc_str str = {0};
    sc_test_allocator fail_alloc = {0};
    double number = 0.0;
    size_t pos = 0;

    failures += sc_test_expect_status(
        "json parse duplicate",
        sc_json_parse(sc_allocator_heap(),
                      sc_str_from_cstr("{\"a\":1,\"a\":2,\"b\":null,\"arr\":[true,\"x\"]}"),
                      &root,
                      &error),
        SC_OK
    );
    failures += sc_test_expect_true("json duplicate last wins",
                            sc_json_as_number(sc_json_object_get(root, sc_str_from_cstr("a")), &number) &&
                                number == 2.0);
    failures += sc_test_expect_true("json null vs missing",
                            sc_json_is_null(sc_json_object_get(root, sc_str_from_cstr("b"))) &&
                                sc_json_object_get(root, sc_str_from_cstr("missing")) == nullptr);
    failures += sc_test_expect_true("json array", sc_json_array_len(sc_json_object_get(root, sc_str_from_cstr("arr"))) == 2);
    sc_json_destroy(root);
    root = nullptr;

    failures += sc_test_expect_status("json string escapes",
                              sc_json_parse(sc_allocator_heap(),
                                            sc_str_from_cstr("{\"s\":\"line\\b\\f\\n\\r\\t\\u003c\\u20ac\\ud83d\\ude80\"}"),
                                            &root,
                                            &error),
                              SC_OK);
    failures += sc_test_expect_true(
        "json unicode decoded",
        sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("s")), &str) &&
            strcmp(str.ptr, "line\b\f\n\r\t<" "\xE2\x82\xAC" "\xF0\x9F\x9A\x80") == 0
    );
    sc_json_destroy(root);
    root = nullptr;

    failures += sc_test_expect_status(
        "json parse duplicate again",
        sc_json_parse(sc_allocator_heap(),
                      sc_str_from_cstr("{\"a\":1,\"a\":2,\"b\":null,\"arr\":[true,\"x\"]}"),
                      &root,
                      &error),
        SC_OK
    );
    failures += sc_test_expect_status("json serialize", sc_json_serialize(root, sc_allocator_heap(), &text), SC_OK);
    failures += sc_test_expect_true("json serialize object", text.len > 0 && text.ptr[0] == '{');
    sc_string_clear(&text);

    failures += sc_test_expect_status("json clone", sc_json_clone(root, sc_allocator_heap(), &clone), SC_OK);
    failures += sc_test_expect_true("json clone lookup", sc_json_object_get(clone, sc_str_from_cstr("a")) != nullptr);

    failures += sc_test_expect_status(
        "json large number rejected",
        sc_json_parse(sc_allocator_heap(), sc_str_from_cstr("1e9999"), &payload, &error),
        SC_ERR_PARSE
    );
    failures += sc_test_expect_true("json parse error offset", error.error_key != nullptr);
    failures += sc_test_expect_status(
        "json invalid utf8",
        sc_json_parse(sc_allocator_heap(), sc_str_from_parts("\xC0\x80", 2), &payload, &error),
        SC_ERR_PARSE
    );
    failures += sc_test_expect_status(
        "json unpaired surrogate",
        sc_json_parse(sc_allocator_heap(), sc_str_from_cstr("\"\\ud83d\""), &payload, &error),
        SC_ERR_PARSE
    );
    for (size_t i = 0; i < 70; ++i) {
        deep_input[pos++] = '[';
    }
    for (size_t i = 0; i < 70; ++i) {
        deep_input[pos++] = ']';
    }
    failures += sc_test_expect_status(
        "json deep nesting",
        sc_json_parse(sc_allocator_heap(), sc_str_from_parts(deep_input, pos), &payload, &error),
        SC_ERR_PARSE
    );
    sc_test_allocator_init(&fail_alloc);
    sc_test_allocator_fail_after(&fail_alloc, 0);
    failures += sc_test_expect_status(
        "json allocation failure",
        sc_json_parse(&fail_alloc.base, sc_str_from_cstr("null"), &payload, &error),
        SC_ERR_NO_MEMORY
    );
    failures += sc_test_expect_status("json huge array create", sc_json_array_new(sc_allocator_heap(), &huge_array), SC_OK);
    for (size_t i = 0; failures == 0 && i < 100000; ++i) {
        sc_status status = sc_json_null_new(sc_allocator_heap(), &huge_item);
        if (!sc_status_is_ok(status)) {
            failures += sc_test_expect_status("json huge array item", status, SC_OK);
            break;
        }
        status = sc_json_array_append(huge_array, huge_item);
        if (!sc_status_is_ok(status)) {
            sc_json_destroy(huge_item);
            huge_item = nullptr;
            failures += sc_test_expect_status("json huge array append", status, SC_OK);
            break;
        }
        huge_item = nullptr;
    }
    if (failures == 0) {
        failures += sc_test_expect_status("json huge array overflow item", sc_json_null_new(sc_allocator_heap(), &huge_item), SC_OK);
        failures += sc_test_expect_status("json huge array overflow", sc_json_array_append(huge_array, huge_item), SC_ERR_PARSE);
        sc_json_destroy(huge_item);
        huge_item = nullptr;
    }
    sc_json_destroy(huge_array);

    failures += sc_test_expect_status("json schema object", sc_json_schema_object(sc_allocator_heap(), &schema), SC_OK);
    failures += sc_test_expect_status(
        "json schema string property",
        sc_json_schema_add_string_property(schema, sc_str_from_cstr("prompt"), true),
        SC_OK
    );
    failures += sc_test_expect_true("json schema property", sc_json_object_get(sc_json_object_get(schema, sc_str_from_cstr("properties")), sc_str_from_cstr("prompt")) != nullptr);

    failures += sc_test_expect_status(
        "json provider payload",
        sc_json_provider_payload(sc_allocator_heap(), sc_str_from_cstr("model"), sc_str_from_cstr("hello"), &payload),
        SC_OK
    );
    failures += sc_test_expect_true("json provider payload prompt",
                            sc_json_as_str(sc_json_object_get(payload, sc_str_from_cstr("prompt")), &str) &&
                                sc_str_equal(str, sc_str_from_cstr("hello")));

    sc_json_destroy(root);
    sc_json_destroy(clone);
    sc_json_destroy(schema);
    sc_json_destroy(payload);
    return failures;
}

static int test_toml(void)
{
    int failures = 0;
    sc_toml_source source = {0};
    sc_toml_diag diag = {0};
    const sc_toml_value *value = nullptr;

    failures += sc_test_expect_status(
        "toml parse",
        sc_toml_parse_source(sc_allocator_heap(),
                             sc_str_from_cstr("config.toml"),
                             sc_str_from_cstr("name = \"zero\"\n[net]\nport = 8080\nenabled = true\n"),
                             &source,
                             &diag),
        SC_OK
    );
    value = sc_toml_get(&source, sc_str_from_cstr("net.port"));
    failures += sc_test_expect_true("toml integer", value != nullptr && value->type == SC_TOML_INTEGER && value->integer_value == 8080);
    value = sc_toml_get(&source, sc_str_from_cstr("net.enabled"));
    failures += sc_test_expect_true("toml bool", value != nullptr && value->type == SC_TOML_BOOL && value->bool_value);
    sc_toml_source_clear(&source);

    failures += sc_test_expect_status(
        "toml diag",
        sc_toml_parse_source(sc_allocator_heap(),
                             sc_str_from_cstr("bad.toml"),
                             sc_str_from_cstr("not valid\n"),
                             &source,
                             &diag),
        SC_ERR_PARSE
    );
    failures += sc_test_expect_true("toml line column", diag.line == 1 && diag.column == 1 && diag.error_key != nullptr);
    sc_toml_diag_clear(&diag);
    sc_toml_source_clear(&source);

    return failures;
}

static int test_cli(void)
{
    int failures = 0;
    char *help_argv[] = {"smolclaw", "config", "--help"};
    char *command_argv[] = {"smolclaw", "gateway", "serve"};
    char *hard_gateway_argv[] = {"smolclaw", "gateway", "serve", "--hard-shutdown"};
    char *bind_gateway_argv[] = {"smolclaw", "gateway", "serve", "--bind", "127.0.0.1"};
    char *init_argv[] = {"smolclaw", "init-config"};
    char *onboard_argv[] = {"smolclaw", "onboard", "--dry-run"};
    char *daemon_argv[] = {"smolclaw", "daemon"};
    char *hard_daemon_argv[] = {"smolclaw", "daemon", "--hard-shutdown"};
    char *hard_chat_argv[] = {"smolclaw", "chat", "--hard-shutdown"};
    char *cron_add_argv[] = {"smolclaw", "cron", "add", "daily-report", "0 9 * * *", "Send daily summary"};
    char *cron_list_argv[] = {"smolclaw", "cron", "list"};
    char *cron_remove_argv[] = {"smolclaw", "cron", "remove", "daily-report"};
    char *bad_argv[] = {"smolclaw", "--wat"};
    sc_cli_parse_result result = {0};

    failures += sc_test_expect_status("cli nested help", sc_cli_parse(sc_cli_default_root(), 3, help_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli help command", result.kind == SC_CLI_PARSE_HELP && strcmp(result.command->name, "config") == 0);
    failures += sc_test_expect_status("cli nested command", sc_cli_parse(sc_cli_default_root(), 3, command_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli command", result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "serve") == 0);
    failures += sc_test_expect_status("cli gateway hard command", sc_cli_parse(sc_cli_default_root(), 4, hard_gateway_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli gateway hard parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "serve") == 0 && result.hard_shutdown);
    failures += sc_test_expect_status("cli gateway bind command", sc_cli_parse(sc_cli_default_root(), 5, bind_gateway_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli gateway bind parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "serve") == 0 &&
                                result.gateway_bind != nullptr && strcmp(result.gateway_bind, "127.0.0.1") == 0);
    failures += sc_test_expect_status("cli init config command", sc_cli_parse(sc_cli_default_root(), 2, init_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli init config parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "init-config") == 0);
    failures += sc_test_expect_status("cli onboard command", sc_cli_parse(sc_cli_default_root(), 3, onboard_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli onboard parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "onboard") == 0);
    failures += sc_test_expect_status("cli daemon command", sc_cli_parse(sc_cli_default_root(), 2, daemon_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli daemon parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "daemon") == 0);
    failures += sc_test_expect_status("cli daemon hard command", sc_cli_parse(sc_cli_default_root(), 3, hard_daemon_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli daemon hard parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "daemon") == 0 && result.hard_shutdown);
    failures += sc_test_expect_status("cli chat hard command", sc_cli_parse(sc_cli_default_root(), 3, hard_chat_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli chat hard parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "chat") == 0 && result.hard_shutdown);
    failures += sc_test_expect_status("cli cron add command", sc_cli_parse(sc_cli_default_root(), 6, cron_add_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli cron add parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "add") == 0);
    failures += sc_test_expect_status("cli cron list command", sc_cli_parse(sc_cli_default_root(), 3, cron_list_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli cron list parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "list") == 0);
    failures += sc_test_expect_status("cli cron remove command", sc_cli_parse(sc_cli_default_root(), 4, cron_remove_argv, &result), SC_OK);
    failures += sc_test_expect_true("cli cron remove parsed",
                            result.kind == SC_CLI_PARSE_COMMAND && strcmp(result.command->name, "remove") == 0);
    failures += sc_test_expect_status("cli bad option", sc_cli_parse(sc_cli_default_root(), 2, bad_argv, &result), SC_ERR_PARSE);

    return failures;
}

static int test_log(void)
{
    int failures = 0;
    sc_log_field secret = {
        .key = "api_key",
        .value = sc_str_from_cstr("secret-value"),
        .secret = false,
    };
    sc_log_field diagnostic = {
        .key = "error_key",
        .value = sc_str_from_cstr("sc.bootstrap.read_open_failed"),
        .secret = false,
    };
    sc_log_level level = SC_LOG_INFO;
    sc_log_format format = SC_LOG_FORMAT_CONSOLE;
    sc_str redacted = sc_log_redact_field(&secret);
    sc_str public_value = sc_log_redact_field(&diagnostic);
    FILE *stream = tmpfile();
    char log_text[512] = {0};
    sc_log_field fields[] = {
        {.key = "message", .value = sc_str_from_cstr("quoted \"line\"\nnext"), .secret = false},
        {.key = "api_token", .value = sc_str_from_cstr("secret-value"), .secret = false},
    };

    failures += sc_test_expect_true("log redacts key", sc_str_equal(redacted, sc_str_from_cstr("[REDACTED]")));
    failures += sc_test_expect_true("log keeps error_key", sc_str_equal(public_value, diagnostic.value));
    failures += sc_test_expect_true("log parses fatal", sc_log_level_from_cstr("fatal", &level) && level == SC_LOG_FATAL);
    failures += sc_test_expect_true("log parses warning", sc_log_level_from_cstr("WARNING", &level) && level == SC_LOG_WARN);
    failures += sc_test_expect_true("log level name", strcmp(sc_log_level_name(SC_LOG_FATAL), "fatal") == 0);
    failures += sc_test_expect_true("log parses console", sc_log_format_from_cstr("console", &format) && format == SC_LOG_FORMAT_CONSOLE);
    failures += sc_test_expect_true("log parses json", sc_log_format_from_cstr("JSON", &format) && format == SC_LOG_FORMAT_JSON);
    failures += sc_test_expect_true("log format name", strcmp(sc_log_format_name(SC_LOG_FORMAT_CONSOLE), "console") == 0);

    failures += sc_test_expect_true("log tmpfile available", stream != nullptr);
    if (stream != nullptr) {
        sc_log_set_format(SC_LOG_FORMAT_JSON);
        sc_log_write_to(stream, SC_LOG_WARN, "sc.test", "test.event", fields, SC_ARRAY_LEN(fields));
        rewind(stream);
        size_t read_count = fread(log_text, 1, sizeof(log_text) - 1, stream);
        log_text[read_count] = '\0';
        failures += sc_test_expect_true("json log object", strstr(log_text, "{\"level\":\"warn\",\"target\":\"sc.test\",\"event\":\"test.event\"") != nullptr);
        failures += sc_test_expect_true("json log escape", strstr(log_text, "\"message\":\"quoted \\\"line\\\"\\nnext\"") != nullptr);
        failures += sc_test_expect_true("json log redacted", strstr(log_text, "secret-value") == nullptr && strstr(log_text, "[REDACTED]") != nullptr);
        sc_log_set_format(SC_LOG_FORMAT_CONSOLE);
        (void)fclose(stream);
    }

    return failures;
}

static int test_url(void)
{
    int failures = 0;
    sc_url url = {0};

    failures += sc_test_expect_status(
        "url parse",
        sc_url_parse(sc_allocator_heap(),
                     sc_str_from_cstr("https://User:Pass@Example.COM:8443/path?q=1#frag"),
                     &url),
        SC_OK
    );
    failures += sc_test_expect_true("url lower host", sc_str_equal(sc_string_as_str(&url.host), sc_str_from_cstr("example.com")));
    failures += sc_test_expect_true("url credentials", sc_url_has_credentials(&url));
    failures += sc_test_expect_status("url reject credentials", sc_url_reject_credentials(&url), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("url domain", sc_url_host_matches_domain(&url, sc_str_from_cstr("example.com")));
    sc_url_clear(&url);

    failures += sc_test_expect_status(
        "url private",
        sc_url_parse(sc_allocator_heap(), sc_str_from_cstr("http://192.168.1.10/"), &url),
        SC_OK
    );
    failures += sc_test_expect_true("url private check", sc_url_host_is_private_address(&url));
    sc_url_clear(&url);

    failures += sc_test_expect_status(
        "url malformed",
        sc_url_parse(sc_allocator_heap(), sc_str_from_cstr("not-a-url"), &url),
        SC_ERR_PARSE
    );

    return failures;
}
