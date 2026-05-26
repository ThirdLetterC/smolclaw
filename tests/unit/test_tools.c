#define _XOPEN_SOURCE 700

#include "sc/autonomy.h"
#include "sc/config.h"
#include "sc/memory.h"
#include "sc/provider.h"
#include "sc/registry.h"
#include "sc/security.h"
#include "sc/tool.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAKE_CALL(args_value) (&(sc_tool_call){.struct_size = sizeof(sc_tool_call), .call_id = sc_str_from_cstr("call"), .args = (args_value)})

typedef struct provider_stream_capture {
    int deltas;
    int done;
    sc_string text;
} provider_stream_capture;

typedef struct tool_async_capture {
    int calls;
    sc_status status;
    bool success;
    sc_string output;
} tool_async_capture;

static int test_file_read_and_registry_specs(void);
static int test_policy_and_approval_failures(void);
static int test_search_tools(void);
static int test_memory_tools(void);
static int test_new_book_tools(void);
static int test_memory_backend_rules(void);
static int test_shell_disabled(void);
static int test_shell_rtk_compression(void);
static int test_mcp_server_tool_spec(void);
static int test_mcp_discover_tools(void);
static int test_mcp_stdio_invoke(void);
static int test_gemini_cli_provider(void);
static int test_claude_code_provider_mcp(void);
static int test_tool_wrappers(void);
static sc_status argv_for_script(sc_str script, sc_string *out);
static int read_file_text(sc_str path, char *buffer, size_t buffer_size);
static sc_status parse_args(const char *json, sc_json_value **out);
static sc_status capture_provider_stream(void *user_data, const sc_provider_stream_event *event);
static void provider_stream_capture_clear(provider_stream_capture *capture);
static void tool_async_capture_complete(void *user_data, const sc_tool_result *result, sc_status status);
static void tool_async_capture_clear(tool_async_capture *capture);
static sc_status url_tool_spec(void *impl, sc_tool_spec *out);
static sc_status url_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void url_tool_destroy(void *impl);

static const sc_tool_vtab url_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "url_probe",
    .display_name = "URL probe",
    .feature_flag = "SC_TEST_URL_TOOL",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = url_tool_spec,
    .invoke = url_tool_invoke,
    .destroy = url_tool_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_file_read_and_registry_specs();
    failures += test_policy_and_approval_failures();
    failures += test_search_tools();
    failures += test_memory_tools();
    failures += test_new_book_tools();
    failures += test_memory_backend_rules();
    failures += test_shell_disabled();
    failures += test_shell_rtk_compression();
    failures += test_mcp_server_tool_spec();
    failures += test_mcp_discover_tools();
    failures += test_mcp_stdio_invoke();
    failures += test_gemini_cli_provider();
    failures += test_claude_code_provider_mcp();
    failures += test_tool_wrappers();

    return failures == 0 ? 0 : 1;
}

