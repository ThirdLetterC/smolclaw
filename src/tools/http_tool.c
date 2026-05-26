// cppcheck-suppress-file redundantInitialization
#include "tools/tool_internal.h"

#include "sc/config.h"

#include "net/http_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tools/http_tool_internal.h"

static sc_status http_tool_spec(void *impl, sc_tool_spec *out);
static sc_status http_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status http_tool_invoke_async(void *impl,
                                        sc_async_context *context,
                                        const sc_tool_call *call,
                                        sc_allocator *alloc,
                                        sc_tool_invoke_complete_fn complete,
                                        void *complete_user_data,
                                        sc_async_op **out);
static void http_tool_destroy(void *impl);
static sc_status http_tool_new(sc_allocator *alloc, const sc_tool_context *context, http_tool_kind kind, sc_tool **out);
static sc_status invoke_http(http_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_web_search(http_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_tool_risk method_risk(sc_str method);
static sc_status http_get_config_string(const http_tool *tool, sc_str path, sc_allocator *alloc, sc_string *out);
static sc_status append_query_url(sc_allocator *alloc, sc_str base, sc_str query, sc_string *out);
static sc_status append_url_encoded(sc_string_builder *builder, sc_str value);
static sc_status append_json_string_escaped(sc_string_builder *builder, sc_str value);
static bool str_equal_ignore_case(sc_str left, const char *right);
static sc_status env_value_from_config(const http_tool *tool, sc_str config_path, sc_allocator *alloc, sc_string *out);
static sc_status build_header_array(sc_allocator *alloc, const sc_str *headers, size_t header_count, sc_string *out);
static sc_status build_search_body(sc_allocator *alloc, sc_str query, sc_string *out);
static sc_status http_perform(sc_allocator *alloc,
                              sc_str method,
                              sc_str url,
                              sc_str headers_json,
                              sc_str body,
                              size_t max_bytes,
                              int64_t timeout_ms,
                              sc_string *out);

static const sc_tool_vtab http_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "http",
    .display_name = "HTTP",
    .feature_flag = "SC_TOOL_HTTP",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = http_tool_spec,
    .invoke = http_tool_invoke,
    .destroy = http_tool_destroy,
    .invoke_async = http_tool_invoke_async,
};

static const sc_tool_vtab web_search_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "web_search",
    .display_name = "Web search",
    .feature_flag = "SC_TOOL_WEB_SEARCH",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = http_tool_spec,
    .invoke = http_tool_invoke,
    .destroy = http_tool_destroy,
    .invoke_async = http_tool_invoke_async,
};

sc_status sc_tool_http_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return http_tool_new(alloc, context, HTTP_TOOL_REQUEST, out);
}

sc_status sc_tool_web_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return http_tool_new(alloc, context, HTTP_TOOL_WEB_SEARCH, out);
}

static sc_status http_tool_spec(void *impl, sc_tool_spec *out)
{
    const http_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = tool->kind == HTTP_TOOL_REQUEST ? sc_str_from_cstr("http") : sc_str_from_cstr("web_search"),
        .description = tool->kind == HTTP_TOOL_REQUEST ? sc_str_from_cstr("tool.http.description") :
                                                         sc_str_from_cstr("tool.web_search.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = tool->kind == HTTP_TOOL_REQUEST ? SC_TOOL_RISK_NETWORK : SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_NETWORK,
        .side_effect = SC_TOOL_SIDE_EFFECT_NETWORK,
        .default_autonomy = tool->kind == HTTP_TOOL_REQUEST ? SC_AUTONOMY_SUPERVISED : SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = tool->kind == HTTP_TOOL_REQUEST ? sc_str_from_cstr("tool.http.catalog") :
                                                                  sc_str_from_cstr("tool.web_search.catalog"),
    };
    return sc_status_ok();
}

static sc_status http_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    http_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.invalid_argument");
    }
    return tool->kind == HTTP_TOOL_REQUEST ? invoke_http(tool, call, alloc, out) :
                                             invoke_web_search(tool, call, alloc, out);
}

