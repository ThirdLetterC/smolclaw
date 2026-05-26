#include "sc/config.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int expect_prop(const sc_config *config, const char *path, const char *expected);
static int test_defaults_and_metadata(void);
static int test_load_and_precedence(void);
static int test_unknown_and_invalid_fields(void);
static int test_property_secret_and_docs(void);
static int test_file_secret_store(void);
static int test_migration_provider_and_proxy(void);
static int test_mcp_config(void);
static int test_lightpanda_config_validation(void);

int main(void)
{
    int failures = 0;

    failures += test_defaults_and_metadata();
    failures += test_load_and_precedence();
    failures += test_unknown_and_invalid_fields();
    failures += test_property_secret_and_docs();
    failures += test_file_secret_store();
    failures += test_migration_provider_and_proxy();
    failures += test_mcp_config();
    failures += test_lightpanda_config_validation();

    return failures == 0 ? 0 : 1;
}

static int expect_prop(const sc_config *config, const char *path, const char *expected)
{
    sc_string value = {0};
    int failures = sc_test_expect_status("config get prop",
                                 sc_config_get_prop(config, sc_str_from_cstr(path), sc_allocator_heap(), &value),
                                 SC_OK);
    failures += sc_test_expect_true("config prop value", strcmp(value.ptr == nullptr ? "" : value.ptr, expected) == 0);
    sc_string_clear(&value);
    return failures;
}

static int test_defaults_and_metadata(void)
{
    sc_config config = {0};
    int failures = 0;

    failures += sc_test_expect_status("config defaults", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("config validate defaults", sc_config_validate(&config, nullptr), SC_OK);
    failures += sc_test_expect_true("config generated field count", sc_config_field_count >= 10);
    failures += sc_test_expect_true("config secret metadata",
                            sc_config_field_is_secret(sc_str_from_cstr("provider.api_key")));
    failures += expect_prop(&config, "memory.backend", "none");
    failures += expect_prop(&config, "gateway.port", "8080");
    failures += expect_prop(&config, "reliability.max_retries", "2");
    failures += expect_prop(&config, "reliability.retry_backoff_ms", "250");
    failures += expect_prop(&config, "agent.consecutive_message_limit", "5");
    failures += expect_prop(&config, "browser.lightpanda.cdp_url", "ws://127.0.0.1:9222");
    failures += expect_prop(&config, "browser.lightpanda.version_url", "http://127.0.0.1:9222/json/version");

    sc_config_clear(&config);
    return failures;
}

static int test_load_and_precedence(void)
{
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_override env[] = {
        {.path = sc_str_from_cstr("provider.api_key"), .value = sc_str_from_cstr("env-secret")},
    };
    sc_config_override runtime[] = {
        {.path = sc_str_from_cstr("gateway.port"), .value = sc_str_from_cstr("7070")},
    };
    sc_config_load_options options = {
        .default_file = {
            .kind = SC_CONFIG_SOURCE_DEFAULT_FILE,
            .source_path = sc_str_from_cstr("default.toml"),
            .body = sc_str_from_cstr("[provider]\ndefault = \"local\"\n[gateway]\nport = 9090\n"),
            .present = true,
        },
        .workspace_marker = {
            .kind = SC_CONFIG_SOURCE_WORKSPACE_MARKER,
            .source_path = sc_str_from_cstr(".smolclaw-workspace"),
            .body = sc_str_from_cstr("[runtime]\nworkspace_path = \"/tmp/work\"\n"),
            .present = true,
        },
        .env_overrides = env,
        .env_override_count = SC_ARRAY_LEN(env),
        .runtime_overrides = runtime,
        .runtime_override_count = SC_ARRAY_LEN(runtime),
    };
    sc_string source = {0};
    int failures = 0;

    failures += sc_test_expect_status("config load", sc_config_load(sc_allocator_heap(), &options, &config, &diag), SC_OK);
    failures += expect_prop(&config, "provider.default", "local");
    failures += expect_prop(&config, "runtime.workspace_path", "/tmp/work");
    failures += expect_prop(&config, "gateway.port", "7070");
    failures += sc_test_expect_status("config prop source",
                              sc_config_get_prop_source(&config,
                                                        sc_str_from_cstr("gateway.port"),
                                                        sc_allocator_heap(),
                                                        &source),
                              SC_OK);
    failures += sc_test_expect_true("config runtime source", strstr(source.ptr, "runtime") != nullptr);

    sc_string_clear(&source);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    return failures;
}

static int test_unknown_and_invalid_fields(void)
{
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options schema_v2 = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("schema-v2.toml"),
            .body = sc_str_from_cstr("schema_version = 2\n"
                                     "[providers]\n"
                                     "fallback = \"gemini\"\n"
                                     "[providers.models.gemini]\n"
                                     "api_key = \"secret\"\n"
                                     "model = \"gemini-test\"\n"
                                     "[channels.telegram]\n"
                                     "allowed_users = [\"smlkw\", \"42\"]\n"
                                     "[trust]\n"
                                     "initial_score = 0.8\n"),
            .present = true,
        },
    };
    sc_config_load_options invalid = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("invalid.toml"),
            .body = sc_str_from_cstr("[gateway]\nport = \"bad\"\n"),
            .present = true,
        },
    };
    sc_string value = {0};
    sc_string redacted = {0};
    int failures = 0;

    failures += sc_test_expect_status("config schema v2",
                              sc_config_load(sc_allocator_heap(), &schema_v2, &config, &diag),
                              SC_OK);
    failures += expect_prop(&config, "schema.version", "2");
    failures += expect_prop(&config, "providers.fallback", "gemini");
    failures += expect_prop(&config, "providers.models.gemini.model", "gemini-test");
    failures += sc_test_expect_status("config array prop",
                              sc_config_get_prop(&config,
                                                 sc_str_from_cstr("channels.telegram.allowed_users"),
                                                 sc_allocator_heap(),
                                                 &value),
                              SC_OK);
    failures += sc_test_expect_true("config array retained", strstr(value.ptr, "smlkw") != nullptr && strstr(value.ptr, "42") != nullptr);
    failures += sc_test_expect_status("config v2 secret redacted",
                              sc_config_get_prop_redacted(&config,
                                                          sc_str_from_cstr("providers.models.gemini.api_key"),
                                                          sc_allocator_heap(),
                                                          &redacted),
                              SC_OK);
    failures += sc_test_expect_true("config v2 secret value", strcmp(redacted.ptr, "[REDACTED]") == 0);
    sc_string_clear(&redacted);
    sc_string_clear(&value);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);

    failures += sc_test_expect_status("config invalid type",
                              sc_config_load(sc_allocator_heap(), &invalid, &config, &diag),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("config invalid diag",
                            sc_str_equal(sc_string_as_str(&diag.path), sc_str_from_cstr("gateway.port")) &&
                                sc_str_equal(sc_string_as_str(&diag.source_path), sc_str_from_cstr("invalid.toml")) &&
                                diag.line == 2);
    sc_config_diag_clear(&diag);
    return failures;
}

