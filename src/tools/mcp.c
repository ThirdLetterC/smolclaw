// cppcheck-suppress-file redundantInitialization
#include "tools/tool_internal.h"

#include "tools/mcp_client_internal.h"

#include "net/curl_global.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

typedef struct mcp_server_tool {
    sc_tool_impl_context base;
    sc_string server_name;
    sc_string transport;
    sc_string tool_name;
    sc_string target_tool_name;
    sc_string command;
    sc_string args;
    sc_string url;
    sc_string headers;
} mcp_server_tool;

#ifdef SC_HAVE_LIBCURL
typedef struct mcp_http_response {
    sc_bytes bytes;
    size_t max_bytes;
    bool too_large;
} mcp_http_response;
#endif

static sc_status mcp_server_spec(void *impl, sc_tool_spec *out);
static sc_status mcp_server_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void mcp_server_destroy(void *impl);
static void mcp_server_clear_contents(mcp_server_tool *tool);
static sc_status build_mcp_tool_name(sc_allocator *alloc, sc_str server_name, sc_string *out);
static sc_status build_mcp_prefixed_tool_name(sc_allocator *alloc, sc_str server_name, sc_str tool_name, sc_string *out);
static sc_status build_tools_call_json(sc_allocator *alloc, sc_str target_tool, sc_str args_json, int64_t id, sc_string *out);
static sc_status build_tools_list_json(sc_allocator *alloc, sc_string *out);
static sc_status build_initialize_json(sc_allocator *alloc, sc_string *out);
static sc_status append_framed_json(sc_string_builder *builder, sc_str body);
static sc_status mcp_request_stdio(mcp_server_tool *tool, sc_allocator *alloc, sc_str request_json, int64_t wanted_id, sc_string *out);
static sc_status mcp_request_http(mcp_server_tool *tool, sc_allocator *alloc, sc_str request_json, sc_string *out);
static sc_status mcp_call_stdio(mcp_server_tool *tool, sc_allocator *alloc, sc_str target_tool, sc_str args_json, sc_string *out);
static sc_status mcp_call_http(mcp_server_tool *tool, sc_allocator *alloc, sc_str target_tool, sc_str args_json, sc_string *out);
static sc_status mcp_discover_names(mcp_server_tool *tool, sc_allocator *alloc, sc_vec *names);
static sc_status mcp_names_from_response(sc_allocator *alloc, sc_str response, sc_vec *names);
static sc_status build_stdio_argv(sc_allocator *alloc, sc_str command, sc_str args_json, sc_json_value **args_root, char ***out_argv, size_t *out_count);
static sc_status write_all(int fd, sc_str bytes);
static sc_status read_child_output(sc_allocator *alloc, int fd, pid_t child, size_t max_bytes, int64_t timeout_ms, sc_bytes *out);
static sc_status extract_mcp_response(sc_allocator *alloc, sc_str stream, int64_t wanted_id, sc_string *out);
static sc_status maybe_extract_sse_payload(sc_allocator *alloc, sc_str response, sc_string *out);
static sc_status set_string(sc_json_value *object, sc_str key, sc_str value);
static sc_status set_number(sc_json_value *object, sc_str key, double value);
static sc_status set_object(sc_json_value *object, sc_str key, sc_json_value *value);
static sc_tool_risk mcp_tool_risk(const mcp_server_tool *tool);
#ifdef SC_HAVE_LIBCURL
static size_t mcp_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
static sc_status add_header_list(sc_allocator *alloc, struct curl_slist **headers, sc_str raw_headers);
#endif

static const sc_tool_vtab mcp_server_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mcp_server",
    .display_name = "MCP server proxy",
    .feature_flag = "SC_TOOL_MCP",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = mcp_server_spec,
    .invoke = mcp_server_invoke,
    .destroy = mcp_server_destroy,
};