static sc_status http_tool_invoke_async(void *impl,
                                        sc_async_context *context,
                                        const sc_tool_call *call,
                                        sc_allocator *alloc,
                                        sc_tool_invoke_complete_fn complete,
                                        void *complete_user_data,
                                        sc_async_op **out)
{
    http_tool *tool = impl;
    http_tool_async_state *state = nullptr;
    sc_status status = sc_status_ok();
    sc_str method = {0};
    sc_str url = {0};
    sc_str headers = {0};
    sc_str body = {0};
    sc_str query = {0};
    sc_string provider = {0};
    sc_string api_key = {0};
    sc_string cse_id = {0};
    sc_string built_url = {0};
    sc_string built_headers = {0};
    sc_string built_body = {0};
    size_t max_bytes = 0;

    if (tool == nullptr || context == nullptr || call == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.async_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(http_tool_async_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (http_tool_async_state){
        .alloc = alloc,
        .tool = tool,
        .complete = complete,
        .complete_user_data = complete_user_data,
    };

    status = sc_tool_check_cancelled(&tool->base, call);
    if (tool->kind == HTTP_TOOL_REQUEST) {
        if (sc_status_is_ok(status)) {
            status = sc_tool_get_string_arg(call, sc_str_from_cstr("method"), &method);
        }
        if (sc_status_is_ok(status)) {
            status = sc_tool_get_string_arg(call, sc_str_from_cstr("url"), &url);
        }
        if (sc_status_is_ok(status)) {
            (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("headers_json"), &headers);
            (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("body"), &body);
        }
        if (sc_status_is_ok(status)) {
            status = sc_tool_security_check(&tool->base,
                                            sc_str_from_cstr("http"),
                                            method_risk(method),
                                            sc_str_from_cstr(""),
                                            false,
                                            url,
                                            sc_str_from_cstr(""));
        }
        if (sc_status_is_ok(status)) {
            max_bytes = (size_t)sc_config_get_int(tool->base.context.config,
                                                  sc_str_from_cstr("tools.http.max_body_bytes"),
                                                  (int64_t)(tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes));
            status = sc_string_from_cstr(alloc, "http", &state->tool_name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc, url, &state->args_summary);
        }
        if (sc_status_is_ok(status)) {
            status = http_tool_schedule_async(state,
                                              context,
                                              method,
                                              url,
                                              headers,
                                              body,
                                              max_bytes,
                                              tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms);
        }
    } else {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("query"), &query);
        if (sc_status_is_ok(status)) {
            status = http_get_config_string(tool, sc_str_from_cstr("tools.web_search.provider"), alloc, &provider);
            if (!sc_status_is_ok(status) || provider.len == 0 || sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("none"))) {
                sc_status_clear(&status);
                status = sc_status_unsupported("sc.web_search_tool.not_configured");
            }
        }
        if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("fake"))) {
            sc_tool_result result = {0};
            sc_string fake = {0};
            sc_string_builder builder = {0};
            sc_string_builder_init(&builder, alloc);
            status = sc_string_builder_append_cstr(&builder, "provider=fake\nquery=");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, query);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &fake);
            } else {
                sc_string_builder_clear(&builder);
            }
            if (sc_status_is_ok(status)) {
                status = sc_tool_set_output(alloc, &tool->base, &result, sc_string_as_str(&fake), true);
            }
            if (sc_status_is_ok(status)) {
                status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("web_search"), query, sc_string_as_str(&result.output), true);
            } else {
                sc_tool_log_failure(sc_str_from_cstr("web_search"), status);
                (void)sc_tool_record_receipt_status(&tool->base,
                                                    sc_str_from_cstr("web_search"),
                                                    query,
                                                    sc_str_from_cstr("error"),
                                                    false,
                                                    status);
            }
            complete(complete_user_data, sc_status_is_ok(status) ? &result : nullptr, status);
            sc_tool_result_clear(&result);
            sc_string_clear(&fake);
            http_tool_async_state_destroy(state);
            goto cleanup;
        }
        if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("brave"))) {
            sc_string api_header = {0};
            sc_str header_values[1] = {0};
            status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.brave_api_key_env"), alloc, &api_key);
            if (sc_status_is_ok(status)) {
                status = append_query_url(alloc, sc_str_from_cstr("https://api.search.brave.com/res/v1/web/search?q="), query, &built_url);
            }
            if (sc_status_is_ok(status)) {
                sc_string_builder builder = {0};
                sc_string_builder_init(&builder, alloc);
                status = sc_string_builder_append_cstr(&builder, "X-Subscription-Token: ");
                if (sc_status_is_ok(status)) {
                    status = sc_string_builder_append(&builder, sc_string_as_str(&api_key));
                }
                if (sc_status_is_ok(status)) {
                    status = sc_string_builder_finish(&builder, &api_header);
                } else {
                    sc_string_builder_clear(&builder);
                }
            }
            if (sc_status_is_ok(status)) {
                header_values[0] = sc_string_as_str(&api_header);
                status = build_header_array(alloc, header_values, SC_ARRAY_LEN(header_values), &built_headers);
            }
            sc_string_clear(&api_header);
        } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("serper"))) {
            sc_string api_header = {0};
            sc_str header_values[2] = {0};
            status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.serper_api_key_env"), alloc, &api_key);
            if (sc_status_is_ok(status)) {
                status = sc_string_from_cstr(alloc, "https://google.serper.dev/search", &built_url);
            }
            if (sc_status_is_ok(status)) {
                status = build_search_body(alloc, query, &built_body);
            }
            if (sc_status_is_ok(status)) {
                sc_string_builder builder = {0};
                sc_string_builder_init(&builder, alloc);
                status = sc_string_builder_append_cstr(&builder, "X-API-KEY: ");
                if (sc_status_is_ok(status)) {
                    status = sc_string_builder_append(&builder, sc_string_as_str(&api_key));
                }
                if (sc_status_is_ok(status)) {
                    status = sc_string_builder_finish(&builder, &api_header);
                } else {
                    sc_string_builder_clear(&builder);
                }
            }
            if (sc_status_is_ok(status)) {
                header_values[0] = sc_str_from_cstr("Content-Type: application/json");
                header_values[1] = sc_string_as_str(&api_header);
                status = build_header_array(alloc, header_values, SC_ARRAY_LEN(header_values), &built_headers);
            }
            sc_string_clear(&api_header);
        } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("google_cse"))) {
            sc_string_builder builder = {0};
            status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.google_api_key_env"), alloc, &api_key);
            if (sc_status_is_ok(status)) {
                status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.google_cse_id_env"), alloc, &cse_id);
            }
            sc_string_builder_init(&builder, alloc);
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "https://www.googleapis.com/customsearch/v1?key=");
            }
            if (sc_status_is_ok(status)) {
                status = append_url_encoded(&builder, sc_string_as_str(&api_key));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "&cx=");
            }
            if (sc_status_is_ok(status)) {
                status = append_url_encoded(&builder, sc_string_as_str(&cse_id));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "&q=");
            }
            if (sc_status_is_ok(status)) {
                status = append_url_encoded(&builder, query);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &built_url);
            } else {
                sc_string_builder_clear(&builder);
            }
        } else if (sc_status_is_ok(status)) {
            status = sc_status_unsupported("sc.web_search_tool.provider_unsupported");
        }
        if (sc_status_is_ok(status)) {
            status = sc_tool_security_check(&tool->base,
                                            sc_str_from_cstr("web_search"),
                                            SC_TOOL_RISK_READONLY,
                                            sc_str_from_cstr(""),
                                            false,
                                            sc_string_as_str(&built_url),
                                            sc_str_from_cstr(""));
        }
        if (sc_status_is_ok(status)) {
            max_bytes = (size_t)sc_config_get_int(tool->base.context.config,
                                                  sc_str_from_cstr("tools.web_search.max_response_bytes"),
                                                  (int64_t)(tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes));
            status = sc_string_from_cstr(alloc, "web_search", &state->tool_name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc, query, &state->args_summary);
        }
        if (sc_status_is_ok(status)) {
            status = http_tool_schedule_async(state,
                                              context,
                                              sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("serper")) ? sc_str_from_cstr("POST") :
                                                                                                                      sc_str_from_cstr("GET"),
                                              sc_string_as_str(&built_url),
                                              sc_string_as_str(&built_headers),
                                              sc_string_as_str(&built_body),
                                              max_bytes,
                                              tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms);
        }
    }

    if (!sc_status_is_ok(status)) {
        sc_str tool_name = tool->kind == HTTP_TOOL_WEB_SEARCH ? sc_str_from_cstr("web_search") : sc_str_from_cstr("http");
        sc_str args_summary = tool->kind == HTTP_TOOL_WEB_SEARCH ? query : url;
        sc_tool_log_failure(tool_name, status);
        (void)sc_tool_record_receipt_status(&tool->base,
                                            tool_name,
                                            args_summary,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
        complete(complete_user_data, nullptr, status);
        http_tool_async_state_destroy(state);
    }

cleanup:
    sc_string_clear(&provider);
    sc_string_clear(&api_key);
    sc_string_clear(&cse_id);
    sc_string_clear(&built_url);
    sc_string_clear(&built_headers);
    sc_string_clear(&built_body);
    return sc_status_ok();
}