static int test_property_secret_and_docs(void)
{
    sc_config config = {0};
    sc_secret_store *secret_store = nullptr;
    sc_string redacted = {0};
    sc_string exported = {0};
    sc_string stored_secret = {0};
    sc_string docs = {0};
    int failures = 0;

    failures += sc_test_expect_status("config defaults", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("secret store", sc_secret_store_memory_new(sc_allocator_heap(), &secret_store), SC_OK);
    failures += sc_test_expect_status("attach secret store", sc_config_attach_secret_store(&config, secret_store, true), SC_OK);
    failures += sc_test_expect_status("config set prop",
                              sc_config_set_prop(&config,
                                                 sc_str_from_cstr("provider.api_key"),
                                                 sc_str_from_cstr("secret-value")),
                              SC_OK);
    failures += sc_test_expect_status("secret store get",
                              sc_secret_store_get(secret_store,
                                                  sc_str_from_cstr("provider.api_key"),
                                                  sc_allocator_heap(),
                                                  &stored_secret),
                              SC_OK);
    failures += sc_test_expect_true("secret store value", strcmp(stored_secret.ptr, "secret-value") == 0);
    sc_string_secure_clear(&stored_secret);
    failures += sc_test_expect_status("config set telegram token",
                              sc_config_set_prop(&config,
                                                 sc_str_from_cstr("channels.telegram.bot_token"),
                                                 sc_str_from_cstr("telegram-secret")),
                              SC_OK);
    failures += sc_test_expect_status("telegram token secret get",
                              sc_secret_store_get(secret_store,
                                                  sc_str_from_cstr("channels.telegram.bot_token"),
                                                  sc_allocator_heap(),
                                                  &stored_secret),
                              SC_OK);
    failures += sc_test_expect_true("telegram token secret value", strcmp(stored_secret.ptr, "telegram-secret") == 0);
    failures += sc_test_expect_status("config get redacted",
                              sc_config_get_prop_redacted(&config,
                                                          sc_str_from_cstr("provider.api_key"),
                                                          sc_allocator_heap(),
                                                          &redacted),
                              SC_OK);
    failures += sc_test_expect_true("config redacted", strcmp(redacted.ptr, "[REDACTED]") == 0);
    failures += sc_test_expect_status("config export redacted", sc_config_export_redacted(&config, sc_allocator_heap(), &exported), SC_OK);
    failures += sc_test_expect_true("config export hides secret", strstr(exported.ptr, "secret-value") == nullptr && strstr(exported.ptr, "[REDACTED]") != nullptr);
    failures += sc_test_expect_status("config docs", sc_config_describe_fields(sc_allocator_heap(), true, &docs), SC_OK);
    failures += sc_test_expect_true("config docs redact default", strstr(docs.ptr, "secret-value") == nullptr);
    failures += sc_test_expect_true("config docs mention secret path", strstr(docs.ptr, "provider.api_key") != nullptr);
    sc_string_clear(&docs);
    failures += sc_test_expect_status("config public docs", sc_config_describe_fields(sc_allocator_heap(), false, &docs), SC_OK);
    failures += sc_test_expect_true("config public docs omit secret path", strstr(docs.ptr, "provider.api_key -") == nullptr);

    sc_string_clear(&redacted);
    sc_string_clear(&exported);
    sc_string_clear(&stored_secret);
    sc_string_clear(&docs);
    sc_config_clear(&config);
    sc_secret_store_destroy(secret_store);
    return failures;
}

static int test_file_secret_store(void)
{
    sc_secret_store *store = nullptr;
    sc_secret_store *reopened = nullptr;
    sc_string stored = {0};
    char path[512] = {0};
    char key_path[520] = {0};
    int failures = 0;
    struct stat st = {0};
    (void)snprintf(path, sizeof(path), "/tmp/smolclaw-secret-%ld.dat", (long)getpid());
    (void)snprintf(key_path, sizeof(key_path), "%s.key", path);
    (void)remove(path);
    (void)remove(key_path);

    failures += sc_test_expect_status("file secret store new",
                              sc_secret_store_file_new(sc_allocator_heap(), sc_str_from_cstr(path), &store),
                              SC_OK);
    failures += sc_test_expect_status("file secret put",
                              sc_secret_store_put(store,
                                                  sc_str_from_cstr("providers.models.gpt.api_key"),
                                                  sc_str_from_cstr("secret-value")),
                              SC_OK);
    failures += sc_test_expect_status("file secret get",
                              sc_secret_store_get(store,
                                                  sc_str_from_cstr("providers.models.gpt.api_key"),
                                                  sc_allocator_heap(),
                                                  &stored),
                              SC_OK);
    failures += sc_test_expect_true("file secret value", strcmp(stored.ptr, "secret-value") == 0);
    failures += sc_test_expect_true("file secret mode", stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);
    sc_string_secure_clear(&stored);
    sc_secret_store_destroy(store);

    failures += sc_test_expect_status("file secret reopen",
                              sc_secret_store_file_new(sc_allocator_heap(), sc_str_from_cstr(path), &reopened),
                              SC_OK);
    failures += sc_test_expect_status("file secret reopened get",
                              sc_secret_store_get(reopened,
                                                  sc_str_from_cstr("providers.models.gpt.api_key"),
                                                  sc_allocator_heap(),
                                                  &stored),
                              SC_OK);
    failures += sc_test_expect_true("file secret reopened value", strcmp(stored.ptr, "secret-value") == 0);

    sc_string_secure_clear(&stored);
    sc_secret_store_destroy(reopened);
    (void)remove(path);
    (void)remove(key_path);
    return failures;
}

static int test_migration_provider_and_proxy(void)
{
    sc_config config = {0};
    sc_provider_resolved provider = {0};
    sc_string credential = {0};
    sc_string proxy = {0};
    int failures = 0;

    failures += sc_test_expect_status("config defaults", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    config.schema_version = 0;
    failures += sc_test_expect_status("config migrate", sc_config_migrate(&config, 0, nullptr), SC_OK);
    failures += sc_test_expect_status("config migrate idempotent",
                              sc_config_migrate(&config, SC_CONFIG_SCHEMA_VERSION_CURRENT, nullptr),
                              SC_OK);
    failures += sc_test_expect_status("config alias",
                              sc_config_add_provider_alias(&config,
                                                           sc_str_from_cstr("fast"),
                                                           sc_str_from_cstr("openai"),
                                                           sc_str_from_cstr("small"),
                                                           sc_str_from_cstr("OPENAI_API_KEY")),
                              SC_OK);
    failures += sc_test_expect_true("config resolve alias",
                            sc_config_resolve_provider_alias(&config, sc_str_from_cstr("fast"), &provider) &&
                                sc_str_equal(provider.model, sc_str_from_cstr("small")));
    failures += sc_test_expect_status("config credential source",
                              sc_config_provider_credential_source(&config, sc_allocator_heap(), &credential),
                              SC_OK);
    failures += sc_test_expect_true("config credential env", strstr(credential.ptr, "SMOLCLAW_PROVIDER_API_KEY") != nullptr);
    sc_string_clear(&credential);
    failures += sc_test_expect_status("config inline credential",
                              sc_config_set_prop(&config,
                                                 sc_str_from_cstr("provider.api_key"),
                                                 sc_str_from_cstr("inline-secret")),
                              SC_OK);
    failures += sc_test_expect_status("config inline credential source",
                              sc_config_provider_credential_source(&config, sc_allocator_heap(), &credential),
                              SC_OK);
    failures += sc_test_expect_true("config inline credential wins", strcmp(credential.ptr, "inline:provider.api_key") == 0);

    failures += sc_test_expect_status("config proxy enabled",
                              sc_config_set_prop(&config,
                                                 sc_str_from_cstr("proxy.enabled"),
                                                 sc_str_from_cstr("true")),
                              SC_OK);
    failures += sc_test_expect_status("config proxy default",
                              sc_config_set_prop(&config,
                                                 sc_str_from_cstr("proxy.default_url"),
                                                 sc_str_from_cstr("http://proxy.default:8080")),
                              SC_OK);
    failures += sc_test_expect_status("config proxy rule",
                              sc_config_add_proxy_rule(&config,
                                                       sc_str_from_cstr("openai"),
                                                       sc_str_from_cstr("http://proxy.openai:8080"),
                                                       true),
                              SC_OK);
    failures += sc_test_expect_status("config proxy match",
                              sc_config_match_proxy(&config,
                                                    sc_str_from_cstr("openai"),
                                                    sc_allocator_heap(),
                                                    &proxy),
                              SC_OK);
    failures += sc_test_expect_true("config proxy rule result", strcmp(proxy.ptr, "http://proxy.openai:8080") == 0);
    sc_string_clear(&proxy);
    failures += sc_test_expect_status("config proxy default match",
                              sc_config_match_proxy(&config,
                                                    sc_str_from_cstr("anthropic"),
                                                    sc_allocator_heap(),
                                                    &proxy),
                              SC_OK);
    failures += sc_test_expect_true("config proxy default result", strcmp(proxy.ptr, "http://proxy.default:8080") == 0);

    sc_string_clear(&credential);
    sc_string_clear(&proxy);
    sc_config_clear(&config);
    return failures;
}

static int test_mcp_config(void)
{
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options options = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("mcp.toml"),
            .body = sc_str_from_cstr("[mcp]\n"
                                     "deferred_loading = true\n"
                                     "[autonomy]\n"
                                     "auto_approve = [\"localfs__read_file\"]\n"
                                     "[providers.models.cc]\n"
                                     "kind = \"claude-code\"\n"
                                     "mcp_server = \"localfs\"\n"
                                     "mcp_tool = \"query\"\n"
                                     "mcp_prompt_field = \"prompt\"\n"
                                     "[[mcp.servers]]\n"
                                     "name = \"localfs\"\n"
                                     "transport = \"stdio\"\n"
                                     "command = \"node\"\n"
                                     "args = [\"server.js\", \"--root\", \"/tmp/work\"]\n"
                                     "[[mcp.servers]]\n"
                                     "name = \"weather\"\n"
                                     "transport = \"http\"\n"
                                     "url = \"https://mcp.example.test/rpc\"\n"),
            .present = true,
        },
    };
    sc_mcp_server_view localfs = {0};
    sc_mcp_server_view weather = {0};
    sc_string auto_approve = {0};
    int failures = 0;

    failures += sc_test_expect_status("config load mcp", sc_config_load(sc_allocator_heap(), &options, &config, &diag), SC_OK);
    failures += expect_prop(&config, "mcp.deferred_loading", "true");
    failures += expect_prop(&config, "providers.models.cc.mcp_server", "localfs");
    failures += expect_prop(&config, "providers.models.cc.mcp_tool", "query");
    failures += expect_prop(&config, "providers.models.cc.mcp_prompt_field", "prompt");
    failures += sc_test_expect_status("config mcp auto approve",
                              sc_config_get_prop(&config,
                                                 sc_str_from_cstr("autonomy.auto_approve"),
                                                 sc_allocator_heap(),
                                                 &auto_approve),
                              SC_OK);
    failures += sc_test_expect_true("config mcp auto approve retained", strstr(auto_approve.ptr, "localfs__read_file") != nullptr);
    failures += sc_test_expect_true("config mcp server count", sc_config_mcp_server_count(&config) == 2);
    failures += sc_test_expect_true("config mcp localfs",
                            sc_config_find_mcp_server(&config, sc_str_from_cstr("localfs"), &localfs) &&
                                localfs.transport == SC_MCP_TRANSPORT_STDIO &&
                                sc_str_equal(localfs.command, sc_str_from_cstr("node")) &&
                                strstr(localfs.args.ptr, "server.js") != nullptr &&
                                localfs.deferred_loading);
    failures += sc_test_expect_true("config mcp weather",
                            sc_config_mcp_server_at(&config, 1, &weather) &&
                                sc_str_equal(weather.name, sc_str_from_cstr("weather")) &&
                                weather.transport == SC_MCP_TRANSPORT_HTTP &&
                                sc_str_equal(weather.url, sc_str_from_cstr("https://mcp.example.test/rpc")));
    failures += sc_test_expect_true("config mcp transport text",
                            sc_str_equal(sc_mcp_transport_to_str(SC_MCP_TRANSPORT_SSE), sc_str_from_cstr("sse")));

    sc_string_clear(&auto_approve);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    return failures;
}