sc_status sc_mcp_client_call(sc_allocator *alloc,
                             const sc_mcp_client_options *options,
                             sc_str tool_name,
                             sc_str arguments_json,
                             sc_string *out)
{
    mcp_server_tool tool = {0};
    sc_status status = sc_status_ok();

    if (options == nullptr || out == nullptr || tool_name.ptr == nullptr || tool_name.len == 0) {
        return sc_status_invalid_argument("sc.mcp_client.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool.base.alloc = alloc;
    tool.base.context.max_output_bytes = options->max_output_bytes == 0 ? 4096 : options->max_output_bytes;
    tool.base.context.timeout_ms = options->timeout_ms;
    status = sc_string_from_str(alloc, options->transport, &tool.transport);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, options->command, &tool.command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, options->args, &tool.args);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, options->url, &tool.url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, options->headers, &tool.headers);
    }
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&tool.transport), sc_str_from_cstr("stdio"))) {
        status = mcp_call_stdio(&tool, alloc, tool_name, arguments_json, out);
    } else if (sc_status_is_ok(status) &&
               (sc_str_equal(sc_string_as_str(&tool.transport), sc_str_from_cstr("http")) ||
                sc_str_equal(sc_string_as_str(&tool.transport), sc_str_from_cstr("sse")))) {
        status = mcp_call_http(&tool, alloc, tool_name, arguments_json, out);
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.mcp_client.unknown_transport");
    }
    mcp_server_clear_contents(&tool);
    return status;
}