static void http_tool_destroy(void *impl)
{
    http_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(http_tool));
}

static sc_status http_tool_new(sc_allocator *alloc, const sc_tool_context *context, http_tool_kind kind, sc_tool **out)
{
    http_tool *tool = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(http_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (http_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status) && kind == HTTP_TOOL_REQUEST) {
        status = sc_tool_schema_four_strings(alloc,
                                             sc_str_from_cstr("method"),
                                             true,
                                             sc_str_from_cstr("url"),
                                             true,
                                             sc_str_from_cstr("headers_json"),
                                             false,
                                             sc_str_from_cstr("body"),
                                             false,
                                             &tool->base.schema);
    } else if (sc_status_is_ok(status)) {
        status = sc_tool_schema_string_required(alloc,
                                                sc_str_from_cstr("query"),
                                                sc_str_from_cstr("tool.web_search.description"),
                                                &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, kind == HTTP_TOOL_REQUEST ? &http_vtab : &web_search_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        http_tool_destroy(tool);
    }
    return status;
}

static sc_status invoke_http(http_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str method = {0};
    sc_str url = {0};
    sc_str headers = {0};
    sc_str body = {0};
    sc_string response = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("method"), &method);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("url"), &url);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("headers_json"), &headers);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("body"), &body);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("http"),
                                        method_risk(method),
                                        sc_str_from_cstr(""),
                                        false,
                                        url,
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        size_t max_bytes = (size_t)sc_config_get_int(tool->base.context.config,
                                                     sc_str_from_cstr("tools.http.max_body_bytes"),
                                                     (int64_t)(tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes));
        status = http_perform(alloc,
                              method,
                              url,
                              headers,
                              body,
                              max_bytes,
                              tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms,
                              &response);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&response), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("http"), url, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("http"),
                                            url,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&response);
    return status;
}