static int test_file_read_and_registry_specs(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context context = {0};
    sc_tool *tool = nullptr;
    sc_tool_result result = {0};
    sc_tool_spec spec = {0};
    sc_json_value *args = nullptr;
    sc_json_value *specs = nullptr;
    sc_string specs_json = {0};
    sc_string dir = {0};
    sc_string path = {0};
    char args_json[1024] = {0};
    int written = 0;

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("file path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("a.txt"), &path), SC_OK);
    failures += sc_test_expect_true("write file", sc_test_write_file(sc_string_as_str(&path), "abcdef") == 0);
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("workspace", sc_security_policy_set_workspace(&policy, sc_string_as_str(&dir)), SC_OK);
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){
        .struct_size = sizeof(context),
        .policy = &policy,
        .receipts = &receipts,
        .max_output_bytes = 3,
    };

    failures += sc_test_expect_status("file read new", sc_tool_file_read_new(sc_allocator_heap(), &context, &tool), SC_OK);
    failures += sc_test_expect_status("file read spec", sc_tool_spec_get(tool, &spec), SC_OK);
    failures += sc_test_expect_true("file read schema exists", spec.input_schema != nullptr);
    failures += sc_test_expect_true("file read risk", spec.risk == SC_TOOL_RISK_READONLY);

    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\"}", path.ptr);
    failures += sc_test_expect_true("args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("parse args", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("file read invoke", sc_tool_invoke(tool, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("file read truncated", strcmp(result.output.ptr, "abc") == 0);
    failures += sc_test_expect_true("receipt appended", receipts.receipts.len == 1 && sc_receipt_chain_verify(&receipts));
    if (receipts.receipts.len > 0) {
        const sc_tool_receipt *receipt = sc_vec_at_const(&receipts.receipts, 0);
        failures += sc_test_expect_true("receipt args hashed",
                                receipt != nullptr &&
                                    strstr(receipt->args_summary.ptr, "hash=") != nullptr &&
                                    strstr(receipt->args_summary.ptr, path.ptr) == nullptr);
        failures += sc_test_expect_true("receipt output hashed",
                                receipt != nullptr &&
                                    strstr(receipt->output_summary.ptr, "hash=") != nullptr &&
                                    strstr(receipt->output_summary.ptr, "abc") == nullptr);
    }
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("parse missing args", parse_args("{}", &args), SC_OK);
    failures += sc_test_expect_status("missing required arg",
                              sc_tool_invoke(tool, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_INVALID_ARGUMENT);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("parse wrong type args", parse_args("{\"path\":3}", &args), SC_OK);
    failures += sc_test_expect_status("wrong type arg",
                              sc_tool_invoke(tool, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_true("schema failures rejected before receipts", receipts.receipts.len == 1 && sc_receipt_chain_verify(&receipts));

    sc_tool *tools[] = {tool};
    failures += sc_test_expect_status("model specs", sc_tool_registry_model_specs_from_tools(tools, 1, sc_allocator_heap(), &specs), SC_OK);
    failures += sc_test_expect_status("serialize specs", sc_json_serialize(specs, sc_allocator_heap(), &specs_json), SC_OK);
    failures += sc_test_expect_true("specs mention file_read", strstr(specs_json.ptr, "file_read") != nullptr);

    sc_string_clear(&specs_json);
    sc_json_destroy(specs);
    sc_json_destroy(args);
    sc_tool_destroy(tool);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_policy_and_approval_failures(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context context = {0};
    sc_tool *file_tool = nullptr;
    sc_tool *memory_tool = nullptr;
    sc_tool *approval_tool = nullptr;
    sc_tool_result result = {0};
    sc_memory *memory = nullptr;
    sc_json_value *args = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    char args_json[1024] = {0};
    int written = 0;

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("file path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("a.txt"), &path), SC_OK);
    failures += sc_test_expect_true("write file", sc_test_write_file(sc_string_as_str(&path), "content") == 0);
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("workspace", sc_security_policy_set_workspace(&policy, sc_string_as_str(&dir)), SC_OK);
    failures += sc_test_expect_status("deny file read", sc_security_policy_add_denied_tool(&policy, sc_str_from_cstr("file_read")), SC_OK);
    failures += sc_test_expect_status("none memory", sc_memory_none_new(sc_allocator_heap(), &memory), SC_OK);
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy, .receipts = &receipts, .memory = memory};

    failures += sc_test_expect_status("file read new", sc_tool_file_read_new(sc_allocator_heap(), &context, &file_tool), SC_OK);
    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\"}", path.ptr);
    failures += sc_test_expect_true("args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("parse args", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("denied file read", sc_tool_invoke(file_tool, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("failure receipt", receipts.receipts.len == 1 && sc_receipt_chain_verify(&receipts));
    sc_json_destroy(args);
    args = nullptr;

    sc_security_policy_clear(&policy);
    failures += sc_test_expect_status("policy defaults 2", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("workspace 2", sc_security_policy_set_workspace(&policy, sc_string_as_str(&dir)), SC_OK);
    context.policy = &policy;
    failures += sc_test_expect_status("memory store new", sc_tool_memory_store_new(sc_allocator_heap(), &context, &memory_tool), SC_OK);
    failures += sc_test_expect_status("parse memory args", parse_args("{\"namespace\":\"n\",\"key\":\"k\",\"content\":\"v\"}", &args), SC_OK);
    failures += sc_test_expect_status("approval required",
                              sc_tool_invoke(memory_tool, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_CANCELLED);
    failures += sc_test_expect_true("approval failure receipt", receipts.receipts.len == 2 && sc_receipt_chain_verify(&receipts));

    sc_json_destroy(args);
    args = nullptr;
    sc_tool_destroy(memory_tool);
    memory_tool = nullptr;

    failures += sc_test_expect_status("approval test tool", sc_tool_approval_test_new(sc_allocator_heap(), &context, &approval_tool), SC_OK);
    failures += sc_test_expect_status("parse approval args", parse_args("{\"message\":\"probe\"}", &args), SC_OK);
    failures += sc_test_expect_status("approval test requires approval",
                              sc_tool_invoke(approval_tool, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_CANCELLED);
    failures += sc_test_expect_true("approval test failure receipt", receipts.receipts.len == 3 && sc_receipt_chain_verify(&receipts));
    failures += sc_test_expect_status("approval test auto approve policy",
                              sc_security_policy_add_auto_approved_tool(&policy, sc_str_from_cstr("approval_test")),
                              SC_OK);
    failures += sc_test_expect_status("approval test auto approved",
                              sc_tool_invoke(approval_tool, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_OK);
    failures += sc_test_expect_true("approval test output", strstr(result.output.ptr, "approved=true") != nullptr);
    failures += sc_test_expect_true("approval test success receipt", receipts.receipts.len == 4 && sc_receipt_chain_verify(&receipts));
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    sc_tool_destroy(approval_tool);
    sc_tool_destroy(memory_tool);
    sc_tool_destroy(file_tool);
    sc_memory_destroy(memory);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_search_tools(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context context = {0};
    sc_tool *content = nullptr;
    sc_tool *glob = nullptr;
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    char args_json[1024] = {0};
    int written = 0;

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("file path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("needle.txt"), &path), SC_OK);
    failures += sc_test_expect_true("write file", sc_test_write_file(sc_string_as_str(&path), "find the needle") == 0);
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("workspace", sc_security_policy_set_workspace(&policy, sc_string_as_str(&dir)), SC_OK);
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy, .receipts = &receipts, .max_output_bytes = 4096};

    failures += sc_test_expect_status("content new", sc_tool_content_search_new(sc_allocator_heap(), &context, &content), SC_OK);
    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\",\"query\":\"needle\"}", dir.ptr);
    failures += sc_test_expect_true("content args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("parse content args", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("content invoke", sc_tool_invoke(content, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("content found", strstr(result.output.ptr, "needle.txt") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;
    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\",\"query\":\"needle\\u0000suffix\"}", dir.ptr);
    failures += sc_test_expect_true("content nul args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("parse content nul args", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("content rejects nul query",
                              sc_tool_invoke(content, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_INVALID_ARGUMENT);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("glob new", sc_tool_glob_search_new(sc_allocator_heap(), &context, &glob), SC_OK);
    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\",\"pattern\":\"*.txt\"}", dir.ptr);
    failures += sc_test_expect_true("glob args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("parse glob args", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("glob invoke", sc_tool_invoke(glob, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("glob found", strstr(result.output.ptr, "needle.txt") != nullptr);

    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    sc_tool_destroy(glob);
    sc_tool_destroy(content);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_memory_tools(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context context = {0};
    sc_memory *memory = nullptr;
    sc_tool *store = nullptr;
    sc_tool *recall = nullptr;
    sc_tool *export_tool = nullptr;
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;

    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.autonomy = SC_AUTONOMY_AUTONOMOUS;
    failures += sc_test_expect_status("none memory", sc_memory_markdown_open(sc_allocator_heap(), sc_str_from_cstr("/tmp/sc-tools-memory.md"), &memory), SC_OK);
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy, .receipts = &receipts, .memory = memory};

    failures += sc_test_expect_status("memory store new", sc_tool_memory_store_new(sc_allocator_heap(), &context, &store), SC_OK);
    failures += sc_test_expect_status("memory recall new", sc_tool_memory_recall_new(sc_allocator_heap(), &context, &recall), SC_OK);
    failures += sc_test_expect_status("memory export new", sc_tool_memory_export_new(sc_allocator_heap(), &context, &export_tool), SC_OK);
    failures += sc_test_expect_status("parse store", parse_args("{\"namespace\":\"n\",\"key\":\"k\",\"content\":\"v\"}", &args), SC_OK);
    failures += sc_test_expect_status("memory store", sc_tool_invoke(store, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("memory store location",
                            result.output.ptr != nullptr &&
                                strstr(result.output.ptr, "namespace=n") != nullptr &&
                                strstr(result.output.ptr, "key=k") != nullptr &&
                                strstr(result.output.ptr, "v") == nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("parse recall", parse_args("{\"namespace\":\"n\",\"key\":\"k\"}", &args), SC_OK);
    failures += sc_test_expect_status("memory recall", sc_tool_invoke(recall, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("memory recall output", strcmp(result.output.ptr, "v") == 0);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("parse missing recall", parse_args("{\"namespace\":\"n\",\"key\":\"missing\"}", &args), SC_OK);
    failures += sc_test_expect_status("memory recall missing", sc_tool_invoke(recall, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("memory recall missing output",
                            result.output.ptr != nullptr &&
                                strstr(result.output.ptr, "not_found") != nullptr &&
                                strstr(result.output.ptr, "key=missing") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("parse export", parse_args("{\"namespace\":\"n\"}", &args), SC_OK);
    failures += sc_test_expect_status("memory export", sc_tool_invoke(export_tool, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("memory export output", strstr(result.output.ptr, "\"k\"") != nullptr);
    failures += sc_test_expect_true("memory receipts", receipts.receipts.len == 4 && sc_receipt_chain_verify(&receipts));

    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    sc_tool_destroy(export_tool);
    sc_tool_destroy(recall);
    sc_tool_destroy(store);
    sc_memory_destroy(memory);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    return failures;
}

static int test_new_book_tools(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_config config = {0};
    sc_receipt_chain receipts = {0};
    sc_memory *memory = nullptr;
    sc_cron_job_store cron_jobs = {0};
    sc_cron_run_store cron_runs = {0};
    sc_tool_context context = {0};
    sc_tool *file_write = nullptr;
    sc_tool *file_list = nullptr;
    sc_tool *time_tool = nullptr;
    sc_tool *memory_search = nullptr;
    sc_tool *memory_pin = nullptr;
    sc_tool *http = nullptr;
    sc_tool *web_search = nullptr;
    sc_tool *browser = nullptr;
    sc_tool *browser_screenshot = nullptr;
    sc_tool *cron_list = nullptr;
    sc_tool *cron_upsert = nullptr;
    sc_tool *cron_remove = nullptr;
    sc_tool *cron_missing = nullptr;
    sc_tool *tool_diagnostics = nullptr;
    sc_tool *policy_explain = nullptr;
    sc_tool *tool_registry_list = nullptr;
    sc_tool *dependency_status = nullptr;
    sc_tool *capability_matrix = nullptr;
    sc_tool *resource_usage = nullptr;
    sc_async_context *async_context = nullptr;
    sc_tool_result result = {0};
    sc_tool_spec spec = {0};
    tool_async_capture async_capture = {0};
    sc_json_value *args = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    sc_string browser_schema_json = {0};
    char args_json[1024] = {0};
    int written = 0;

    failures += sc_test_expect_status("new tools temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("new tools file path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("created.txt"), &path),
                              SC_OK);
    failures += sc_test_expect_status("new tools policy", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.autonomy = SC_AUTONOMY_AUTONOMOUS;
    failures += sc_test_expect_status("new tools workspace", sc_security_policy_set_workspace(&policy, sc_string_as_str(&dir)), SC_OK);
    failures += sc_test_expect_status("new tools config", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("fake search config",
                              sc_config_set_prop(&config,
                                                 sc_str_from_cstr("tools.web_search.provider"),
                                                 sc_str_from_cstr("fake")),
                              SC_OK);
    failures += sc_test_expect_status("new tools memory", sc_memory_none_new(sc_allocator_heap(), &memory), SC_OK);
    sc_cron_job_store_init(&cron_jobs, sc_allocator_heap());
    sc_cron_run_store_init(&cron_runs, sc_allocator_heap());
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){
        .struct_size = sizeof(context),
        .policy = &policy,
        .receipts = &receipts,
        .memory = memory,
        .config = &config,
        .cron_jobs = &cron_jobs,
        .cron_runs = &cron_runs,
        .max_output_bytes = 4096,
        .timeout_ms = 1000,
    };

    failures += sc_test_expect_status("file write new", sc_tool_file_write_new(sc_allocator_heap(), &context, &file_write), SC_OK);
    failures += sc_test_expect_status("file list new", sc_tool_file_list_new(sc_allocator_heap(), &context, &file_list), SC_OK);
    failures += sc_test_expect_status("time new", sc_tool_time_new(sc_allocator_heap(), &context, &time_tool), SC_OK);
    failures += sc_test_expect_status("memory search new", sc_tool_memory_search_new(sc_allocator_heap(), &context, &memory_search), SC_OK);
    failures += sc_test_expect_status("memory pin new", sc_tool_memory_pin_new(sc_allocator_heap(), &context, &memory_pin), SC_OK);
    failures += sc_test_expect_status("http new", sc_tool_http_new(sc_allocator_heap(), &context, &http), SC_OK);
    failures += sc_test_expect_status("web search new", sc_tool_web_search_new(sc_allocator_heap(), &context, &web_search), SC_OK);
    failures += sc_test_expect_status("browser new", sc_tool_browser_new(sc_allocator_heap(), &context, &browser), SC_OK);
    failures += sc_test_expect_status("browser screenshot new", sc_tool_browser_screenshot_new(sc_allocator_heap(), &context, &browser_screenshot), SC_OK);
    failures += sc_test_expect_status("cron list new", sc_tool_cron_list_new(sc_allocator_heap(), &context, &cron_list), SC_OK);
    failures += sc_test_expect_status("cron upsert new", sc_tool_cron_upsert_new(sc_allocator_heap(), &context, &cron_upsert), SC_OK);
    failures += sc_test_expect_status("cron remove new", sc_tool_cron_remove_new(sc_allocator_heap(), &context, &cron_remove), SC_OK);
    failures += sc_test_expect_status("tool diagnostics new", sc_tool_tool_diagnostics_new(sc_allocator_heap(), &context, &tool_diagnostics), SC_OK);
    failures += sc_test_expect_status("policy explain new", sc_tool_policy_explain_new(sc_allocator_heap(), &context, &policy_explain), SC_OK);
    failures += sc_test_expect_status("tool registry list new", sc_tool_tool_registry_list_new(sc_allocator_heap(), &context, &tool_registry_list), SC_OK);
    failures += sc_test_expect_status("dependency status new", sc_tool_dependency_status_new(sc_allocator_heap(), &context, &dependency_status), SC_OK);
    failures += sc_test_expect_status("capability matrix new", sc_tool_capability_matrix_new(sc_allocator_heap(), &context, &capability_matrix), SC_OK);
    failures += sc_test_expect_status("resource usage new", sc_tool_resource_usage_new(sc_allocator_heap(), &context, &resource_usage), SC_OK);
    failures += sc_test_expect_status("file write spec", sc_tool_spec_get(file_write, &spec), SC_OK);
    failures += sc_test_expect_true("file write name", sc_str_equal(spec.name, sc_str_from_cstr("file_write")));
    failures += sc_test_expect_status("browser spec", sc_tool_spec_get(browser, &spec), SC_OK);
    failures += sc_test_expect_status("browser schema json", sc_json_serialize(spec.input_schema, sc_allocator_heap(), &browser_schema_json), SC_OK);
    failures += sc_test_expect_true("browser schema action enum",
                            strstr(browser_schema_json.ptr, "\"enum\"") != nullptr &&
                                strstr(browser_schema_json.ptr, "\"open\"") != nullptr &&
                                strstr(browser_schema_json.ptr, "\"diagnose\"") != nullptr);
    failures += sc_test_expect_true("browser schema timeout",
                            strstr(browser_schema_json.ptr, "\"timeout_ms\"") != nullptr &&
                                strstr(browser_schema_json.ptr, "\"integer\"") != nullptr);
    sc_string_clear(&browser_schema_json);
    failures += sc_test_expect_status("browser screenshot spec", sc_tool_spec_get(browser_screenshot, &spec), SC_OK);
    failures += sc_test_expect_true("browser screenshot name", sc_str_equal(spec.name, sc_str_from_cstr("browser_screenshot")));
    failures += sc_test_expect_status("browser screenshot schema json", sc_json_serialize(spec.input_schema, sc_allocator_heap(), &browser_schema_json), SC_OK);
    failures += sc_test_expect_true("browser screenshot schema timeout",
                            strstr(browser_schema_json.ptr, "\"timeout_ms\"") != nullptr &&
                                strstr(browser_schema_json.ptr, "\"integer\"") != nullptr);
    failures += sc_test_expect_true("browser screenshot schema format",
                            strstr(browser_schema_json.ptr, "\"format\"") != nullptr &&
                                strstr(browser_schema_json.ptr, "\"photo\"") != nullptr &&
                                strstr(browser_schema_json.ptr, "\"document\"") != nullptr);
    sc_string_clear(&browser_schema_json);

    failures += sc_test_expect_status("tool diagnostics parse", parse_args("{\"tool\":\"shell\"}", &args), SC_OK);
    failures += sc_test_expect_status("tool diagnostics invoke", sc_tool_invoke(tool_diagnostics, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("tool diagnostics output",
                            strstr(result.output.ptr, "name=shell") != nullptr &&
                                strstr(result.output.ptr, "error_key=sc.security.shell_denied") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("browser screenshot diagnostics parse", parse_args("{\"tool\":\"browser_screenshot\"}", &args), SC_OK);
    failures += sc_test_expect_status("browser screenshot diagnostics invoke", sc_tool_invoke(tool_diagnostics, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("browser screenshot diagnostics output",
                            strstr(result.output.ptr, "name=browser_screenshot") != nullptr &&
                                strstr(result.output.ptr, "websocket_client=") != nullptr &&
                                strstr(result.output.ptr, "version_endpoint=") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("policy explain parse", parse_args("{\"tool\":\"shell\",\"shell\":\"date\"}", &args), SC_OK);
    failures += sc_test_expect_status("policy explain invoke", sc_tool_invoke(policy_explain, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("policy explain output", strstr(result.output.ptr, "decision=denied") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("registry list parse", parse_args("{}", &args), SC_OK);
    failures += sc_test_expect_status("registry list invoke", sc_tool_invoke(tool_registry_list, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("registry list output", strstr(result.output.ptr, "tool_diagnostics") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("dependency status parse", parse_args("{}", &args), SC_OK);
    failures += sc_test_expect_status("dependency status invoke", sc_tool_invoke(dependency_status, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("dependency status output", strstr(result.output.ptr, "pdftotext=") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("capability matrix parse", parse_args("{}", &args), SC_OK);
    failures += sc_test_expect_status("capability matrix invoke", sc_tool_invoke(capability_matrix, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("capability matrix output", strstr(result.output.ptr, "capability status") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("resource usage parse", parse_args("{}", &args), SC_OK);
    failures += sc_test_expect_status("resource usage invoke", sc_tool_invoke(resource_usage, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("resource usage output",
                            strstr(result.output.ptr, "process_user_cpu_ms=") != nullptr &&
                                strstr(result.output.ptr, "process_max_rss_kb=") != nullptr &&
                                strstr(result.output.ptr, "tool_registry_live=") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\",\"content\":\"hello\",\"mode\":\"create\"}", path.ptr);
    failures += sc_test_expect_true("file write args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("file write parse", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("file write invoke", sc_tool_invoke(file_write, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    written = snprintf(args_json, sizeof(args_json), "{\"path\":\"%s\"}", dir.ptr);
    failures += sc_test_expect_true("file list args format", written > 0 && (size_t)written < sizeof(args_json));
    failures += sc_test_expect_status("file list parse", parse_args(args_json, &args), SC_OK);
    failures += sc_test_expect_status("file list invoke", sc_tool_invoke(file_list, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("file list output", strstr(result.output.ptr, "created.txt") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("time parse", parse_args("{}", &args), SC_OK);
    failures += sc_test_expect_status("time invoke", sc_tool_invoke(time_tool, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("time output", strstr(result.output.ptr, "timezone=UTC") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("time unsupported parse", parse_args("{\"timezone\":\"America/New_York\"}", &args), SC_OK);
    failures += sc_test_expect_status("time unsupported timezone",
                              sc_tool_invoke(time_tool, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_UNSUPPORTED);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("memory pin parse", parse_args("{\"namespace\":\"n\",\"key\":\"k\",\"content\":\"v\"}", &args), SC_OK);
    failures += sc_test_expect_status("memory pin invoke", sc_tool_invoke(memory_pin, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("memory search parse", parse_args("{\"namespace\":\"n\",\"query\":\"v\"}", &args), SC_OK);
    failures += sc_test_expect_status("memory search invoke", sc_tool_invoke(memory_search, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("http private parse", parse_args("{\"method\":\"GET\",\"url\":\"http://127.0.0.1/private\"}", &args), SC_OK);
    failures += sc_test_expect_status("http private denied", sc_tool_invoke(http, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("web search parse", parse_args("{\"query\":\"smolclaw\"}", &args), SC_OK);
    failures += sc_test_expect_status("web search fake", sc_tool_invoke(web_search, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("web search fake output", strstr(result.output.ptr, "provider=fake") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("async context new", sc_async_context_new(nullptr, &async_context), SC_OK);
    failures += sc_test_expect_status("web search async parse", parse_args("{\"query\":\"smolclaw\"}", &args), SC_OK);
    failures += sc_test_expect_status("web search async fake",
                              sc_tool_invoke_async(web_search,
                                                   async_context,
                                                   MAKE_CALL(args),
                                                   sc_allocator_heap(),
                                                   tool_async_capture_complete,
                                                   &async_capture,
                                                   nullptr),
                              SC_OK);
    failures += sc_test_expect_true("web search async callback", async_capture.calls == 1);
    failures += sc_test_expect_status("web search async status", async_capture.status, SC_OK);
    failures += sc_test_expect_true("web search async output", async_capture.success && strstr(async_capture.output.ptr, "provider=fake") != nullptr);
    tool_async_capture_clear(&async_capture);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("http async private parse", parse_args("{\"method\":\"GET\",\"url\":\"http://127.0.0.1/private\"}", &args), SC_OK);
    failures += sc_test_expect_status("http async private invoke",
                              sc_tool_invoke_async(http,
                                                   async_context,
                                                   MAKE_CALL(args),
                                                   sc_allocator_heap(),
                                                   tool_async_capture_complete,
                                                   &async_capture,
                                                   nullptr),
                              SC_OK);
    failures += sc_test_expect_true("http async private callback", async_capture.calls == 1);
    failures += sc_test_expect_status("http async private denied", async_capture.status, SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("http async private no output", !async_capture.success && async_capture.output.ptr == nullptr);
    tool_async_capture_clear(&async_capture);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("browser diagnose parse", parse_args("{\"action\":\"diagnose\"}", &args), SC_OK);
    failures += sc_test_expect_status("browser diagnose", sc_tool_invoke(browser, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("browser diagnose output",
                            strstr(result.output.ptr, "lightpanda=") != nullptr &&
                                strstr(result.output.ptr, "websocket-client=") != nullptr &&
                                strstr(result.output.ptr, "version-endpoint=") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("browser open missing url parse", parse_args("{\"action\":\"open\"}", &args), SC_OK);
    {
        sc_status open_status = sc_tool_invoke(browser, MAKE_CALL(args), sc_allocator_heap(), &result);
        failures += sc_test_expect_true("browser open action recognized",
                                open_status.code == SC_ERR_INVALID_ARGUMENT &&
                                    open_status.error_key != nullptr &&
                                    strcmp(open_status.error_key, "sc.browser_tool.url_required") == 0);
        sc_status_clear(&open_status);
    }
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("browser open timeout parse", parse_args("{\"action\":\"open\",\"timeout_ms\":250}", &args), SC_OK);
    {
        sc_status timeout_status = sc_tool_invoke(browser, MAKE_CALL(args), sc_allocator_heap(), &result);
        failures += sc_test_expect_true("browser open accepts timeout field",
                                timeout_status.code == SC_ERR_INVALID_ARGUMENT &&
                                    timeout_status.error_key != nullptr &&
                                    strcmp(timeout_status.error_key, "sc.browser_tool.url_required") == 0);
        sc_status_clear(&timeout_status);
    }
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("browser invalid timeout parse", parse_args("{\"action\":\"open\",\"timeout_ms\":0}", &args), SC_OK);
    {
        sc_status timeout_status = sc_tool_invoke(browser, MAKE_CALL(args), sc_allocator_heap(), &result);
        failures += sc_test_expect_true("browser rejects invalid timeout",
                                timeout_status.code == SC_ERR_INVALID_ARGUMENT &&
                                    timeout_status.error_key != nullptr &&
                                    strcmp(timeout_status.error_key, "sc.browser_tool.timeout_invalid") == 0);
        sc_status_clear(&timeout_status);
    }
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("browser screenshot missing url parse", parse_args("{}", &args), SC_OK);
    {
        sc_status screenshot_status = sc_tool_invoke(browser_screenshot, MAKE_CALL(args), sc_allocator_heap(), &result);
        failures += sc_test_expect_true("browser screenshot requires url",
                                screenshot_status.code == SC_ERR_INVALID_ARGUMENT &&
                                    screenshot_status.error_key != nullptr &&
                                    strcmp(screenshot_status.error_key, "sc.tool.schema.required_missing") == 0);
        sc_status_clear(&screenshot_status);
    }
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("browser screenshot invalid format parse",
                              parse_args("{\"url\":\"https://example.com\",\"format\":\"pdf\"}", &args),
                              SC_OK);
    {
        sc_status screenshot_status = sc_tool_invoke(browser_screenshot, MAKE_CALL(args), sc_allocator_heap(), &result);
        failures += sc_test_expect_true("browser screenshot rejects invalid format",
                                screenshot_status.code == SC_ERR_INVALID_ARGUMENT &&
                                    screenshot_status.error_key != nullptr &&
                                    strcmp(screenshot_status.error_key, "sc.browser_screenshot_tool.format_invalid") == 0);
        sc_status_clear(&screenshot_status);
    }
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status(
        "cron upsert parse",
        parse_args("{\"id\":\"job-1\",\"schedule\":\"* * * * *\",\"prompt\":\"check\",\"delivery_target\":\"default\"}", &args),
        SC_OK);
    failures += sc_test_expect_status("cron upsert invoke", sc_tool_invoke(cron_upsert, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("cron list parse", parse_args("{\"id\":\"job-1\"}", &args), SC_OK);
    failures += sc_test_expect_status("cron list invoke", sc_tool_invoke(cron_list, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("cron list output", strstr(result.output.ptr, "job-1") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("cron remove parse", parse_args("{\"id\":\"job-1\"}", &args), SC_OK);
    failures += sc_test_expect_status("cron remove invoke", sc_tool_invoke(cron_remove, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    context.cron_jobs = nullptr;
    failures += sc_test_expect_status("cron missing new", sc_tool_cron_list_new(sc_allocator_heap(), &context, &cron_missing), SC_OK);
    failures += sc_test_expect_status("cron missing parse", parse_args("{}", &args), SC_OK);
    size_t receipts_before_cron_missing = receipts.receipts.len;
    failures += sc_test_expect_status("cron missing invoke", sc_tool_invoke(cron_missing, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_true("cron missing receipt",
                            receipts.receipts.len == receipts_before_cron_missing + 1 && sc_receipt_chain_verify(&receipts));
    sc_json_destroy(args);

    sc_tool_destroy(cron_missing);
    sc_tool_destroy(capability_matrix);
    sc_tool_destroy(resource_usage);
    sc_tool_destroy(dependency_status);
    sc_tool_destroy(tool_registry_list);
    sc_tool_destroy(policy_explain);
    sc_tool_destroy(tool_diagnostics);
    sc_tool_destroy(cron_remove);
    sc_tool_destroy(cron_upsert);
    sc_tool_destroy(cron_list);
    sc_tool_destroy(browser_screenshot);
    sc_tool_destroy(browser);
    sc_tool_destroy(web_search);
    sc_tool_destroy(http);
    sc_tool_destroy(memory_pin);
    sc_tool_destroy(memory_search);
    sc_tool_destroy(time_tool);
    sc_tool_destroy(file_list);
    sc_tool_destroy(file_write);
    sc_async_context_destroy(async_context);
    sc_receipt_chain_clear(&receipts);
    sc_cron_run_store_clear(&cron_runs);
    sc_cron_job_store_clear(&cron_jobs);
    sc_memory_destroy(memory);
    sc_config_clear(&config);
    sc_security_policy_clear(&policy);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_memory_backend_rules(void)
{
    int failures = 0;
    sc_string dir = {0};
    sc_string path = {0};
    sc_string sqlite_path = {0};
    sc_memory *memory = nullptr;
    sc_memory *sqlite = nullptr;
    sc_string value = {0};
    sc_string snapshot = {0};
    sc_memory_result result = {0};
    sc_status sqlite_status = {0};
    const sc_memory_vtab *vtab = nullptr;
    const char bad_utf8[] = {(char)0xc3, (char)0x28, '\0'};
    sc_memory_query query = {.struct_size = sizeof(query), .namespace_name = sc_str_from_cstr("rules")};
    sc_memory_export_options raw_export = {
        .struct_size = sizeof(raw_export),
        .include_raw_sensitive_content = true,
        .include_redacted = true,
        .include_deleted = true,
    };

    failures += sc_test_expect_status("rules temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("rules markdown path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("memory.md"), &path),
                              SC_OK);
    failures += sc_test_expect_status("rules markdown open", sc_memory_markdown_open(sc_allocator_heap(), sc_string_as_str(&path), &memory), SC_OK);
    vtab = sc_memory_vtab_of(memory);
    failures += sc_test_expect_true("rules descriptor schema", vtab != nullptr && vtab->config_schema_ref != nullptr);
    failures += sc_test_expect_true("rules descriptor persistence", vtab != nullptr && vtab->persistence_type == SC_MEMORY_PERSISTENCE_FILE);
    failures += sc_test_expect_true("rules descriptor capabilities",
                            vtab != nullptr &&
                                (vtab->memory_capabilities & SC_MEMORY_BACKEND_CAP_REDACT) != 0 &&
                                (vtab->memory_capabilities & SC_MEMORY_BACKEND_CAP_IMPORT_EXPORT) != 0);

    failures += sc_test_expect_status("rules put",
                              sc_memory_put(memory,
                                            &(sc_memory_record){
                                                .struct_size = sizeof(sc_memory_record),
                                                .namespace_name = sc_str_from_cstr("rules"),
                                                .key = sc_str_from_cstr("alpha"),
                                                .value = sc_str_from_cstr("visible-content"),
                                                .metadata_json = sc_str_from_cstr("{\"kind\":\"test\"}"),
                                                .source = sc_str_from_cstr("tools"),
                                            }),
                              SC_OK);
    failures += sc_test_expect_status("rules get",
                              sc_memory_get(memory, sc_str_from_cstr("rules"), sc_str_from_cstr("alpha"), sc_allocator_heap(), &value),
                              SC_OK);
    failures += sc_test_expect_true("rules get value", strcmp(value.ptr, "visible-content") == 0);
    sc_string_clear(&value);

    failures += sc_test_expect_status("rules default export", sc_memory_export_snapshot(memory, &query, sc_allocator_heap(), &snapshot), SC_OK);
    failures += sc_test_expect_true("rules export redacted", strstr(snapshot.ptr, "visible-content") == nullptr && strstr(snapshot.ptr, "[REDACTED]") != nullptr);
    sc_string_clear(&snapshot);
    failures += sc_test_expect_status("rules raw export",
                              sc_memory_export_snapshot_ex(memory, &query, &raw_export, sc_allocator_heap(), &snapshot),
                              SC_OK);
    failures += sc_test_expect_true("rules raw export content", strstr(snapshot.ptr, "visible-content") != nullptr);
    sc_string_clear(&snapshot);

    failures += sc_test_expect_status("rules redact", sc_memory_redact(memory, sc_str_from_cstr("rules"), sc_str_from_cstr("alpha")), SC_OK);
    failures += sc_test_expect_status("rules redacted get",
                              sc_memory_get(memory, sc_str_from_cstr("rules"), sc_str_from_cstr("alpha"), sc_allocator_heap(), &value),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("rules default search", sc_memory_search(memory, &query, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rules default excludes redacted", result.entries.len == 0);
    sc_memory_result_clear(&result);
    query.include_redacted = true;
    failures += sc_test_expect_status("rules redacted search", sc_memory_search(memory, &query, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rules redacted state",
                            result.entries.len == 1 &&
                                ((sc_memory_entry *)result.entries.ptr)[0].redaction_state == SC_MEMORY_REDACTION_REDACTED);
    sc_memory_result_clear(&result);
    query.include_redacted = false;

    failures += sc_test_expect_status("rules second put",
                              sc_memory_put(memory,
                                            &(sc_memory_record){
                                                .struct_size = sizeof(sc_memory_record),
                                                .namespace_name = sc_str_from_cstr("rules"),
                                                .key = sc_str_from_cstr("beta"),
                                                .value = sc_str_from_cstr("persisted"),
                                            }),
                              SC_OK);
    sc_memory_destroy(memory);
    memory = nullptr;
    failures += sc_test_expect_status("rules markdown reopen", sc_memory_markdown_open(sc_allocator_heap(), sc_string_as_str(&path), &memory), SC_OK);
    failures += sc_test_expect_status("rules reopen get",
                              sc_memory_get(memory, sc_str_from_cstr("rules"), sc_str_from_cstr("beta"), sc_allocator_heap(), &value),
                              SC_OK);
    failures += sc_test_expect_true("rules reopen value", strcmp(value.ptr, "persisted") == 0);
    sc_string_clear(&value);

    failures += sc_test_expect_status("rules invalid utf8",
                              sc_memory_put(memory,
                                            &(sc_memory_record){
                                                .struct_size = sizeof(sc_memory_record),
                                                .namespace_name = sc_str_from_cstr("rules"),
                                                .key = sc_str_from_cstr("bad-utf8"),
                                                .value = sc_str_from_parts(bad_utf8, 2),
                                            }),
                              SC_ERR_PARSE);
    failures += sc_test_expect_status("rules secret key",
                              sc_memory_put(memory,
                                            &(sc_memory_record){
                                                .struct_size = sizeof(sc_memory_record),
                                                .namespace_name = sc_str_from_cstr("rules"),
                                                .key = sc_str_from_cstr("api_token"),
                                                .value = sc_str_from_cstr("secret"),
                                            }),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("rules malformed import",
                              sc_memory_import_snapshot(memory, sc_str_from_cstr("{"), nullptr),
                              SC_ERR_PARSE);
    failures += sc_test_expect_status("rules embedding unsupported",
                              sc_memory_validate_embedding(vtab,
                                                           &(sc_memory_embedding_spec){
                                                               .struct_size = sizeof(sc_memory_embedding_spec),
                                                               .model_id = sc_str_from_cstr("m"),
                                                               .dimensions = 3,
                                                               .batch_size = 1,
                                                           },
                                                           &(sc_memory_embedding_spec){
                                                               .struct_size = sizeof(sc_memory_embedding_spec),
                                                               .model_id = sc_str_from_cstr("m"),
                                                               .dimensions = 3,
                                                               .batch_size = 1,
                                                           }),
                              SC_ERR_UNSUPPORTED);
    sc_memory_vtab embedding_vtab = *vtab;
    embedding_vtab.memory_capabilities |= SC_MEMORY_BACKEND_CAP_EMBEDDINGS;
    failures += sc_test_expect_status("rules embedding mismatch",
                              sc_memory_validate_embedding(&embedding_vtab,
                                                           &(sc_memory_embedding_spec){
                                                               .struct_size = sizeof(sc_memory_embedding_spec),
                                                               .model_id = sc_str_from_cstr("m"),
                                                               .dimensions = 3,
                                                               .batch_size = 2,
                                                           },
                                                           &(sc_memory_embedding_spec){
                                                               .struct_size = sizeof(sc_memory_embedding_spec),
                                                               .model_id = sc_str_from_cstr("m"),
                                                               .dimensions = 4,
                                                               .batch_size = 1,
                                                           }),
                              SC_ERR_INVALID_ARGUMENT);

    failures += sc_test_expect_status("rules sqlite path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("memory.sqlite"), &sqlite_path),
                              SC_OK);
    sqlite_status = sc_memory_sqlite_open(sc_allocator_heap(), sc_string_as_str(&sqlite_path), &sqlite);
    if (sqlite_status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&sqlite_status);
    } else {
        failures += sc_test_expect_status("rules sqlite open", sqlite_status, SC_OK);
        vtab = sc_memory_vtab_of(sqlite);
        failures += sc_test_expect_true("rules sqlite descriptor",
                                vtab != nullptr &&
                                    vtab->persistence_type == SC_MEMORY_PERSISTENCE_SQLITE &&
                                    vtab->supports_migrations &&
                                    vtab->migration_version >= 2);
        failures += sc_test_expect_status("rules sqlite put",
                                  sc_memory_put(sqlite,
                                                &(sc_memory_record){
                                                    .struct_size = sizeof(sc_memory_record),
                                                    .namespace_name = sc_str_from_cstr("rules"),
                                                    .key = sc_str_from_cstr("sqlite"),
                                                    .value = sc_str_from_cstr("value"),
                                                }),
                                  SC_OK);
        failures += sc_test_expect_status("rules sqlite forget",
                                  sc_memory_forget(sqlite, sc_str_from_cstr("rules"), sc_str_from_cstr("sqlite")),
                                  SC_OK);
        failures += sc_test_expect_status("rules sqlite forgotten get",
                                  sc_memory_get(sqlite,
                                                sc_str_from_cstr("rules"),
                                                sc_str_from_cstr("sqlite"),
                                                sc_allocator_heap(),
                                                &value),
                                  SC_ERR_INVALID_ARGUMENT);
    }

    sc_memory_destroy(sqlite);
    sc_memory_destroy(memory);
    sc_memory_result_clear(&result);
    sc_string_clear(&snapshot);
    sc_string_clear(&value);
    sc_string_clear(&sqlite_path);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_shell_disabled(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context context = {0};
    sc_tool *shell = nullptr;
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;

    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy, .receipts = &receipts};
    failures += sc_test_expect_status("shell new", sc_tool_shell_new(sc_allocator_heap(), &context, &shell), SC_OK);
    failures += sc_test_expect_status("parse shell", parse_args("{\"command\":\"echo nope\"}", &args), SC_OK);
    failures += sc_test_expect_status("shell denied", sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("shell receipt", receipts.receipts.len == 1 && sc_receipt_chain_verify(&receipts));

    sc_json_destroy(args);
    sc_tool_destroy(shell);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    return failures;
}

static int test_shell_rtk_compression(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_config config = {0};
    sc_tool_context context = {0};
    sc_tool *shell = nullptr;
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;
    sc_string dir = {0};
    sc_string rtk = {0};
    sc_string input = {0};
    char command_json[1024] = {0};
    int written = 0;

    failures += sc_test_expect_status("rtk temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("rtk script path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("rtk-fake.sh"), &rtk), SC_OK);
    failures += sc_test_expect_status("rtk input path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("input.txt"), &input), SC_OK);
    failures += sc_test_expect_true("write rtk script",
                            sc_test_write_file(sc_string_as_str(&rtk),
                                       "#!/bin/sh\n"
                                       "printf 'rtk wrapped:'\n"
                                       "for arg do printf '[%s]' \"$arg\"; done\n"
                                       "printf '\\n'\n") == 0);
    failures += sc_test_expect_true("chmod rtk script", chmod(rtk.ptr, 0700) == 0);
    failures += sc_test_expect_true("write rtk input", sc_test_write_file(sc_string_as_str(&input), "raw-output") == 0);
    failures += sc_test_expect_status("rtk config defaults", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("rtk config enabled", sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.enabled"), sc_str_from_cstr("true")), SC_OK);
    failures += sc_test_expect_status("rtk config command", sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.command"), sc_string_as_str(&rtk)), SC_OK);
    failures += sc_test_expect_status("rtk policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("rtk workspace", sc_security_policy_set_workspace(&policy, sc_string_as_str(&dir)), SC_OK);
    failures += sc_test_expect_status("rtk auto approve shell", sc_security_policy_add_auto_approved_tool(&policy, sc_str_from_cstr("shell")), SC_OK);
    policy.shell_enabled = true;
    policy.sandbox_backend = SC_SANDBOX_BACKEND_NOOP;
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){
        .struct_size = sizeof(context),
        .policy = &policy,
        .receipts = &receipts,
        .config = &config,
        .max_output_bytes = 4096,
    };
    failures += sc_test_expect_status("rtk shell new", sc_tool_shell_new(sc_allocator_heap(), &context, &shell), SC_OK);

    failures += sc_test_expect_status("rtk parse wrapped", parse_args("{\"command\":\"git status\"}", &args), SC_OK);
    failures += sc_test_expect_status("rtk invoke wrapped", sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rtk wrapped output",
                            result.output.ptr != nullptr &&
                                strstr(result.output.ptr, "rtk wrapped:[-u][git][status]") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("rtk disable", sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.enabled"), sc_str_from_cstr("false")), SC_OK);
    failures += sc_test_expect_status("rtk parse disabled", parse_args("{\"command\":\"echo disabled\"}", &args), SC_OK);
    failures += sc_test_expect_status("rtk invoke disabled", sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rtk disabled raw", result.output.ptr != nullptr && strstr(result.output.ptr, "disabled") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("rtk re-enable", sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.enabled"), sc_str_from_cstr("true")), SC_OK);
    failures += sc_test_expect_status("rtk missing command", sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.command"), sc_str_from_cstr("/tmp/sc-missing-rtk")), SC_OK);
    written = snprintf(command_json, sizeof(command_json), "{\"command\":\"cat %s\"}", input.ptr);
    failures += sc_test_expect_true("rtk passthrough args format", written > 0 && (size_t)written < sizeof(command_json));
    failures += sc_test_expect_status("rtk parse passthrough", parse_args(command_json, &args), SC_OK);
    failures += sc_test_expect_status("rtk missing passthrough", sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rtk passthrough raw", result.output.ptr != nullptr && strstr(result.output.ptr, "raw-output") != nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("rtk no fallback",
                              sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.fallback_passthrough"), sc_str_from_cstr("false")),
                              SC_OK);
    failures += sc_test_expect_status("rtk parse unsupported", parse_args(command_json, &args), SC_OK);
    failures += sc_test_expect_status("rtk missing unsupported",
                              sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result),
                              SC_ERR_UNSUPPORTED);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("rtk restore fallback",
                              sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.fallback_passthrough"), sc_str_from_cstr("true")),
                              SC_OK);
    failures += sc_test_expect_status("rtk restore command", sc_config_set_prop(&config, sc_str_from_cstr("tools.rtk.command"), sc_string_as_str(&rtk)), SC_OK);

    failures += sc_test_expect_status("rtk parse disallowed", parse_args("{\"command\":\"echo disallowed\"}", &args), SC_OK);
    failures += sc_test_expect_status("rtk invoke disallowed", sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rtk disallowed raw",
                            result.output.ptr != nullptr &&
                                strstr(result.output.ptr, "disallowed") != nullptr &&
                                strstr(result.output.ptr, "rtk wrapped") == nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    written = snprintf(command_json, sizeof(command_json), "{\"command\":\"cat %s | cat\"}", input.ptr);
    failures += sc_test_expect_true("rtk complex args format", written > 0 && (size_t)written < sizeof(command_json));
    failures += sc_test_expect_status("rtk parse complex", parse_args(command_json, &args), SC_OK);
    failures += sc_test_expect_status("rtk invoke complex", sc_tool_invoke(shell, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("rtk complex raw",
                            result.output.ptr != nullptr &&
                                strstr(result.output.ptr, "raw-output") != nullptr &&
                                strstr(result.output.ptr, "rtk wrapped") == nullptr);

    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    sc_tool_destroy(shell);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    sc_config_clear(&config);
    sc_string_clear(&input);
    sc_string_clear(&rtk);
    sc_string_clear(&dir);
    return failures;
}

static int test_mcp_server_tool_spec(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_tool_context context = {0};
    sc_tool *tool = nullptr;
    sc_tool_spec spec = {0};
    sc_json_value *specs = nullptr;
    sc_string specs_json = {0};
    sc_tool *tools[1] = {0};

    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy};
    failures += sc_test_expect_status("mcp tool new",
                              sc_tool_mcp_server_new(sc_allocator_heap(),
                                                     &context,
                                                     sc_str_from_cstr("localfs"),
                                                     sc_str_from_cstr("stdio"),
                                                     sc_str_from_cstr("node"),
                                                     sc_str_from_cstr("[]"),
                                                     sc_str_from_cstr(""),
                                                     sc_str_from_cstr(""),
                                                     &tool),
                              SC_OK);
    failures += sc_test_expect_status("mcp tool spec", sc_tool_spec_get(tool, &spec), SC_OK);
    failures += sc_test_expect_true("mcp tool name", sc_str_equal(spec.name, sc_str_from_cstr("localfs__mcp_call")));
    failures += sc_test_expect_true("mcp tool risk", spec.risk == SC_TOOL_RISK_SIDE_EFFECT);
    tools[0] = tool;
    failures += sc_test_expect_status("mcp model specs",
                              sc_tool_registry_model_specs_from_tools(tools, SC_ARRAY_LEN(tools), sc_allocator_heap(), &specs),
                              SC_OK);
    failures += sc_test_expect_status("mcp serialize specs", sc_json_serialize(specs, sc_allocator_heap(), &specs_json), SC_OK);
    failures += sc_test_expect_true("mcp specs visible", strstr(specs_json.ptr, "localfs__mcp_call") != nullptr);

    sc_string_clear(&specs_json);
    sc_json_destroy(specs);
    sc_tool_destroy(tool);
    sc_security_policy_clear(&policy);
    return failures;
}

static int test_mcp_discover_tools(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_tool_context context = {0};
    sc_tool **tools = nullptr;
    size_t tool_count = 0;
    sc_tool_spec spec = {0};
    sc_string dir = {0};
    sc_string script = {0};
    sc_string argv_json = {0};

    failures += sc_test_expect_status("discover temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("discover script path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("mcp-discover.sh"), &script),
                              SC_OK);
    failures += sc_test_expect_true("write discover script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "while IFS= read -r line; do :; done\n"
                                       "body='{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"tools\":[{\"name\":\"read_file\",\"description\":\"Read file\",\"inputSchema\":{\"type\":\"object\"}}]}}'\n"
                                       "printf 'Content-Length: %s\\r\\n\\r\\n%s' \"${#body}\" \"$body\"\n") == 0);
    if (failures == 0) {
        sc_string_builder builder = {0};
        sc_string_builder_init(&builder, sc_allocator_heap());
        failures += sc_test_expect_status("discover argv prefix", sc_string_builder_append_cstr(&builder, "[\""), SC_OK);
        failures += sc_test_expect_status("discover argv script", sc_string_builder_append(&builder, sc_string_as_str(&script)), SC_OK);
        failures += sc_test_expect_status("discover argv suffix", sc_string_builder_append_cstr(&builder, "\"]"), SC_OK);
        failures += sc_test_expect_status("discover argv finish", sc_string_builder_finish(&builder, &argv_json), SC_OK);
    }
    failures += sc_test_expect_status("discover policy", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.autonomy = SC_AUTONOMY_AUTONOMOUS;
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy};
    failures += sc_test_expect_status("mcp discover",
                              sc_tool_mcp_server_discover(sc_allocator_heap(),
                                                          &context,
                                                          sc_str_from_cstr("localfs"),
                                                          sc_str_from_cstr("stdio"),
                                                          sc_str_from_cstr("/bin/sh"),
                                                          sc_string_as_str(&argv_json),
                                                          sc_str_from_cstr(""),
                                                          sc_str_from_cstr(""),
                                                          &tools,
                                                          &tool_count),
                              SC_OK);
    failures += sc_test_expect_true("mcp discover count", tool_count == 1 && tools != nullptr);
    if (tool_count == 1 && tools != nullptr) {
        failures += sc_test_expect_status("mcp discover spec", sc_tool_spec_get(tools[0], &spec), SC_OK);
        failures += sc_test_expect_true("mcp discover name", sc_str_equal(spec.name, sc_str_from_cstr("localfs__read_file")));
    }
    for (size_t i = 0; i < tool_count; i += 1) {
        sc_tool_destroy(tools[i]);
    }
    sc_free(sc_allocator_heap(), tools, tool_count * sizeof(*tools), _Alignof(sc_tool *));
    sc_security_policy_clear(&policy);
    sc_string_clear(&argv_json);
    sc_string_clear(&script);
    sc_string_clear(&dir);
    return failures;
}

static int test_mcp_stdio_invoke(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_tool_context context = {0};
    sc_tool *tool = nullptr;
    sc_json_value *args = nullptr;
    sc_tool_result result = {0};
    sc_string dir = {0};
    sc_string script = {0};
    sc_string argv_json = {0};
    char invoke_json[1024] = {0};
    int written = 0;

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("mcp script path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("mcp.sh"), &script), SC_OK);
    failures += sc_test_expect_true("write mcp script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "while IFS= read -r line; do :; done\n"
                                       "body='{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}'\n"
                                       "printf 'Content-Length: %s\\r\\n\\r\\n%s' \"${#body}\" \"$body\"\n") == 0);
    if (failures == 0) {
        sc_string_builder builder = {0};
        sc_string_builder_init(&builder, sc_allocator_heap());
        failures += sc_test_expect_status("mcp argv append prefix",
                                  sc_string_builder_append_cstr(&builder, "[\""),
                                  SC_OK);
        failures += sc_test_expect_status("mcp argv append script",
                                  sc_string_builder_append(&builder, sc_string_as_str(&script)),
                                  SC_OK);
        failures += sc_test_expect_status("mcp argv append suffix",
                                  sc_string_builder_append_cstr(&builder, "\"]"),
                                  SC_OK);
        failures += sc_test_expect_status("mcp argv finish", sc_string_builder_finish(&builder, &argv_json), SC_OK);
    }
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.autonomy = SC_AUTONOMY_AUTONOMOUS;
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy};
    failures += sc_test_expect_status("mcp stdio new",
                              sc_tool_mcp_server_new(sc_allocator_heap(),
                                                     &context,
                                                     sc_str_from_cstr("localfs"),
                                                     sc_str_from_cstr("stdio"),
                                                     sc_str_from_cstr("/bin/sh"),
                                                     sc_string_as_str(&argv_json),
                                                     sc_str_from_cstr(""),
                                                     sc_str_from_cstr(""),
                                                     &tool),
                              SC_OK);
    written = snprintf(invoke_json,
                       sizeof(invoke_json),
                       "{\"tool\":\"ping\",\"arguments_json\":\"{\\\"value\\\":1}\"}");
    failures += sc_test_expect_true("mcp invoke args format", written > 0 && (size_t)written < sizeof(invoke_json));
    failures += sc_test_expect_status("mcp parse invoke args", parse_args(invoke_json, &args), SC_OK);
    failures += sc_test_expect_status("mcp stdio invoke", sc_tool_invoke(tool, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("mcp stdio output", strstr(result.output.ptr, "\"id\":2") != nullptr && strstr(result.output.ptr, "\"ok\"") != nullptr);

    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    sc_tool_destroy(tool);
    sc_security_policy_clear(&policy);
    sc_string_clear(&argv_json);
    sc_string_clear(&script);
    sc_string_clear(&dir);
    return failures;
}

static int test_claude_code_provider_mcp(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_provider_response response = {0};
    sc_string dir = {0};
    sc_string script = {0};
    sc_string argv_json = {0};
    sc_provider_options options = {0};

    failures += sc_test_expect_status("claude mcp temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("claude mcp script path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("claude-mcp.sh"), &script),
                              SC_OK);
    failures += sc_test_expect_true("write claude mcp script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "input=$(cat)\n"
                                       "case \"$input\" in\n"
                                       "  *'\"prompt\":\"hello\"'*) body='{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"claude reply\"}]}}' ;;\n"
                                       "  *) body='{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"missing prompt\"}]}}' ;;\n"
                                       "esac\n"
                                       "printf 'Content-Length: %s\\r\\n\\r\\n%s' \"${#body}\" \"$body\"\n") == 0);
    failures += sc_test_expect_status("claude mcp argv", argv_for_script(sc_string_as_str(&script), &argv_json), SC_OK);
    options = (sc_provider_options){
        .struct_size = sizeof(options),
        .provider_name = sc_str_from_cstr("cc"),
        .mcp_tool = sc_str_from_cstr("query"),
        .mcp_prompt_field = sc_str_from_cstr("prompt"),
        .mcp_transport = sc_str_from_cstr("stdio"),
        .command = sc_str_from_cstr("/bin/sh"),
        .mcp_args = sc_string_as_str(&argv_json),
    };
    failures += sc_test_expect_status("claude mcp provider new", sc_provider_claude_code_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("claude mcp generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_OK);
    failures += sc_test_expect_true("claude mcp response", strcmp(response.text.ptr, "claude reply") == 0);
    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write malformed claude mcp script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "while IFS= read -r line; do :; done\n"
                                       "body='not-json'\n"
                                       "printf 'Content-Length: %s\\r\\n\\r\\n%s' \"${#body}\" \"$body\"\n") == 0);
    failures += sc_test_expect_status("claude malformed provider new", sc_provider_claude_code_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("claude malformed generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("claude malformed untouched", response.struct_size == 0 && response.text.ptr == nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write oversized claude mcp script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "while IFS= read -r line; do :; done\n"
                                       "body_len=1049000\n"
                                       "printf 'Content-Length: %s\\r\\n\\r\\n' \"$body_len\"\n"
                                       "dd if=/dev/zero bs=$body_len count=1 2>/dev/null | tr '\\000' x\n") == 0);
    failures += sc_test_expect_status("claude oversized provider new", sc_provider_claude_code_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("claude oversized generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_IO);
    failures += sc_test_expect_true("claude oversized untouched", response.struct_size == 0 && response.text.ptr == nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write timeout claude mcp script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "while IFS= read -r line; do :; done\n"
                                       "sleep 1\n") == 0);
    options.timeout_ms = 10;
    failures += sc_test_expect_status("claude timeout provider new", sc_provider_claude_code_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("claude timeout generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_TIMEOUT);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("claude missing mcp provider", sc_provider_claude_code_new(sc_allocator_heap(), nullptr, &provider), SC_OK);
    failures += sc_test_expect_status("claude missing mcp generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_UNSUPPORTED);

    sc_provider_destroy(provider);
    sc_string_clear(&argv_json);
    sc_string_clear(&script);
    sc_string_clear(&dir);
    return failures;
}

static int test_gemini_cli_provider(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_provider_response response = {0};
    provider_stream_capture stream = {0};
    sc_string dir = {0};
    sc_string script = {0};
    sc_string args_path = {0};
    sc_provider_options options = {0};
    char script_text[2048] = {0};
    char args_text[1024] = {0};
    sc_media_attachment media = {.struct_size = sizeof(media)};

    failures += sc_test_expect_status("gemini cli temp dir", sc_test_make_temp_dir("tools", &dir), SC_OK);
    failures += sc_test_expect_status("gemini cli script path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("gemini-fake.sh"), &script),
                              SC_OK);
    failures += sc_test_expect_status("gemini cli args path",
                              sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("args.txt"), &args_path),
                              SC_OK);
    (void)snprintf(script_text,
                   sizeof(script_text),
                   "#!/bin/sh\n"
                   "printf '%%s\\n' \"$*\" > '%s'\n"
                   "printf '{\"response\":\"gemini reply\",\"stats\":{\"models\":{\"gemini-2.5-flash\":{\"tokens\":{\"prompt\":3,\"candidates\":4,\"total\":7}}}}}'\n",
                   args_path.ptr);
    failures += sc_test_expect_true("write gemini cli script", sc_test_write_file(sc_string_as_str(&script), script_text) == 0);
    failures += sc_test_expect_true("chmod gemini cli script", chmod(script.ptr, 0700) == 0);

    options = (sc_provider_options){
        .struct_size = sizeof(options),
        .provider_name = sc_str_from_cstr("gemini-cli"),
        .default_model = sc_str_from_cstr("gemini-2.5-flash"),
        .command = sc_string_as_str(&script),
    };
    failures += sc_test_expect_status("gemini cli provider new", sc_provider_gemini_cli_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("gemini cli generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello"),
                                                                          .system_instruction = sc_str_from_cstr("be direct"),
                                                                          .media_context = sc_str_from_cstr("text context")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_OK);
    failures += sc_test_expect_true("gemini cli response", strcmp(response.text.ptr, "gemini reply") == 0);
    failures += sc_test_expect_true("gemini cli usage",
                            response.input_tokens == 3 && response.output_tokens == 4 && response.total_tokens == 7);
    failures += sc_test_expect_true("gemini cli model", strcmp(response.model.ptr, "gemini-2.5-flash") == 0);
    failures += sc_test_expect_true("read gemini cli args", read_file_text(sc_string_as_str(&args_path), args_text, sizeof(args_text)) == 0);
    failures += sc_test_expect_true("gemini cli prompt arg", strstr(args_text, "--prompt") != nullptr);
    failures += sc_test_expect_true("gemini cli output format arg", strstr(args_text, "--output-format json") != nullptr);
    failures += sc_test_expect_true("gemini cli model arg", strstr(args_text, "--model gemini-2.5-flash") != nullptr);
    failures += sc_test_expect_true("gemini cli no yolo", strstr(args_text, "yolo") == nullptr && strstr(args_text, "approval") == nullptr);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("gemini cli request model",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .model = sc_str_from_cstr("gemini-test-model"),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_OK);
    failures += sc_test_expect_true("read gemini cli request model args",
                            read_file_text(sc_string_as_str(&args_path), args_text, sizeof(args_text)) == 0);
    failures += sc_test_expect_true("gemini cli request model arg", strstr(args_text, "--model gemini-test-model") != nullptr);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("gemini cli stream",
                              sc_provider_stream(provider,
                                                 &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                        .prompt = sc_str_from_cstr("hello")},
                                                 sc_allocator_heap(),
                                                 capture_provider_stream,
                                                 &stream),
                              SC_OK);
    failures += sc_test_expect_true("gemini cli stream events", stream.deltas == 1 && stream.done == 1);
    failures += sc_test_expect_true("gemini cli stream text", strcmp(stream.text.ptr, "gemini reply") == 0);
    provider_stream_capture_clear(&stream);

    failures += sc_test_expect_status("gemini cli tools unsupported",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello"),
                                                                          .tool_specs_json = sc_str_from_cstr("[]")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_true("gemini cli tools untouched", response.struct_size == 0 && response.text.ptr == nullptr);
    failures += sc_test_expect_status("gemini cli media unsupported",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello"),
                                                                          .media = &media,
                                                                          .media_count = 1},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_true("gemini cli media untouched", response.struct_size == 0 && response.text.ptr == nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write malformed gemini cli script",
                            sc_test_write_file(sc_string_as_str(&script), "#!/bin/sh\nprintf 'not-json'\n") == 0);
    failures += sc_test_expect_true("chmod malformed gemini cli script", chmod(script.ptr, 0700) == 0);
    failures += sc_test_expect_status("gemini malformed provider new", sc_provider_gemini_cli_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("gemini malformed generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("gemini malformed untouched", response.struct_size == 0 && response.text.ptr == nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write empty response gemini cli script",
                            sc_test_write_file(sc_string_as_str(&script), "#!/bin/sh\nprintf '{\"response\":\"\"}'\n") == 0);
    failures += sc_test_expect_true("chmod empty response gemini cli script", chmod(script.ptr, 0700) == 0);
    failures += sc_test_expect_status("gemini empty provider new", sc_provider_gemini_cli_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("gemini empty generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("gemini empty untouched", response.struct_size == 0 && response.text.ptr == nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write oversized gemini cli script",
                            sc_test_write_file(sc_string_as_str(&script),
                                       "#!/bin/sh\n"
                                       "head -c 1049000 /dev/zero | tr '\\000' x\n") == 0);
    failures += sc_test_expect_true("chmod oversized gemini cli script", chmod(script.ptr, 0700) == 0);
    failures += sc_test_expect_status("gemini oversized provider new", sc_provider_gemini_cli_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("gemini oversized generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_IO);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_true("write timeout gemini cli script", sc_test_write_file(sc_string_as_str(&script), "#!/bin/sh\nsleep 1\n") == 0);
    failures += sc_test_expect_true("chmod timeout gemini cli script", chmod(script.ptr, 0700) == 0);
    options.timeout_ms = 10;
    failures += sc_test_expect_status("gemini timeout provider new", sc_provider_gemini_cli_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_status("gemini timeout generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hello")},
                                                   sc_allocator_heap(),
                                                   &response),
                              SC_ERR_TIMEOUT);
    sc_provider_destroy(provider);

    sc_string_clear(&args_path);
    sc_string_clear(&script);
    sc_string_clear(&dir);
    return failures;
}

static int test_tool_wrappers(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_tool *inner = nullptr;
    sc_tool *wrapped = nullptr;
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;

    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("allowed domain", sc_security_policy_add_allowed_domain(&policy, sc_str_from_cstr("example.com")), SC_OK);
    failures += sc_test_expect_status("redirect recheck",
                              sc_security_validate_redirect(&policy,
                                                            sc_str_from_cstr("https://example.com/a"),
                                                            sc_str_from_cstr("http://127.0.0.1/private")),
                              SC_ERR_SECURITY_DENIED);

    failures += sc_test_expect_status("rate inner", sc_tool_new(sc_allocator_heap(), &url_tool_vtab, nullptr, &inner), SC_OK);
    failures += sc_test_expect_status("rate wrapper", sc_tool_rate_limit_wrapper_new(sc_allocator_heap(), inner, 1, &wrapped), SC_OK);
    inner = nullptr;
    failures += sc_test_expect_status("parse url args", parse_args("{\"url\":\"https://example.com/ok\"}", &args), SC_OK);
    failures += sc_test_expect_status("rate first", sc_tool_invoke(wrapped, MAKE_CALL(args), sc_allocator_heap(), &result), SC_OK);
    sc_tool_result_clear(&result);
    failures += sc_test_expect_status("rate second", sc_tool_invoke(wrapped, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_TIMEOUT);
    sc_json_destroy(args);
    sc_tool_destroy(wrapped);
    args = nullptr;
    wrapped = nullptr;

    failures += sc_test_expect_status("domain inner", sc_tool_new(sc_allocator_heap(), &url_tool_vtab, nullptr, &inner), SC_OK);
    failures += sc_test_expect_status("domain wrapper", sc_tool_domain_guard_wrapper_new(sc_allocator_heap(), inner, &policy, &wrapped), SC_OK);
    inner = nullptr;
    failures += sc_test_expect_status("parse denied url", parse_args("{\"url\":\"http://127.0.0.1/private\"}", &args), SC_OK);
    failures += sc_test_expect_status("domain denied", sc_tool_invoke(wrapped, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    sc_json_destroy(args);
    sc_tool_destroy(wrapped);
    args = nullptr;
    wrapped = nullptr;

    failures += sc_test_expect_status("timeout inner", sc_tool_new(sc_allocator_heap(), &url_tool_vtab, nullptr, &inner), SC_OK);
    failures += sc_test_expect_status("timeout wrapper", sc_tool_timeout_wrapper_new(sc_allocator_heap(), inner, 0, &wrapped), SC_OK);
    inner = nullptr;
    failures += sc_test_expect_status("parse timeout url", parse_args("{\"url\":\"https://example.com/ok\"}", &args), SC_OK);
    failures += sc_test_expect_status("timeout denied", sc_tool_invoke(wrapped, MAKE_CALL(args), sc_allocator_heap(), &result), SC_ERR_TIMEOUT);

    sc_json_destroy(args);
    sc_tool_destroy(wrapped);
    sc_security_policy_clear(&policy);
    return failures;
}

static sc_status url_tool_spec(void *impl, sc_tool_spec *out)
{
    (void)impl;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.test.url_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("url_probe"),
        .description = sc_str_from_cstr("tool.url_probe.description"),
        .risk = SC_TOOL_RISK_NETWORK,
    };
    return sc_status_ok();
}

static sc_status url_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    (void)impl;
    (void)call;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.test.url_tool.invalid_argument");
    }
    sc_tool_result_clear(out);
    *out = (sc_tool_result){.struct_size = sizeof(*out), .success = true};
    return sc_string_from_cstr(alloc, "ok", &out->output);
}

static void url_tool_destroy(void *impl)
{
    (void)impl;
}

static sc_status argv_for_script(sc_str script, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, sc_allocator_heap());
    status = sc_string_builder_append_cstr(&builder, "[\"");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, script);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\"]");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static int read_file_text(sc_str path, char *buffer, size_t buffer_size)
{
    FILE *file = nullptr;
    size_t count = 0;

    if (path.ptr == nullptr || buffer == nullptr || buffer_size == 0) {
        return -1;
    }
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        return -1;
    }
    count = fread(buffer, 1, buffer_size - 1u, file);
    buffer[count] = '\0';
    if (ferror(file)) {
        (void)fclose(file);
        return -1;
    }
    return fclose(file);
}

static sc_status parse_args(const char *json, sc_json_value **out)
{
    sc_json_parse_error error = {0};
    return sc_json_parse(sc_allocator_heap(), sc_str_from_cstr(json), out, &error);
}

static sc_status capture_provider_stream(void *user_data, const sc_provider_stream_event *event)
{
    provider_stream_capture *capture = user_data;

    if (capture == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.test.stream_invalid_argument");
    }
    if (event->type == SC_PROVIDER_STREAM_DELTA) {
        capture->deltas += 1;
        sc_string_clear(&capture->text);
        return sc_string_from_str(sc_allocator_heap(), sc_string_as_str(&event->text), &capture->text);
    }
    if (event->type == SC_PROVIDER_STREAM_DONE) {
        capture->done += 1;
    }
    return sc_status_ok();
}

static void provider_stream_capture_clear(provider_stream_capture *capture)
{
    if (capture == nullptr) {
        return;
    }
    sc_string_clear(&capture->text);
    *capture = (provider_stream_capture){0};
}

static void tool_async_capture_complete(void *user_data, const sc_tool_result *result, sc_status status)
{
    tool_async_capture *capture = user_data;

    if (capture == nullptr) {
        sc_status_clear(&status);
        return;
    }
    capture->calls += 1;
    sc_status_clear(&capture->status);
    capture->status = status;
    capture->success = result != nullptr && result->success;
    sc_string_clear(&capture->output);
    if (result != nullptr && result->output.ptr != nullptr) {
        sc_status copy_status = sc_string_from_str(sc_allocator_heap(), sc_string_as_str(&result->output), &capture->output);
        if (!sc_status_is_ok(copy_status) && sc_status_is_ok(capture->status)) {
            capture->status = copy_status;
        } else {
            sc_status_clear(&copy_status);
        }
    }
}

static void tool_async_capture_clear(tool_async_capture *capture)
{
    if (capture == nullptr) {
        return;
    }
    sc_status_clear(&capture->status);
    sc_string_clear(&capture->output);
    *capture = (tool_async_capture){0};
}