sc_status sc_tool_mcp_server_new(sc_allocator *alloc,
                                  const sc_tool_context *context,
                                  sc_str server_name,
                                  sc_str transport,
                                  sc_str command,
                                  sc_str args,
                                  sc_str url,
                                  sc_str headers,
                                  sc_tool **out)
{
    mcp_server_tool *tool = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || server_name.ptr == nullptr || server_name.len == 0) {
        return sc_status_invalid_argument("sc.mcp_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(mcp_server_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (mcp_server_tool){0};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, server_name, &tool->server_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, transport, &tool->transport);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, command, &tool->command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, args, &tool->args);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, url, &tool->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, headers, &tool->headers);
    }
    if (sc_status_is_ok(status)) {
        status = build_mcp_tool_name(alloc, server_name, &tool->tool_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_two_strings(alloc,
                                            sc_str_from_cstr("tool"),
                                            true,
                                            sc_str_from_cstr("arguments_json"),
                                            false,
                                            &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &mcp_server_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        mcp_server_destroy(tool);
    }
    return status;
}

sc_status sc_tool_mcp_server_tool_new(sc_allocator *alloc,
                                      const sc_tool_context *context,
                                      sc_str server_name,
                                      sc_str tool_name,
                                      sc_str transport,
                                      sc_str command,
                                      sc_str args,
                                      sc_str url,
                                      sc_str headers,
                                      sc_tool **out)
{
    mcp_server_tool *tool = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || server_name.ptr == nullptr || server_name.len == 0 || tool_name.ptr == nullptr || tool_name.len == 0) {
        return sc_status_invalid_argument("sc.mcp_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(mcp_server_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (mcp_server_tool){0};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, server_name, &tool->server_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, transport, &tool->transport);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, tool_name, &tool->target_tool_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, command, &tool->command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, args, &tool->args);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, url, &tool->url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, headers, &tool->headers);
    }
    if (sc_status_is_ok(status)) {
        status = build_mcp_prefixed_tool_name(alloc, server_name, tool_name, &tool->tool_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_string(alloc, sc_str_from_cstr("arguments_json"), false, &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &mcp_server_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        mcp_server_destroy(tool);
    }
    return status;
}

sc_status sc_tool_mcp_server_discover(sc_allocator *alloc,
                                      const sc_tool_context *context,
                                      sc_str server_name,
                                      sc_str transport,
                                      sc_str command,
                                      sc_str args,
                                      sc_str url,
                                      sc_str headers,
                                      sc_tool ***out_tools,
                                      size_t *out_count)
{
    mcp_server_tool probe = {0};
    sc_vec names = {0};
    sc_tool **tools = nullptr;
    sc_status status = sc_status_ok();

    if (out_tools == nullptr || out_count == nullptr || server_name.ptr == nullptr || server_name.len == 0) {
        return sc_status_invalid_argument("sc.mcp_tool.discover_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out_tools = nullptr;
    *out_count = 0;
    sc_vec_init(&names, alloc, sizeof(sc_string));
    status = sc_tool_context_copy(alloc, context, &probe.base);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, server_name, &probe.server_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, transport, &probe.transport);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, command, &probe.command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, args, &probe.args);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, url, &probe.url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, headers, &probe.headers);
    }
    if (sc_status_is_ok(status)) {
        status = mcp_discover_names(&probe, alloc, &names);
    }
    if (sc_status_is_ok(status) && names.len == 0) {
        status = sc_status_unsupported("sc.mcp_tool.no_discovered_tools");
    }
    if (sc_status_is_ok(status)) {
        tools = sc_alloc(alloc, names.len * sizeof(*tools), _Alignof(sc_tool *));
        if (tools == nullptr) {
            status = sc_status_no_memory();
        } else {
            memset(tools, 0, names.len * sizeof(*tools));
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < names.len; i += 1) {
        const sc_string *name = sc_vec_at_const(&names, i);
        status = sc_tool_mcp_server_tool_new(alloc,
                                             context,
                                             server_name,
                                             sc_string_as_str(name),
                                             transport,
                                             command,
                                             args,
                                             url,
                                             headers,
                                             &tools[i]);
    }
    if (sc_status_is_ok(status)) {
        *out_tools = tools;
        *out_count = names.len;
        tools = nullptr;
    }
    if (tools != nullptr) {
        for (size_t i = 0; i < names.len; i += 1) {
            sc_tool_destroy(tools[i]);
        }
        sc_free(alloc, tools, names.len * sizeof(*tools), _Alignof(sc_tool *));
    }
    for (size_t i = 0; i < names.len; i += 1) {
        sc_string *name = sc_vec_at(&names, i);
        sc_string_clear(name);
    }
    sc_vec_clear(&names);
    mcp_server_clear_contents(&probe);
    return status;
}

static sc_status mcp_server_spec(void *impl, sc_tool_spec *out)
{
    const mcp_server_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.mcp_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_string_as_str(&tool->tool_name),
        .description = sc_str_from_cstr("tool.mcp_server.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = mcp_tool_risk(tool),
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_MCP | (mcp_tool_risk(tool) == SC_TOOL_RISK_NETWORK ? SC_TOOL_CAPABILITY_NETWORK :
                                                                                                      SC_TOOL_CAPABILITY_PROCESS),
        .side_effect = mcp_tool_risk(tool) == SC_TOOL_RISK_NETWORK ? SC_TOOL_SIDE_EFFECT_NETWORK : SC_TOOL_SIDE_EFFECT_PROCESS,
        .default_autonomy = SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = sc_str_from_cstr("tool.mcp_server.catalog"),
    };
    return sc_status_ok();
}

static sc_status mcp_server_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    mcp_server_tool *tool = impl;
    sc_str target_tool = {0};
    sc_str args_json = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (tool == nullptr) {
        return sc_status_invalid_argument("sc.mcp_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status) && tool->target_tool_name.len == 0) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("tool"), &target_tool);
    } else if (sc_status_is_ok(status)) {
        target_tool = sc_string_as_str(&tool->target_tool_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_optional_string_arg(call, sc_str_from_cstr("arguments_json"), &args_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_string_as_str(&tool->tool_name),
                                        mcp_tool_risk(tool),
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_string_as_str(&tool->url),
                                        sc_string_as_str(&tool->command));
    }
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("stdio"))) {
        status = mcp_call_stdio(tool, alloc, target_tool, args_json, &response);
    } else if (sc_status_is_ok(status) &&
               (sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("http")) ||
                sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("sse")))) {
        status = mcp_call_http(tool, alloc, target_tool, args_json, &response);
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.mcp_tool.unknown_transport");
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&response), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_string_as_str(&tool->tool_name),
                                        target_tool,
                                        sc_status_is_ok(status) ? sc_string_as_str(&response) : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_string_clear(&response);
    return status;
}

static void mcp_server_destroy(void *impl)
{
    mcp_server_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    mcp_server_clear_contents(tool);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(mcp_server_tool));
}

static void mcp_server_clear_contents(mcp_server_tool *tool)
{
    if (tool == nullptr) {
        return;
    }
    sc_tool_impl_context_clear(&tool->base);
    sc_string_clear(&tool->server_name);
    sc_string_clear(&tool->transport);
    sc_string_clear(&tool->tool_name);
    sc_string_clear(&tool->target_tool_name);
    sc_string_clear(&tool->command);
    sc_string_clear(&tool->args);
    sc_string_clear(&tool->url);
    sc_string_secure_clear(&tool->headers);
    *tool = (mcp_server_tool){0};
}