static sc_status invoke_web_search(http_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str query = {0};
    sc_string provider = {0};
    sc_string api_key = {0};
    sc_string cse_id = {0};
    sc_string url = {0};
    sc_string headers = {0};
    sc_string body = {0};
    sc_string response = {0};
    size_t max_bytes = 0;
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("query"), &query);
    }
    if (sc_status_is_ok(status)) {
        status = http_get_config_string(tool, sc_str_from_cstr("tools.web_search.provider"), alloc, &provider);
        if (!sc_status_is_ok(status) || provider.len == 0 || sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("none"))) {
            sc_status_clear(&status);
            status = sc_status_unsupported("sc.web_search_tool.not_configured");
        }
    }
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("fake"))) {
        sc_string_builder builder = {0};
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "provider=fake\nquery=");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, query);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &response);
        } else {
            sc_string_builder_clear(&builder);
        }
        goto finish;
    }
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("brave"))) {
        sc_string api_header = {0};
        sc_str header_values[1] = {0};
        status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.brave_api_key_env"), alloc, &api_key);
        if (sc_status_is_ok(status)) {
            status = append_query_url(alloc, sc_str_from_cstr("https://api.search.brave.com/res/v1/web/search?q="), query, &url);
        }
        if (sc_status_is_ok(status)) {
            sc_string_builder builder = {0};
            sc_string_builder_init(&builder, alloc);
            status = sc_string_builder_append_cstr(&builder, "X-Subscription-Token: ");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&api_key));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &api_header);
            } else {
                sc_string_builder_clear(&builder);
            }
        }
        if (sc_status_is_ok(status)) {
            header_values[0] = sc_string_as_str(&api_header);
            status = build_header_array(alloc, header_values, SC_ARRAY_LEN(header_values), &headers);
        }
        sc_string_clear(&api_header);
    } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("serper"))) {
        sc_string api_header = {0};
        sc_str header_values[2] = {0};
        status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.serper_api_key_env"), alloc, &api_key);
        if (sc_status_is_ok(status)) {
            status = sc_string_from_cstr(alloc, "https://google.serper.dev/search", &url);
        }
        if (sc_status_is_ok(status)) {
            status = build_search_body(alloc, query, &body);
        }
        if (sc_status_is_ok(status)) {
            sc_string_builder builder = {0};
            sc_string_builder_init(&builder, alloc);
            status = sc_string_builder_append_cstr(&builder, "X-API-KEY: ");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&api_key));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &api_header);
            } else {
                sc_string_builder_clear(&builder);
            }
        }
        if (sc_status_is_ok(status)) {
            header_values[0] = sc_str_from_cstr("Content-Type: application/json");
            header_values[1] = sc_string_as_str(&api_header);
            status = build_header_array(alloc, header_values, SC_ARRAY_LEN(header_values), &headers);
        }
        sc_string_clear(&api_header);
    } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("google_cse"))) {
        sc_string_builder builder = {0};
        status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.google_api_key_env"), alloc, &api_key);
        if (sc_status_is_ok(status)) {
            status = env_value_from_config(tool, sc_str_from_cstr("tools.web_search.google_cse_id_env"), alloc, &cse_id);
        }
        sc_string_builder_init(&builder, alloc);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "https://www.googleapis.com/customsearch/v1?key=");
        }
        if (sc_status_is_ok(status)) {
            status = append_url_encoded(&builder, sc_string_as_str(&api_key));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "&cx=");
        }
        if (sc_status_is_ok(status)) {
            status = append_url_encoded(&builder, sc_string_as_str(&cse_id));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "&q=");
        }
        if (sc_status_is_ok(status)) {
            status = append_url_encoded(&builder, query);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &url);
        } else {
            sc_string_builder_clear(&builder);
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_status_unsupported("sc.web_search_tool.provider_unsupported");
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("web_search"),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_string_as_str(&url),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        max_bytes = (size_t)sc_config_get_int(tool->base.context.config,
                                              sc_str_from_cstr("tools.web_search.max_response_bytes"),
                                              (int64_t)(tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes));
        status = http_perform(alloc,
                              sc_str_equal(sc_string_as_str(&provider), sc_str_from_cstr("serper")) ? sc_str_from_cstr("POST") : sc_str_from_cstr("GET"),
                              sc_string_as_str(&url),
                              sc_string_as_str(&headers),
                              sc_string_as_str(&body),
                              max_bytes,
                              tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms,
                              &response);
    }