static int test_lightpanda_config_validation(void)
{
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options non_loopback = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("browser-non-loopback.toml"),
            .body = sc_str_from_cstr("[browser.lightpanda]\n"
                                     "cdp_url = \"ws://192.0.2.10:9222\"\n"),
            .present = true,
        },
    };
    sc_config_load_options non_loopback_allowed = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("browser-non-loopback-allowed.toml"),
            .body = sc_str_from_cstr("[browser.lightpanda]\n"
                                     "require_loopback = false\n"
                                     "cdp_url = \"ws://192.0.2.10:9222\"\n"
                                     "version_url = \"http://192.0.2.10:9222/json/version\"\n"),
            .present = true,
        },
    };
    sc_config_load_options invalid_timeout = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("browser-timeout.toml"),
            .body = sc_str_from_cstr("[browser.lightpanda]\n"
                                     "command_timeout_ms = 0\n"),
            .present = true,
        },
    };
    sc_config_load_options invalid_consecutive = {
        .explicit_file = {
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr("agent-consecutive.toml"),
            .body = sc_str_from_cstr("[agent]\n"
                                     "consecutive_message_limit = 0\n"),
            .present = true,
        },
    };
    int failures = 0;

    failures += sc_test_expect_status("config lightpanda rejects non-loopback",
                              sc_config_load(sc_allocator_heap(), &non_loopback, &config, &diag),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("config lightpanda non-loopback path",
                            sc_str_equal(sc_string_as_str(&diag.path), sc_str_from_cstr("browser.lightpanda.cdp_url")) &&
                                strcmp(diag.error_key, "sc.config.lightpanda_endpoint_not_loopback") == 0);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);

    failures += sc_test_expect_status("config lightpanda allows explicit remote",
                              sc_config_load(sc_allocator_heap(), &non_loopback_allowed, &config, &diag),
                              SC_OK);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);

    failures += sc_test_expect_status("config lightpanda timeout positive",
                              sc_config_load(sc_allocator_heap(), &invalid_timeout, &config, &diag),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("config lightpanda timeout path",
                            sc_str_equal(sc_string_as_str(&diag.path), sc_str_from_cstr("browser.lightpanda.command_timeout_ms")) &&
                                strcmp(diag.error_key, "sc.config.invalid_range") == 0);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);

    failures += sc_test_expect_status("config consecutive positive",
                              sc_config_load(sc_allocator_heap(), &invalid_consecutive, &config, &diag),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("config consecutive path",
                            sc_str_equal(sc_string_as_str(&diag.path), sc_str_from_cstr("agent.consecutive_message_limit")) &&
                                strcmp(diag.error_key, "sc.config.invalid_range") == 0);

    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    return failures;
}