static sc_status build_mcp_tool_name(sc_allocator *alloc, sc_str server_name, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, server_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "__mcp_call");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_mcp_prefixed_tool_name(sc_allocator *alloc, sc_str server_name, sc_str tool_name, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, server_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "__");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, tool_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_tools_call_json(sc_allocator *alloc, sc_str target_tool, sc_str args_json, int64_t id, sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *params = nullptr;
    sc_json_value *arguments = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    status = sc_json_object_new(alloc, &root);
    if (sc_status_is_ok(status)) {
        status = set_string(root, sc_str_from_cstr("jsonrpc"), sc_str_from_cstr("2.0"));
    }
    if (sc_status_is_ok(status)) {
        status = set_number(root, sc_str_from_cstr("id"), (double)id);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(root, sc_str_from_cstr("method"), sc_str_from_cstr("tools/call"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &params);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(params, sc_str_from_cstr("name"), target_tool);
    }
    if (sc_status_is_ok(status)) {
        if (args_json.ptr != nullptr && args_json.len > 0) {
            status = sc_json_parse(alloc, args_json, &arguments, &error);
            if (sc_status_is_ok(status) && sc_json_type_of(arguments) != SC_JSON_OBJECT) {
                status = sc_status_invalid_argument("sc.mcp_tool.arguments_not_object");
            }
        } else {
            status = sc_json_object_new(alloc, &arguments);
        }
    }
    if (sc_status_is_ok(status)) {
        status = set_object(params, sc_str_from_cstr("arguments"), arguments);
        arguments = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = set_object(root, sc_str_from_cstr("params"), params);
        params = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }
    sc_json_destroy(root);
    sc_json_destroy(params);
    sc_json_destroy(arguments);
    return status;
}

static sc_status build_initialize_json(sc_allocator *alloc, sc_string *out)
{
    return sc_string_from_cstr(alloc,
                               "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\","
                               "\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},"
                               "\"clientInfo\":{\"name\":\"smolclaw\",\"version\":\"0.1.0\"}}}",
                               out);
}

static sc_status build_tools_list_json(sc_allocator *alloc, sc_string *out)
{
    return sc_string_from_cstr(alloc,
                               "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{}}",
                               out);
}

static sc_status append_framed_json(sc_string_builder *builder, sc_str body)
{
    char header[64] = {0};
    int written = snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", body.len);
    sc_status status = sc_status_ok();

    if (written < 0 || (size_t)written >= sizeof(header)) {
        return sc_status_no_memory();
    }
    status = sc_string_builder_append(builder, sc_str_from_parts(header, (size_t)written));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, body);
    }
    return status;
}

static sc_status mcp_call_stdio(mcp_server_tool *tool, sc_allocator *alloc, sc_str target_tool, sc_str args_json, sc_string *out)
{
    sc_string request = {0};
    sc_status status = build_tools_call_json(alloc, target_tool, args_json, 2, &request);

    if (sc_status_is_ok(status)) {
        status = mcp_request_stdio(tool, alloc, sc_string_as_str(&request), 2, out);
    }
    sc_string_clear(&request);
    return status;
}