finish:
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&response), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("web_search"), query, sc_string_as_str(&out->output), true);
    } else {
        sc_tool_log_failure(sc_str_from_cstr("web_search"), status);
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("web_search"),
                                            query,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&provider);
    sc_string_clear(&api_key);
    sc_string_clear(&cse_id);
    sc_string_clear(&url);
    sc_string_clear(&headers);
    sc_string_clear(&body);
    sc_string_clear(&response);
    return status;
}

static sc_tool_risk method_risk(sc_str method)
{
    if (str_equal_ignore_case(method, "GET") || str_equal_ignore_case(method, "HEAD")) {
        return SC_TOOL_RISK_READONLY;
    }
    if (str_equal_ignore_case(method, "DELETE")) {
        return SC_TOOL_RISK_DESTRUCTIVE;
    }
    return SC_TOOL_RISK_NETWORK;
}

static sc_status http_get_config_string(const http_tool *tool, sc_str path, sc_allocator *alloc, sc_string *out)
{
    if (tool == nullptr || tool->base.context.config == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.config_missing");
    }
    return sc_config_get_prop(tool->base.context.config, path, alloc, out);
}

static sc_status append_query_url(sc_allocator *alloc, sc_str base, sc_str query, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, base);
    for (size_t i = 0; sc_status_is_ok(status) && i < query.len; i += 1) {
        status = append_url_encoded(&builder, sc_str_from_parts(query.ptr + i, 1));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_url_encoded(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_status_ok();
    if (builder == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.url_encode_invalid_argument");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char encoded[4] = {0};
        unsigned char ch = (unsigned char)value.ptr[i];
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.') {
            status = sc_string_builder_append(builder, sc_str_from_parts(value.ptr + i, 1));
        } else if (ch == ' ') {
            status = sc_string_builder_append_cstr(builder, "+");
        } else {
            (void)snprintf(encoded, sizeof(encoded), "%%%02X", ch);
            status = sc_string_builder_append_cstr(builder, encoded);
        }
    }
    return status;
}