static sc_status mcp_request_stdio(mcp_server_tool *tool, sc_allocator *alloc, sc_str request_json, int64_t wanted_id, sc_string *out)
{
    int stdin_pipe[2] = {-1, -1};
    int stdout_pipe[2] = {-1, -1};
    pid_t child = -1;
    sc_json_value *args_root = nullptr;
    char **argv = nullptr;
    size_t argv_count = 0;
    sc_string init = {0};
    sc_string_builder input = {0};
    sc_bytes output = {0};
    sc_status status = sc_status_ok();

    if (tool->command.len == 0) {
        return sc_status_invalid_argument("sc.mcp_tool.missing_command");
    }
    status = build_stdio_argv(alloc, sc_string_as_str(&tool->command), sc_string_as_str(&tool->args), &args_root, &argv, &argv_count);
    if (sc_status_is_ok(status) && (pipe(stdin_pipe) != 0 || pipe(stdout_pipe) != 0)) {
        status = sc_status_io("sc.mcp_tool.pipe_failed");
    }
    if (sc_status_is_ok(status)) {
        child = fork();
        if (child < 0) {
            status = sc_status_io("sc.mcp_tool.fork_failed");
        } else if (child == 0) {
            (void)dup2(stdin_pipe[0], STDIN_FILENO);
            (void)dup2(stdout_pipe[1], STDOUT_FILENO);
            (void)close(stdin_pipe[0]);
            (void)close(stdin_pipe[1]);
            (void)close(stdout_pipe[0]);
            (void)close(stdout_pipe[1]);
            execvp(argv[0], argv);
            _exit(127);
        }
    }
    if (child > 0) {
        (void)close(stdin_pipe[0]);
        (void)close(stdout_pipe[1]);
        stdin_pipe[0] = -1;
        stdout_pipe[1] = -1;
    }
    if (sc_status_is_ok(status)) {
        status = build_initialize_json(alloc, &init);
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&input, alloc);
        status = append_framed_json(&input, sc_string_as_str(&init));
    }
    if (sc_status_is_ok(status)) {
        status = append_framed_json(&input, sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{}}"));
    }
    if (sc_status_is_ok(status)) {
        status = append_framed_json(&input, request_json);
    }
    if (sc_status_is_ok(status)) {
        status = write_all(stdin_pipe[1], sc_str_from_parts((const char *)input.bytes.ptr, input.bytes.len));
    }
    if (stdin_pipe[1] >= 0) {
        (void)close(stdin_pipe[1]);
        stdin_pipe[1] = -1;
    }
    if (sc_status_is_ok(status)) {
        status = read_child_output(alloc,
                                   stdout_pipe[0],
                                   child,
                                   tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes,
                                   tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms,
                                   &output);
    }
    if (sc_status_is_ok(status)) {
        status = extract_mcp_response(alloc, sc_str_from_parts((const char *)output.ptr, output.len), wanted_id, out);
    }

    if (stdin_pipe[0] >= 0) {
        (void)close(stdin_pipe[0]);
    }
    if (stdin_pipe[1] >= 0) {
        (void)close(stdin_pipe[1]);
    }
    if (stdout_pipe[0] >= 0) {
        (void)close(stdout_pipe[0]);
    }
    if (stdout_pipe[1] >= 0) {
        (void)close(stdout_pipe[1]);
    }
    if (child > 0 && !sc_status_is_ok(status)) {
        (void)kill(child, SIGTERM);
        (void)waitpid(child, nullptr, 0);
    }
    sc_bytes_clear(&output);
    sc_string_builder_clear(&input);
    sc_string_clear(&init);
    sc_free(alloc, argv, argv_count * sizeof(*argv), _Alignof(char *));
    sc_json_destroy(args_root);
    return status;
}

static sc_status mcp_call_http(mcp_server_tool *tool, sc_allocator *alloc, sc_str target_tool, sc_str args_json, sc_string *out)
{
    sc_string request = {0};
    sc_status status = build_tools_call_json(alloc, target_tool, args_json, 1, &request);

    if (sc_status_is_ok(status)) {
        status = mcp_request_http(tool, alloc, sc_string_as_str(&request), out);
    }
    sc_string_clear(&request);
    return status;
}

static sc_status mcp_request_http(mcp_server_tool *tool, sc_allocator *alloc, sc_str request_json, sc_string *out)
{
    sc_status status = sc_status_ok();
#ifdef SC_HAVE_LIBCURL
    CURL *curl = nullptr;
    struct curl_slist *headers = nullptr;
    mcp_http_response response = {0};
    CURLcode code;
    long response_code = 0;

    if (tool->url.len == 0) {
        return sc_status_invalid_argument("sc.mcp_tool.missing_url");
    }
    status = sc_security_validate_url(tool->base.context.policy, sc_string_as_str(&tool->url));
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_curl_global_init("sc.mcp_tool.curl_init_failed");
    if (!sc_status_is_ok(status)) {
        return sc_status_http("sc.mcp_tool.curl_init_failed");
    }
    curl = curl_easy_init();
    if (curl == nullptr) {
        status = sc_status_http("sc.mcp_tool.curl_init_failed");
    }
    if (sc_status_is_ok(status)) {
        sc_bytes_init(&response.bytes, alloc);
        response.max_bytes = tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Accept: application/json, text/event-stream");
        status = add_header_list(alloc, &headers, sc_string_as_str(&tool->headers));
    }
    if (sc_status_is_ok(status)) {
        (void)curl_easy_setopt(curl, CURLOPT_URL, tool->url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.ptr == nullptr ? "" : request_json.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_json.len);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mcp_curl_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 mcp");
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, tool->base.context.timeout_ms == 0 ? 30000L : (long)tool->base.context.timeout_ms);
        code = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (code != CURLE_OK || response.too_large) {
            status = sc_status_http(response.too_large ? "sc.mcp_tool.response_too_large" : "sc.mcp_tool.http_failed");
        } else if (response_code < 200 || response_code >= 300) {
            status = sc_status_http("sc.mcp_tool.http_status");
        }
    }
    if (sc_status_is_ok(status)) {
        status = maybe_extract_sse_payload(alloc, sc_str_from_parts((const char *)response.bytes.ptr, response.bytes.len), out);
    }
    if (curl != nullptr) {
        curl_easy_cleanup(curl);
    }
    curl_slist_free_all(headers);
    sc_bytes_clear(&response.bytes);