static sc_status append_json_string_escaped(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_status_ok();
    if (builder == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.json_escape_invalid_argument");
    }
    status = sc_string_builder_append_cstr(builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char ch = value.ptr[i];
        if (ch == '"' || ch == '\\') {
            char escaped[2] = {'\\', ch};
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else if (ch == '\r') {
            status = sc_string_builder_append_cstr(builder, "\\r");
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(value.ptr + i, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

static bool str_equal_ignore_case(sc_str left, const char *right)
{
    size_t right_len = right == nullptr ? 0 : strlen(right);
    if (left.len != right_len || left.ptr == nullptr || right == nullptr) {
        return false;
    }
    for (size_t i = 0; i < left.len; i += 1) {
        if (tolower((unsigned char)left.ptr[i]) != tolower((unsigned char)right[i])) {
            return false;
        }
    }
    return true;
}

static sc_status env_value_from_config(const http_tool *tool, sc_str config_path, sc_allocator *alloc, sc_string *out)
{
    sc_string env_name = {0};
    const char *value = nullptr;
    sc_status status = http_get_config_string(tool, config_path, alloc, &env_name);
    if (!sc_status_is_ok(status) || env_name.len == 0) {
        sc_status_clear(&status);
        sc_string_clear(&env_name);
        return sc_status_unsupported("sc.web_search_tool.credential_env_missing");
    }
    value = getenv(env_name.ptr);
    if (value == nullptr || value[0] == '\0') {
        sc_string_clear(&env_name);
        return sc_status_unsupported("sc.web_search_tool.credential_missing");
    }
    for (const char *cursor = value; *cursor != '\0'; cursor += 1) {
        if (*cursor == '\r' || *cursor == '\n') {
            sc_string_clear(&env_name);
            return sc_status_invalid_argument("sc.web_search_tool.credential_invalid");
        }
    }
    status = sc_string_from_cstr(alloc, value, out);
    sc_string_clear(&env_name);
    return status;
}

static sc_status build_header_array(sc_allocator *alloc, const sc_str *headers, size_t header_count, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || (header_count > 0 && headers == nullptr)) {
        return sc_status_invalid_argument("sc.http_tool.headers_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "[");
    for (size_t i = 0; sc_status_is_ok(status) && i < header_count; i += 1) {
        if (i > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string_escaped(&builder, headers[i]);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "]");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_search_body(sc_allocator *alloc, sc_str query, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"q\":");
    if (sc_status_is_ok(status)) {
        status = append_json_string_escaped(&builder, query);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

void http_headers_clear(sc_vec *headers)
{
    if (headers == nullptr) {
        return;
    }
    for (size_t i = 0; i < headers->len; i += 1) {
        const sc_http_header *header = sc_vec_at(headers, i);
        sc_string name = {.ptr = (char *)header->name.ptr, .len = header->name.len, .alloc = headers->alloc};
        sc_string value = {.ptr = (char *)header->value.ptr, .len = header->value.len, .alloc = headers->alloc};
        sc_string_clear(&name);
        sc_string_clear(&value);
    }
    sc_vec_clear(headers);
}

sc_status http_headers_from_json(sc_allocator *alloc, sc_str headers_json, sc_vec *out)
{
    sc_json_value *array = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.headers_invalid_argument");
    }
    sc_vec_init(out, alloc, sizeof(sc_http_header));
    if (headers_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, headers_json, &array, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_json_type_of(array) != SC_JSON_ARRAY) {
        sc_json_destroy(array);
        return sc_status_invalid_argument("sc.http_tool.headers_not_array");
    }
    for (size_t i = 0; i < sc_json_array_len(array); i += 1) {
        sc_str header = {0};
        size_t colon = 0;
        sc_http_header parsed;
        sc_string name = {0};
        sc_string value = {0};
        if (!sc_json_as_str(sc_json_array_get(array, i), &header) || header.len == 0) {
            http_headers_clear(out);
            sc_json_destroy(array);
            return sc_status_invalid_argument("sc.http_tool.header_invalid");
        }
        for (size_t j = 0; j < header.len; j += 1) {
            if (header.ptr[j] == '\r' || header.ptr[j] == '\n') {
                http_headers_clear(out);
                sc_json_destroy(array);
                return sc_status_invalid_argument("sc.http_tool.header_invalid");
            }
            if (header.ptr[j] == ':' && colon == 0) {
                colon = j;
            }
        }
        if (colon == 0) {
            http_headers_clear(out);
            sc_json_destroy(array);
            return sc_status_invalid_argument("sc.http_tool.header_invalid");
        }
        status = sc_string_from_str(alloc, sc_str_from_parts(header.ptr, colon), &name);
        while (colon + 1 < header.len && header.ptr[colon + 1] == ' ') {
            colon += 1;
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc,
                                        sc_str_from_parts(header.ptr + colon + 1, header.len - colon - 1),
                                        &value);
        }
        if (sc_status_is_ok(status)) {
            parsed = (sc_http_header){
                .name = sc_string_as_str(&name),
                .value = sc_string_as_str(&value),
            };
            status = sc_vec_push(out, &parsed);
            if (sc_status_is_ok(status)) {
                name = (sc_string){0};
                value = (sc_string){0};
            }
        }
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&name);
            sc_string_clear(&value);
            http_headers_clear(out);
            sc_json_destroy(array);
            return status;
        }
    }
    sc_json_destroy(array);
    return sc_status_ok();
}

static sc_status http_perform(sc_allocator *alloc,
                              sc_str method,
                              sc_str url,
                              sc_str headers_json,
                              sc_str body,
                              size_t max_bytes,
                              int64_t timeout_ms,
                              sc_string *out)
{
    sc_vec headers = {0};
    sc_http_response response = {0};
    sc_status status = sc_status_ok();

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = http_headers_from_json(alloc, headers_json, &headers);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_http_request http_request = {
        .struct_size = sizeof(http_request),
        .method = method,
        .url = url,
        .headers = headers.len == 0 ? nullptr : headers.ptr,
        .header_count = headers.len,
        .body = body,
        .user_agent = sc_str_from_cstr("smolclaw-c/0.1 tool-http"),
        .max_response_bytes = max_bytes == 0 ? 4096 : max_bytes,
        .timeout_ms = timeout_ms <= 0 ? 30000 : timeout_ms,
        .connect_timeout_ms = timeout_ms <= 0 ? 10000 : timeout_ms,
        .follow_location = false,
    };
    status = sc_http_client_perform_sync(alloc, &http_request, &response);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc,
                                    sc_str_from_parts((const char *)response.body.ptr, response.body.len),
                                    out);
    } else if (status.code == SC_ERR_UNSUPPORTED) {
        status = sc_status_unsupported("sc.http_tool.libcurl_unavailable");
    } else if (response.too_large) {
        sc_status_clear(&status);
        status = sc_status_http("sc.http_tool.response_too_large");
    } else if (response.http_status >= 400) {
        sc_status_clear(&status);
        status = sc_status_http("sc.http_tool.http_status");
    }
    sc_http_response_clear(&response);
    http_headers_clear(&headers);
    return status;
}