#else
    (void)tool;
    (void)request_json;
    (void)out;
    status = sc_status_unsupported("sc.mcp_tool.libcurl_unavailable");
#endif
    return status;
}

static sc_status mcp_discover_names(mcp_server_tool *tool, sc_allocator *alloc, sc_vec *names)
{
    sc_string request = {0};
    sc_string response = {0};
    sc_status status = sc_status_ok();

    if (tool == nullptr || names == nullptr) {
        return sc_status_invalid_argument("sc.mcp_tool.discover_invalid_argument");
    }
    status = build_tools_list_json(alloc, &request);
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("stdio"))) {
        status = mcp_request_stdio(tool, alloc, sc_string_as_str(&request), 2, &response);
    } else if (sc_status_is_ok(status) &&
               (sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("http")) ||
                sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("sse")))) {
        status = mcp_request_http(tool, alloc, sc_string_as_str(&request), &response);
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.mcp_tool.unknown_transport");
    }
    if (sc_status_is_ok(status)) {
        status = mcp_names_from_response(alloc, sc_string_as_str(&response), names);
    }
    sc_string_clear(&request);
    sc_string_clear(&response);
    return status;
}

static sc_status mcp_names_from_response(sc_allocator *alloc, sc_str response, sc_vec *names)
{
    sc_json_value *root = nullptr;
    sc_json_value *tools = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (names == nullptr) {
        return sc_status_invalid_argument("sc.mcp_tool.names_invalid_argument");
    }
    status = sc_json_parse(alloc, response, &root, &error);
    if (sc_status_is_ok(status)) {
        sc_json_value *result = sc_json_object_get(root, sc_str_from_cstr("result"));
        tools = sc_json_object_get(result, sc_str_from_cstr("tools"));
        if (sc_json_type_of(tools) != SC_JSON_ARRAY) {
            status = sc_status_parse("sc.mcp_tool.tools_list_missing");
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(tools); i += 1) {
        sc_json_value *item = sc_json_array_get(tools, i);
        sc_str name = {0};
        sc_string copy = {0};
        if (!sc_json_as_str(sc_json_object_get(item, sc_str_from_cstr("name")), &name) || name.len == 0) {
            status = sc_status_parse("sc.mcp_tool.tool_name_missing");
            break;
        }
        status = sc_string_from_str(alloc, name, &copy);
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(names, &copy);
            if (!sc_status_is_ok(status)) {
                sc_string_clear(&copy);
            }
        }
    }
    sc_json_destroy(root);
    return status;
}

static sc_status build_stdio_argv(sc_allocator *alloc, sc_str command, sc_str args_json, sc_json_value **args_root, char ***out_argv, size_t *out_count)
{
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();
    size_t argc = 1;
    char **argv = nullptr;

    if (out_argv == nullptr || out_count == nullptr || args_root == nullptr || command.len == 0 || command.ptr == nullptr) {
        return sc_status_invalid_argument("sc.mcp_tool.argv_invalid_argument");
    }
    if (args_json.ptr != nullptr && args_json.len > 0) {
        status = sc_json_parse(alloc, args_json, args_root, &error);
        if (sc_status_is_ok(status) && sc_json_type_of(*args_root) != SC_JSON_ARRAY) {
            status = sc_status_invalid_argument("sc.mcp_tool.args_not_array");
        }
        if (sc_status_is_ok(status)) {
            argc += sc_json_array_len(*args_root);
        }
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    argv = sc_alloc(alloc, (argc + 1) * sizeof(*argv), _Alignof(char *));
    if (argv == nullptr) {
        return sc_status_no_memory();
    }
    argv[0] = (char *)command.ptr;
    for (size_t i = 1; i < argc; ++i) {
        sc_str value = {0};
        if (!sc_json_as_str(sc_json_array_get(*args_root, i - 1), &value)) {
            sc_free(alloc, argv, (argc + 1) * sizeof(*argv), _Alignof(char *));
            return sc_status_invalid_argument("sc.mcp_tool.arg_not_string");
        }
        argv[i] = (char *)value.ptr;
    }
    argv[argc] = nullptr;
    *out_argv = argv;
    *out_count = argc + 1;
    return sc_status_ok();
}

static sc_status write_all(int fd, sc_str bytes)
{
    size_t offset = 0;
    while (offset < bytes.len) {
        ssize_t written = write(fd, bytes.ptr + offset, bytes.len - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return sc_status_io("sc.mcp_tool.write_failed");
        }
        offset += (size_t)written;
    }
    return sc_status_ok();
}

static sc_status read_child_output(sc_allocator *alloc, int fd, pid_t child, size_t max_bytes, int64_t timeout_ms, sc_bytes *out)
{
    struct pollfd poll_fd = {.fd = fd, .events = POLLIN | POLLHUP};
    sc_status status = sc_status_ok();
    int flags = fcntl(fd, F_GETFL, 0);
    int wait_status = 0;

    sc_bytes_init(out, alloc);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    while (sc_status_is_ok(status)) {
        int ready = poll(&poll_fd, 1, timeout_ms <= 0 ? 30000 : (int)timeout_ms);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            status = sc_status_io("sc.mcp_tool.poll_failed");
            break;
        }
        if (ready == 0) {
            status = sc_status_timeout("sc.mcp_tool.timeout");
            break;
        }
        if ((poll_fd.revents & POLLIN) != 0) {
            uint8_t buffer[4096] = {0};
            ssize_t count = read(fd, buffer, sizeof(buffer));
            if (count > 0) {
                if (out->len + (size_t)count > max_bytes) {
                    status = sc_status_io("sc.mcp_tool.output_too_large");
                } else {
                    status = sc_bytes_append(out, sc_buf_from_parts(buffer, (size_t)count));
                }
                continue;
            }
            if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
        }
        if ((poll_fd.revents & POLLHUP) != 0) {
            break;
        }
    }
    if (child > 0) {
        if (!sc_status_is_ok(status)) {
            (void)kill(child, SIGTERM);
        }
        (void)waitpid(child, &wait_status, 0);
        if (sc_status_is_ok(status) && (!WIFEXITED(wait_status) || WEXITSTATUS(wait_status) != 0)) {
            status = sc_status_io("sc.mcp_tool.child_failed");
        }
    }
    return status;
}

static sc_status extract_mcp_response(sc_allocator *alloc, sc_str stream, int64_t wanted_id, sc_string *out)
{
    size_t offset = 0;

    while (offset < stream.len) {
        const char *start = stream.ptr + offset;
        const char *header = nullptr;
        const char *body = nullptr;
        char *end = nullptr;
        unsigned long length = 0;
        sc_json_value *root = nullptr;
        sc_json_parse_error error = {0};
        double id = 0.0;

        header = strstr(start, "Content-Length:");
        if (header == nullptr) {
            break;
        }
        length = strtoul(header + strlen("Content-Length:"), &end, 10);
        body = strstr(header, "\r\n\r\n");
        if (body != nullptr) {
            body += 4;
        } else {
            body = strstr(header, "\n\n");
            if (body != nullptr) {
                body += 2;
            }
        }
        if (body == nullptr || length == 0 || (size_t)(body - stream.ptr) + (size_t)length > stream.len) {
            break;
        }
        if (sc_status_is_ok(sc_json_parse(alloc, sc_str_from_parts(body, (size_t)length), &root, &error)) &&
            sc_json_as_number(sc_json_object_get(root, sc_str_from_cstr("id")), &id) &&
            (int64_t)id == wanted_id) {
            sc_json_destroy(root);
            return sc_string_from_str(alloc, sc_str_from_parts(body, (size_t)length), out);
        }
        sc_json_destroy(root);
        offset = (size_t)(body - stream.ptr) + (size_t)length;
    }
    return sc_status_parse("sc.mcp_tool.response_not_found");
}

static sc_status maybe_extract_sse_payload(sc_allocator *alloc, sc_str response, sc_string *out)
{
    sc_string_builder builder = {0};
    bool found_data = false;
    size_t offset = 0;
    sc_status status = sc_status_ok();

    while (offset < response.len) {
        size_t start = offset;
        size_t len = 0;
        while (offset < response.len && response.ptr[offset] != '\n') {
            offset += 1;
        }
        len = offset - start;
        if (len > 0 && response.ptr[start + len - 1] == '\r') {
            len -= 1;
        }
        if (len >= 5 && memcmp(response.ptr + start, "data:", 5) == 0) {
            size_t data_start = start + 5;
            while (data_start < start + len && (response.ptr[data_start] == ' ' || response.ptr[data_start] == '\t')) {
                data_start += 1;
            }
            if (!found_data) {
                sc_string_builder_init(&builder, alloc);
                found_data = true;
            } else {
                status = sc_string_builder_append_cstr(&builder, "\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_str_from_parts(response.ptr + data_start, start + len - data_start));
            }
        }
        if (offset < response.len && response.ptr[offset] == '\n') {
            offset += 1;
        }
    }
    if (found_data && sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else if (found_data) {
        sc_string_builder_clear(&builder);
    } else {
        status = sc_string_from_str(alloc, response, out);
    }
    return status;
}

static sc_status set_string(sc_json_value *object, sc_str key, sc_str value)
{
    sc_json_value *string = nullptr;
    sc_status status = sc_json_string_new(nullptr, value, &string);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(object, key, string);
        string = nullptr;
    }
    sc_json_destroy(string);
    return status;
}

static sc_status set_number(sc_json_value *object, sc_str key, double value)
{
    sc_json_value *number = nullptr;
    sc_status status = sc_json_number_new(nullptr, value, &number);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(object, key, number);
        number = nullptr;
    }
    sc_json_destroy(number);
    return status;
}

static sc_status set_object(sc_json_value *object, sc_str key, sc_json_value *value)
{
    if (object == nullptr || value == nullptr) {
        return sc_status_invalid_argument("sc.mcp_tool.json_invalid_argument");
    }
    return sc_json_object_set(object, key, value);
}

static sc_tool_risk mcp_tool_risk(const mcp_server_tool *tool)
{
    return tool != nullptr && sc_str_equal(sc_string_as_str(&tool->transport), sc_str_from_cstr("stdio")) ?
        SC_TOOL_RISK_SIDE_EFFECT :
        SC_TOOL_RISK_NETWORK;
}

#ifdef SC_HAVE_LIBCURL
static size_t mcp_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mcp_http_response *response = userdata;
    size_t total = size * nmemb;
    if (response == nullptr || response->too_large) {
        return 0;
    }
    if (response->bytes.len + total > response->max_bytes) {
        response->too_large = true;
        return 0;
    }
    return sc_status_is_ok(sc_bytes_append(&response->bytes, sc_buf_from_parts(ptr, total))) ? total : 0;
}

static sc_status add_header_list(sc_allocator *alloc, struct curl_slist **headers, sc_str raw_headers)
{
    sc_json_value *array = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (headers == nullptr || raw_headers.ptr == nullptr || raw_headers.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, raw_headers, &array, &error);
    if (sc_status_is_ok(status) && sc_json_type_of(array) == SC_JSON_ARRAY) {
        for (size_t i = 0; i < sc_json_array_len(array); ++i) {
            sc_str header = {0};
            if (sc_json_as_str(sc_json_array_get(array, i), &header) && header.len > 0) {
                sc_string copy = {0};
                status = sc_string_from_str(alloc, header, &copy);
                if (!sc_status_is_ok(status)) {
                    break;
                }
                *headers = curl_slist_append(*headers, copy.ptr);
                sc_string_clear(&copy);
                if (*headers == nullptr) {
                    status = sc_status_no_memory();
                    break;
                }
            }
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.mcp_tool.headers_not_array");
    }
    sc_json_destroy(array);
    return status;
}
#endif
