#include "tools/tool_internal.h"

#include "net/http_client.h"
#include "sc/config.h"
#include "sc/time.h"
#include "sc/url.h"

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
#include "websocket_client/websocket_client.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct browser_session {
    sc_string name;
    sc_string target_id;
    sc_string cdp_session_id;
    sc_vec refs;
    bool active;
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
    ws_client_t *client;
#endif
} browser_session;

typedef struct browser_tool {
    sc_tool_impl_context base;
    sc_vec sessions;
    uint64_t next_id;
} browser_tool;

static sc_status browser_spec(void *impl, sc_tool_spec *out);
static sc_status browser_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status browser_screenshot_spec(void *impl, sc_tool_spec *out);
static sc_status browser_screenshot_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void browser_destroy(void *impl);
static sc_status browser_schema_add_timeout_ms(sc_allocator *alloc, sc_json_value *schema);
static sc_status browser_schema_add_metadata(sc_allocator *alloc, sc_json_value *schema);
static sc_status browser_schema_set_property_description(sc_allocator *alloc, sc_json_value *schema, sc_str name, sc_str description);
static sc_status browser_schema_set_action_enum(sc_allocator *alloc, sc_json_value *schema);
static sc_status browser_screenshot_schema_add_metadata(sc_allocator *alloc, sc_json_value *schema);
static sc_status browser_screenshot_schema_set_format_enum(sc_allocator *alloc, sc_json_value *schema);
static sc_status browser_config_domain_check(const browser_tool *tool, sc_str url);
static sc_status browser_get_optional_timeout_ms(const sc_tool_call *call, int64_t *out);
static sc_status browser_get_screenshot_delivery_arg(const sc_tool_call *call, sc_attachment_delivery *out);
static bool browser_action_known(sc_str action);
static bool browser_action_requires_url(sc_str action);
static bool browser_action_requires_target(sc_str action);
static bool domain_list_allows(sc_str raw_json, const sc_url *url);
static sc_status browser_diagnose(browser_tool *tool, sc_allocator *alloc, sc_tool_result *out);
static sc_status browser_open(browser_tool *tool,
                              sc_str session_name,
                              sc_str url,
                              int64_t timeout_ms,
                              sc_allocator *alloc,
                              sc_string *out);
static sc_status browser_runtime_eval(browser_tool *tool,
                                      browser_session *session,
                                      sc_str expression,
                                      sc_allocator *alloc,
                                      sc_string *out);
static sc_status browser_snapshot(browser_tool *tool, browser_session *session, sc_allocator *alloc, sc_string *out);
static sc_status browser_click(browser_tool *tool, browser_session *session, sc_str target, sc_allocator *alloc, sc_string *out);
static sc_status browser_wait(browser_tool *tool,
                              browser_session *session,
                              sc_str target,
                              int64_t timeout_ms,
                              sc_allocator *alloc,
                              sc_string *out);
static sc_status browser_capture_screenshot(browser_tool *tool, browser_session *session, sc_allocator *alloc, sc_bytes *out);
static sc_status browser_screenshot_url(browser_tool *tool,
                                        sc_str session_name,
                                        sc_str url,
                                        int64_t timeout_ms,
                                        sc_attachment_delivery delivery,
                                        sc_allocator *alloc,
                                        sc_tool_result *out);
static sc_status browser_screenshot_summary(sc_allocator *alloc,
                                            size_t bytes_len,
                                            sc_attachment_delivery delivery,
                                            sc_string *out);
static sc_status browser_attach_screenshot_result(sc_allocator *alloc,
                                                  sc_tool_result *out,
                                                  const sc_bytes *bytes,
                                                  sc_attachment_delivery delivery);
static sc_status browser_close(browser_tool *tool, sc_str session_name, sc_allocator *alloc, sc_string *out);
static browser_session *browser_find_session(browser_tool *tool, sc_str session_name);
static sc_status browser_get_or_create_session(browser_tool *tool, sc_str session_name, browser_session **out);
static sc_status browser_session_connect(browser_tool *tool, browser_session *session);
static void browser_session_clear(browser_session *session);
static void browser_session_clear_refs(browser_session *session);
static sc_status browser_session_add_ref(browser_session *session, sc_str selector);
static sc_status browser_resolve_target(browser_session *session, sc_str target, sc_str *out);
static sc_status browser_get_config_string(const browser_tool *tool, sc_str path, sc_allocator *alloc, sc_string *out);
static int64_t browser_get_config_int(const browser_tool *tool, sc_str path, int64_t fallback);
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static bool browser_cdp_url_needs_discovery(const sc_url *url);
static sc_status browser_discover_cdp_url(browser_tool *tool, sc_allocator *alloc, sc_string *out);
static bool browser_get_config_bool(const browser_tool *tool, sc_str path, bool fallback);
static sc_status browser_validate_lightpanda_url(browser_tool *tool, sc_str path, sc_str value);
static bool url_is_loopback(const sc_url *url);
#endif
static int64_t browser_deadline_from_ms(int64_t timeout_ms);
static bool browser_deadline_expired(int64_t deadline_ns);
static void browser_sleep_ms(uint32_t delay_ms);
static sc_status append_json_string(sc_string_builder *builder, sc_str value);
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static sc_status cdp_error_status(sc_allocator *alloc, const sc_json_value *root);
static sc_status append_cdp_message(sc_allocator *alloc,
                                    uint64_t id,
                                    sc_str method,
                                    sc_str params_json,
                                    sc_str session_id,
                                    sc_string *out);
#endif
static sc_status append_runtime_eval_params(sc_allocator *alloc, sc_str expression, sc_string *out);
static sc_status append_page_navigate_params(sc_allocator *alloc, sc_str url, sc_string *out);
static sc_status append_page_capture_screenshot_params(sc_allocator *alloc,
                                                       bool capture_beyond_viewport,
                                                       sc_string *out);
static sc_status append_target_create_params(sc_allocator *alloc, sc_str url, sc_string *out);
static sc_status append_target_attach_params(sc_allocator *alloc, sc_str target_id, sc_string *out);
static sc_status append_target_close_params(sc_allocator *alloc, sc_str target_id, sc_string *out);
static sc_status cdp_call(browser_tool *tool,
                          browser_session *session,
                          sc_str method,
                          sc_str params_json,
                          sc_str session_id,
                          sc_allocator *alloc,
                          sc_json_value **out);
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static bool cdp_response_matches(const sc_json_value *root, uint64_t id);
#endif
static sc_status cdp_result_string(const sc_json_value *root, sc_str key, sc_str *out);
static sc_status cdp_eval_value_string(const sc_json_value *root, sc_allocator *alloc, sc_string *out);
static sc_status json_string_field(const sc_json_value *object, sc_str key, sc_str *out);
static sc_status parse_snapshot_result(browser_session *session, sc_str json_text, sc_allocator *alloc, sc_string *out);
static sc_status base64_decode(sc_allocator *alloc, sc_str input, sc_bytes *out);
static int base64_value(char ch);
static bool command_available(const char *name);
static sc_status http_probe(sc_str url, sc_allocator *alloc, bool *out);

static const sc_tool_vtab browser_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "browser",
    .display_name = "Browser",
    .feature_flag = "SC_TOOL_BROWSER",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = browser_spec,
    .invoke = browser_invoke,
    .destroy = browser_destroy,
};

static const sc_tool_vtab browser_screenshot_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "browser_screenshot",
    .display_name = "Browser Screenshot",
    .feature_flag = "SC_TOOL_BROWSER_SCREENSHOT",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = browser_screenshot_spec,
    .invoke = browser_screenshot_invoke,
    .destroy = browser_destroy,
};

sc_status sc_tool_browser_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    browser_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(browser_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (browser_tool){.next_id = 1};
    sc_vec_init(&tool->sessions, alloc, sizeof(browser_session));
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_four_strings(alloc,
                                             sc_str_from_cstr("action"),
                                             true,
                                             sc_str_from_cstr("url"),
                                             false,
                                             sc_str_from_cstr("target"),
                                             false,
                                             sc_str_from_cstr("session"),
                                             false,
                                             &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_add_timeout_ms(alloc, tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_add_metadata(alloc, tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &browser_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        browser_destroy(tool);
    }
    return status;
}

sc_status sc_tool_browser_screenshot_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    browser_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.browser_screenshot_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(browser_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (browser_tool){.next_id = 1};
    sc_vec_init(&tool->sessions, alloc, sizeof(browser_session));
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_four_strings(alloc,
                                             sc_str_from_cstr("url"),
                                             true,
                                             sc_str_from_cstr("session"),
                                             false,
                                             sc_str_from_cstr("otp"),
                                             false,
                                             sc_str_from_cstr("format"),
                                             false,
                                             &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_add_timeout_ms(alloc, tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = browser_screenshot_schema_add_metadata(alloc, tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &browser_screenshot_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        browser_destroy(tool);
    }
    return status;
}

static sc_status browser_spec(void *impl, sc_tool_spec *out)
{
    const browser_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("browser"),
        .description = sc_str_from_cstr("tool.browser.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_NETWORK,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_BROWSER,
        .side_effect = SC_TOOL_SIDE_EFFECT_PROCESS,
        .default_autonomy = SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = sc_str_from_cstr("tool.browser.catalog"),
    };
    return sc_status_ok();
}

static sc_status browser_screenshot_spec(void *impl, sc_tool_spec *out)
{
    const browser_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_screenshot_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("browser_screenshot"),
        .description = sc_str_from_cstr("tool.browser_screenshot.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_NETWORK,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_BROWSER,
        .side_effect = SC_TOOL_SIDE_EFFECT_PROCESS,
        .default_autonomy = SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = sc_str_from_cstr("tool.browser_screenshot.catalog"),
    };
    return sc_status_ok();
}

static sc_status browser_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    browser_tool *tool = impl;
    sc_str action = {0};
    sc_str url = {0};
    sc_str target = {0};
    sc_str session_name = {0};
    sc_str otp = {0};
    sc_string output = {0};
    sc_bytes screenshot = {0};
    int64_t timeout_ms = 0;
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("action"), &action);
    }
    if (sc_status_is_ok(status) && !browser_action_known(action)) {
        status = sc_status_invalid_argument("sc.browser_tool.unknown_action");
    }
    if (sc_status_is_ok(status) && sc_str_equal(action, sc_str_from_cstr("diagnose"))) {
        return browser_diagnose(tool, alloc, out);
    }
    if (sc_status_is_ok(status) && tool->base.context.config != nullptr &&
        !sc_config_get_bool(tool->base.context.config, sc_str_from_cstr("browser.enabled"), true)) {
        status = sc_status_unsupported("sc.browser_tool.disabled");
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("url"), &url);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("target"), &target);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("session"), &session_name);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("otp"), &otp);
    }
    if (sc_status_is_ok(status)) {
        status = browser_get_optional_timeout_ms(call, &timeout_ms);
    }
    if (sc_status_is_ok(status) && browser_action_requires_url(action) && url.len == 0) {
        status = sc_status_invalid_argument("sc.browser_tool.url_required");
    }
    if (sc_status_is_ok(status) && browser_action_requires_target(action) && target.len == 0) {
        status = sc_status_invalid_argument("sc.browser_tool.target_required");
    }
    if (sc_status_is_ok(status) && browser_action_requires_url(action)) {
        status = sc_tool_security_check_ex(&tool->base,
                                           sc_str_from_cstr("browser"),
                                           SC_TOOL_RISK_NETWORK,
                                           sc_str_from_cstr(""),
                                           false,
                                           url,
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           otp);
        if (sc_status_is_ok(status)) {
            status = browser_config_domain_check(tool, url);
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_tool_security_check_ex(&tool->base,
                                           sc_str_from_cstr("browser"),
                                           SC_TOOL_RISK_NETWORK,
                                           sc_str_from_cstr(""),
                                           false,
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           otp);
    }
    if (sc_status_is_ok(status)) {
        if (sc_str_equal(action, sc_str_from_cstr("open"))) {
            status = browser_open(tool, session_name, url, timeout_ms, alloc, &output);
        } else if (sc_str_equal(action, sc_str_from_cstr("close"))) {
            status = browser_close(tool, session_name, alloc, &output);
        } else {
            browser_session *session = browser_find_session(tool, session_name);
            if (session == nullptr) {
                status = sc_status_invalid_argument("sc.browser_tool.session_not_open");
            } else if (sc_str_equal(action, sc_str_from_cstr("title")) ||
                       sc_str_equal(action, sc_str_from_cstr("get_title"))) {
                status = browser_runtime_eval(tool,
                                              session,
                                              sc_str_from_cstr("document.title"),
                                              alloc,
                                              &output);
            } else if (sc_str_equal(action, sc_str_from_cstr("text")) ||
                       sc_str_equal(action, sc_str_from_cstr("get")) ||
                       sc_str_equal(action, sc_str_from_cstr("get_text"))) {
                status = browser_runtime_eval(tool,
                                              session,
                                              sc_str_from_cstr("document.body ? document.body.innerText.slice(0, 50000) : ''"),
                                              alloc,
                                              &output);
            } else if (sc_str_equal(action, sc_str_from_cstr("snapshot"))) {
                status = browser_snapshot(tool, session, alloc, &output);
            } else if (sc_str_equal(action, sc_str_from_cstr("click"))) {
                status = browser_click(tool, session, target, alloc, &output);
            } else if (sc_str_equal(action, sc_str_from_cstr("wait"))) {
                status = browser_wait(tool, session, target, timeout_ms, alloc, &output);
            } else if (sc_str_equal(action, sc_str_from_cstr("screenshot"))) {
                status = browser_capture_screenshot(tool, session, alloc, &screenshot);
                if (sc_status_is_ok(status)) {
                    status = browser_screenshot_summary(alloc,
                                                        screenshot.len,
                                                        SC_ATTACHMENT_DELIVERY_PHOTO,
                                                        &output);
                }
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&output), true);
    }
    if (sc_status_is_ok(status) && screenshot.len > 0) {
        status = browser_attach_screenshot_result(alloc,
                                                  out,
                                                  &screenshot,
                                                  SC_ATTACHMENT_DELIVERY_PHOTO);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("browser"),
                                        action,
                                        sc_status_is_ok(status) ? sc_string_as_str(&out->output) : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_string_clear(&output);
    sc_bytes_clear(&screenshot);
    return status;
}

static sc_status browser_screenshot_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    browser_tool *tool = impl;
    sc_str url = {0};
    sc_str session_name = {0};
    sc_str otp = {0};
    int64_t timeout_ms = 0;
    sc_attachment_delivery delivery = SC_ATTACHMENT_DELIVERY_PHOTO;
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_screenshot_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("url"), &url);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("session"), &session_name);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("otp"), &otp);
    }
    if (sc_status_is_ok(status)) {
        status = browser_get_optional_timeout_ms(call, &timeout_ms);
    }
    if (sc_status_is_ok(status)) {
        status = browser_get_screenshot_delivery_arg(call, &delivery);
    }
    if (sc_status_is_ok(status) && tool->base.context.config != nullptr &&
        !sc_config_get_bool(tool->base.context.config, sc_str_from_cstr("browser.enabled"), true)) {
        status = sc_status_unsupported("sc.browser_screenshot_tool.disabled");
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check_ex(&tool->base,
                                           sc_str_from_cstr("browser_screenshot"),
                                           SC_TOOL_RISK_NETWORK,
                                           sc_str_from_cstr(""),
                                           false,
                                           url,
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           otp);
        if (sc_status_is_ok(status)) {
            status = browser_config_domain_check(tool, url);
        }
    }
    if (sc_status_is_ok(status)) {
        status = browser_screenshot_url(tool, session_name, url, timeout_ms, delivery, alloc, out);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("browser_screenshot"),
                                        url,
                                        sc_status_is_ok(status) ? sc_string_as_str(&out->output) : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    return status;
}

static void browser_destroy(void *impl)
{
    browser_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    for (size_t i = 0; i < tool->sessions.len; i += 1) {
        browser_session *session = sc_vec_at(&tool->sessions, i);
        browser_session_clear(session);
    }
    sc_vec_clear(&tool->sessions);
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(browser_tool));
}

static sc_status browser_schema_add_timeout_ms(sc_allocator *alloc, sc_json_value *schema)
{
    sc_json_value *properties = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("properties"));
    sc_json_value *property = nullptr;
    sc_json_value *type = nullptr;
    sc_json_value *minimum = nullptr;
    sc_status status;

    if (properties == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.schema_invalid");
    }

    status = sc_json_object_new(alloc, &property);
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(alloc, sc_str_from_cstr("integer"), &type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(property, sc_str_from_cstr("type"), type);
        type = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_number_new(alloc, 1.0, &minimum);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(property, sc_str_from_cstr("minimum"), minimum);
        minimum = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(properties, sc_str_from_cstr("timeout_ms"), property);
        property = nullptr;
    }
    sc_json_destroy(minimum);
    sc_json_destroy(type);
    sc_json_destroy(property);
    return status;
}

static sc_status browser_schema_add_metadata(sc_allocator *alloc, sc_json_value *schema)
{
    sc_status status = browser_schema_set_action_enum(alloc, schema);

    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("action"),
            sc_str_from_cstr("Browser action to run. Use open with url first; use title, text, snapshot, click, wait, or close after a session exists; use diagnose for local backend checks."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(alloc,
                                                         schema,
                                                         sc_str_from_cstr("url"),
                                                         sc_str_from_cstr("Required only for action=open."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("target"),
            sc_str_from_cstr("CSS selector or snapshot @ref. Required for click and selector wait."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("session"),
            sc_str_from_cstr("Optional browser session name. Omit to use the default session."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("timeout_ms"),
            sc_str_from_cstr("Optional positive timeout in milliseconds for open navigation or wait actions. Overrides browser Lightpanda timeout config for this request."));
    }
    return status;
}

static sc_status browser_screenshot_schema_add_metadata(sc_allocator *alloc, sc_json_value *schema)
{
    sc_status status = browser_schema_set_property_description(
        alloc,
        schema,
        sc_str_from_cstr("url"),
        sc_str_from_cstr("HTTP or HTTPS URL to open before capturing a PNG screenshot."));
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("session"),
            sc_str_from_cstr("Optional transient browser session name. Omit to use the default screenshot session."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("otp"),
            sc_str_from_cstr("Optional one-time policy code when OTP gating is enabled for browser screenshots."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("timeout_ms"),
            sc_str_from_cstr("Optional positive timeout in milliseconds for opening the URL before capture."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_schema_set_property_description(
            alloc,
            schema,
            sc_str_from_cstr("format"),
            sc_str_from_cstr("Optional upload format for the screenshot attachment: photo for chat preview, document for original PNG file delivery."));
    }
    if (sc_status_is_ok(status)) {
        status = browser_screenshot_schema_set_format_enum(alloc, schema);
    }
    return status;
}

static sc_status browser_schema_set_property_description(sc_allocator *alloc, sc_json_value *schema, sc_str name, sc_str description)
{
    sc_json_value *properties = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("properties"));
    sc_json_value *property = properties == nullptr ? nullptr : sc_json_object_get(properties, name);
    sc_json_value *value = nullptr;
    sc_status status;

    if (property == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.schema_invalid");
    }

    status = sc_json_string_new(alloc, description, &value);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(property, sc_str_from_cstr("description"), value);
        value = nullptr;
    }
    sc_json_destroy(value);
    return status;
}

static sc_status browser_screenshot_schema_set_format_enum(sc_allocator *alloc, sc_json_value *schema)
{
    static const char *const formats[] = {
        "photo",
        "document",
    };
    sc_json_value *properties = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("properties"));
    sc_json_value *format = properties == nullptr ? nullptr : sc_json_object_get(properties, sc_str_from_cstr("format"));
    sc_json_value *values = nullptr;
    sc_status status;

    if (format == nullptr) {
        return sc_status_invalid_argument("sc.browser_screenshot_tool.schema_invalid");
    }

    status = sc_json_array_new(alloc, &values);
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(formats); i += 1) {
        sc_json_value *value = nullptr;
        status = sc_json_string_new(alloc, sc_str_from_cstr(formats[i]), &value);
        if (sc_status_is_ok(status)) {
            status = sc_json_array_append(values, value);
            if (sc_status_is_ok(status)) {
                value = nullptr;
            }
        }
        sc_json_destroy(value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(format, sc_str_from_cstr("enum"), values);
        values = nullptr;
    }
    sc_json_destroy(values);
    return status;
}

static sc_status browser_schema_set_action_enum(sc_allocator *alloc, sc_json_value *schema)
{
    static const char *const actions[] = {
        "open",
        "title",
        "get_title",
        "text",
        "get",
        "get_text",
        "snapshot",
        "click",
        "wait",
        "screenshot",
        "close",
        "diagnose",
    };
    sc_json_value *properties = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("properties"));
    sc_json_value *action = properties == nullptr ? nullptr : sc_json_object_get(properties, sc_str_from_cstr("action"));
    sc_json_value *values = nullptr;
    sc_status status;

    if (action == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.schema_invalid");
    }

    status = sc_json_array_new(alloc, &values);
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(actions); i += 1) {
        sc_json_value *value = nullptr;
        status = sc_json_string_new(alloc, sc_str_from_cstr(actions[i]), &value);
        if (sc_status_is_ok(status)) {
            status = sc_json_array_append(values, value);
            if (sc_status_is_ok(status)) {
                value = nullptr;
            }
        }
        sc_json_destroy(value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(action, sc_str_from_cstr("enum"), values);
        values = nullptr;
    }
    sc_json_destroy(values);
    return status;
}

static sc_status browser_diagnose(browser_tool *tool, sc_allocator *alloc, sc_tool_result *out)
{
    sc_string_builder builder = {0};
    sc_string diagnostic = {0};
    sc_string version_url = {0};
    bool reachable = false;
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "lightpanda=");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, command_available("lightpanda") ? "available\n" : "missing\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "websocket-client=");
    }
    if (sc_status_is_ok(status)) {
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
        status = sc_string_builder_append_cstr(&builder, "available\n");
#else
        status = sc_string_builder_append_cstr(&builder, "missing\n");
#endif
    }
    if (sc_status_is_ok(status)) {
        status = browser_get_config_string(tool,
                                           sc_str_from_cstr("browser.lightpanda.version_url"),
                                           alloc,
                                           &version_url);
    }
    if (sc_status_is_ok(status)) {
        status = http_probe(sc_string_as_str(&version_url), alloc, &reachable);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_status_ok();
            reachable = false;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "version-endpoint=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, reachable ? "reachable" : "unreachable");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &diagnostic);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&diagnostic), true);
    }
    sc_string_clear(&version_url);
    sc_string_clear(&diagnostic);
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("browser"),
                                        sc_str_from_cstr("diagnose"),
                                        sc_str_from_cstr("diagnostic"),
                                        sc_status_is_ok(status),
                                        status);
    return status;
}

static sc_status browser_open(browser_tool *tool,
                              sc_str session_name,
                              sc_str url,
                              int64_t timeout_ms,
                              sc_allocator *alloc,
                              sc_string *out)
{
    browser_session *session = nullptr;
    sc_json_value *root = nullptr;
    sc_str target_id = {0};
    sc_str cdp_session_id = {0};
    sc_string params = {0};
    sc_string ready = {0};
    sc_string_builder builder = {0};
    sc_status status;

    status = browser_get_or_create_session(tool, session_name, &session);
    if (sc_status_is_ok(status)) {
        browser_session_clear_refs(session);
        sc_string_clear(&session->target_id);
        sc_string_clear(&session->cdp_session_id);
        status = browser_session_connect(tool, session);
    }
    if (sc_status_is_ok(status)) {
        status = append_target_create_params(alloc, sc_str_from_cstr("about:blank"), &params);
    }
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Target.createTarget"),
                          sc_string_as_str(&params),
                          sc_str_from_cstr(""),
                          alloc,
                          &root);
    }
    if (sc_status_is_ok(status)) {
        status = cdp_result_string(root, sc_str_from_cstr("targetId"), &target_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(tool->base.alloc, target_id, &session->target_id);
    }
    sc_json_destroy(root);
    root = nullptr;
    sc_string_clear(&params);
    if (sc_status_is_ok(status)) {
        status = append_target_attach_params(alloc, sc_string_as_str(&session->target_id), &params);
    }
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Target.attachToTarget"),
                          sc_string_as_str(&params),
                          sc_str_from_cstr(""),
                          alloc,
                          &root);
    }
    if (sc_status_is_ok(status)) {
        status = cdp_result_string(root, sc_str_from_cstr("sessionId"), &cdp_session_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(tool->base.alloc, cdp_session_id, &session->cdp_session_id);
    }
    sc_json_destroy(root);
    root = nullptr;
    sc_string_clear(&params);
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Page.enable"),
                          sc_str_from_cstr("{}"),
                          sc_string_as_str(&session->cdp_session_id),
                          alloc,
                          &root);
    }
    sc_json_destroy(root);
    root = nullptr;
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Runtime.enable"),
                          sc_str_from_cstr("{}"),
                          sc_string_as_str(&session->cdp_session_id),
                          alloc,
                          &root);
    }
    sc_json_destroy(root);
    root = nullptr;
    if (sc_status_is_ok(status)) {
        status = append_page_navigate_params(alloc, url, &params);
    }
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Page.navigate"),
                          sc_string_as_str(&params),
                          sc_string_as_str(&session->cdp_session_id),
                          alloc,
                          &root);
    }
    sc_json_destroy(root);
    root = nullptr;
    sc_string_clear(&params);
    if (sc_status_is_ok(status)) {
        status = browser_wait(tool, session, sc_str_from_cstr("load"), timeout_ms, alloc, &ready);
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "opened url=");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, url);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " session=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&session->name));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " state=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&ready));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, out);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    sc_string_clear(&ready);
    return status;
}

static sc_status browser_runtime_eval(browser_tool *tool,
                                      browser_session *session,
                                      sc_str expression,
                                      sc_allocator *alloc,
                                      sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_string params = {0};
    sc_status status;

    status = append_runtime_eval_params(alloc, expression, &params);
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Runtime.evaluate"),
                          sc_string_as_str(&params),
                          sc_string_as_str(&session->cdp_session_id),
                          alloc,
                          &root);
    }
    if (sc_status_is_ok(status)) {
        status = cdp_eval_value_string(root, alloc, out);
    }
    sc_json_destroy(root);
    sc_string_clear(&params);
    return status;
}

static sc_status browser_snapshot(browser_tool *tool, browser_session *session, sc_allocator *alloc, sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_str markdown = {0};
    sc_status status = cdp_call(tool,
                                session,
                                sc_str_from_cstr("LP.getMarkdown"),
                                sc_str_from_cstr("{}"),
                                sc_string_as_str(&session->cdp_session_id),
                                alloc,
                                &root);
    if (sc_status_is_ok(status) &&
        (cdp_result_string(root, sc_str_from_cstr("markdown"), &markdown).code == SC_OK ||
         cdp_result_string(root, sc_str_from_cstr("text"), &markdown).code == SC_OK ||
         cdp_result_string(root, sc_str_from_cstr("content"), &markdown).code == SC_OK)) {
        status = sc_string_from_str(alloc, markdown, out);
        sc_json_destroy(root);
        return status;
    }
    sc_json_destroy(root);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
    }
    sc_string raw = {0};
    status = browser_runtime_eval(tool,
                                  session,
                                  sc_str_from_cstr("(()=>{const esc=s=>String(s).replace(/\\\\/g,'\\\\\\\\').replace(/\"/g,'\\\\\"');const refs=[];const lines=[document.title||'',location.href||'',document.body?document.body.innerText.slice(0,40000):''];Array.from(document.querySelectorAll('a,button,input,textarea,select,[role=button]')).slice(0,50).forEach((el,i)=>{const label=(el.innerText||el.value||el.getAttribute('aria-label')||el.href||el.name||el.id||el.tagName||'').trim();let sel=el.id?'#'+esc(el.id):(el.name?el.tagName.toLowerCase()+'[name=\"'+esc(el.name)+'\"]':el.tagName.toLowerCase()+':nth-of-type(1)');refs.push(sel);lines.push('@'+(i+1)+' '+label);});return JSON.stringify({text:lines.join('\\n'),refs});})()"),
                                  alloc,
                                  &raw);
    if (sc_status_is_ok(status)) {
        status = parse_snapshot_result(session, sc_string_as_str(&raw), alloc, out);
    }
    sc_string_clear(&raw);
    return status;
}

static sc_status browser_click(browser_tool *tool, browser_session *session, sc_str target, sc_allocator *alloc, sc_string *out)
{
    sc_str selector = {0};
    sc_string expression = {0};
    sc_string_builder builder = {0};
    sc_string ignored = {0};
    sc_status status = browser_resolve_target(session, target, &selector);

    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "(()=>{const el=document.querySelector(");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, selector);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ");if(!el){return 'not_found';}el.click();return 'clicked';})()");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &expression);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = browser_runtime_eval(tool, session, sc_string_as_str(&expression), alloc, &ignored);
    }
    if (sc_status_is_ok(status) && !sc_str_equal(sc_string_as_str(&ignored), sc_str_from_cstr("clicked"))) {
        status = sc_status_invalid_argument("sc.browser_tool.target_not_found");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "clicked", out);
    }
    sc_string_clear(&ignored);
    sc_string_clear(&expression);
    return status;
}

static sc_status browser_wait(browser_tool *tool,
                              browser_session *session,
                              sc_str target,
                              int64_t request_timeout_ms,
                              sc_allocator *alloc,
                              sc_string *out)
{
    sc_string expression = {0};
    sc_string_builder builder = {0};
    sc_string value = {0};
    int64_t timeout_ms = 0;
    int64_t deadline_ns = 0;
    sc_status status = sc_status_ok();

    if (target.len == 0 ||
        sc_str_equal(target, sc_str_from_cstr("load")) ||
        sc_str_equal(target, sc_str_from_cstr("networkidle"))) {
        timeout_ms = request_timeout_ms > 0 ?
                         request_timeout_ms :
                         browser_get_config_int(tool, sc_str_from_cstr("browser.lightpanda.navigation_timeout_ms"), 30000);
        deadline_ns = browser_deadline_from_ms(timeout_ms);
        while (sc_status_is_ok(status)) {
            sc_string_clear(&value);
            status = browser_runtime_eval(tool, session, sc_str_from_cstr("document.readyState"), alloc, &value);
            if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&value), sc_str_from_cstr("complete"))) {
                break;
            }
            if (sc_status_is_ok(status) && browser_deadline_expired(deadline_ns)) {
                status = sc_status_timeout("sc.browser_tool.wait_timeout");
                break;
            }
            if (sc_status_is_ok(status)) {
                browser_sleep_ms(100);
            }
        }
    } else {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "document.querySelector(");
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, target);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ") ? 'ready' : 'missing'");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &expression);
        } else {
            sc_string_builder_clear(&builder);
        }
        timeout_ms = request_timeout_ms > 0 ?
                         request_timeout_ms :
                         browser_get_config_int(tool, sc_str_from_cstr("browser.lightpanda.command_timeout_ms"), 5000);
        deadline_ns = browser_deadline_from_ms(timeout_ms);
        while (sc_status_is_ok(status)) {
            sc_string_clear(&value);
            status = browser_runtime_eval(tool, session, sc_string_as_str(&expression), alloc, &value);
            if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&value), sc_str_from_cstr("ready"))) {
                break;
            }
            if (sc_status_is_ok(status) && browser_deadline_expired(deadline_ns)) {
                status = sc_status_timeout("sc.browser_tool.wait_timeout");
                break;
            }
            if (sc_status_is_ok(status)) {
                browser_sleep_ms(100);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "ready", out);
    }
    sc_string_clear(&value);
    sc_string_clear(&expression);
    return status;
}

static sc_status browser_capture_screenshot(browser_tool *tool, browser_session *session, sc_allocator *alloc, sc_bytes *out)
{
    sc_json_value *root = nullptr;
    sc_string params = {0};
    sc_str data = {0};
    int64_t max_bytes = 0;
    sc_status status;

    if (tool == nullptr || session == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.screenshot_invalid_argument");
    }
    status = append_page_capture_screenshot_params(alloc, true, &params);
    if (sc_status_is_ok(status)) {
        status = cdp_call(tool,
                          session,
                          sc_str_from_cstr("Page.captureScreenshot"),
                          sc_string_as_str(&params),
                          sc_string_as_str(&session->cdp_session_id),
                          alloc,
                          &root);
    }
    if (!sc_status_is_ok(status) && status.error_key != nullptr &&
        strcmp(status.error_key, "sc.browser_tool.cdp_error") == 0) {
        sc_status_clear(&status);
        sc_json_destroy(root);
        root = nullptr;
        sc_string_clear(&params);
        status = append_page_capture_screenshot_params(alloc, false, &params);
        if (sc_status_is_ok(status)) {
            status = cdp_call(tool,
                              session,
                              sc_str_from_cstr("Page.captureScreenshot"),
                              sc_string_as_str(&params),
                              sc_string_as_str(&session->cdp_session_id),
                              alloc,
                              &root);
        }
    }
    if (sc_status_is_ok(status)) {
        status = cdp_result_string(root, sc_str_from_cstr("data"), &data);
    }
    if (sc_status_is_ok(status)) {
        status = base64_decode(alloc, data, out);
    }
    max_bytes = browser_get_config_int(tool, sc_str_from_cstr("browser.screenshot.max_bytes"), 4'194'304);
    if (sc_status_is_ok(status) && max_bytes > 0 && out->len > (size_t)max_bytes) {
        sc_bytes_clear(out);
        status = sc_status_parse("sc.browser_tool.screenshot_too_large");
    }
    sc_json_destroy(root);
    sc_string_clear(&params);
    return status;
}

static sc_status browser_screenshot_url(browser_tool *tool,
                                        sc_str session_name,
                                        sc_str url,
                                        int64_t timeout_ms,
                                        sc_attachment_delivery delivery,
                                        sc_allocator *alloc,
                                        sc_tool_result *out)
{
    browser_session *session = nullptr;
    sc_string opened = {0};
    sc_string summary = {0};
    sc_string closed = {0};
    sc_bytes screenshot = {0};
    sc_status status;

    status = browser_open(tool, session_name, url, timeout_ms, alloc, &opened);
    if (sc_status_is_ok(status)) {
        session = browser_find_session(tool, session_name);
        if (session == nullptr) {
            status = sc_status_invalid_argument("sc.browser_tool.session_not_open");
        }
    }
    if (sc_status_is_ok(status)) {
        status = browser_capture_screenshot(tool, session, alloc, &screenshot);
    }
    if (sc_status_is_ok(status)) {
        status = browser_screenshot_summary(alloc, screenshot.len, delivery, &summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&summary), true);
    }
    if (sc_status_is_ok(status)) {
        status = browser_attach_screenshot_result(alloc, out, &screenshot, delivery);
    }
    if (session != nullptr) {
        sc_status close_status = browser_close(tool, session_name, alloc, &closed);
        if (!sc_status_is_ok(close_status)) {
            sc_status_clear(&close_status);
        }
    }
    sc_string_clear(&closed);
    sc_bytes_clear(&screenshot);
    sc_string_clear(&summary);
    sc_string_clear(&opened);
    return status;
}

static sc_status browser_screenshot_summary(sc_allocator *alloc,
                                            size_t bytes_len,
                                            sc_attachment_delivery delivery,
                                            sc_string *out)
{
    sc_string_builder builder = {0};
    char bytes_text[32] = {0};
    int written = snprintf(bytes_text, sizeof(bytes_text), "%llu", (unsigned long long)bytes_len);
    sc_status status;

    if (written <= 0 || (size_t)written >= sizeof(bytes_text)) {
        return sc_status_io("sc.browser_tool.screenshot_size_format_failed");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "screenshot content_type=image/png bytes=");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, bytes_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " delivery=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(
            &builder,
            delivery == SC_ATTACHMENT_DELIVERY_DOCUMENT ? "document" : "photo");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status browser_attach_screenshot_result(sc_allocator *alloc,
                                                  sc_tool_result *out,
                                                  const sc_bytes *bytes,
                                                  sc_attachment_delivery delivery)
{
    sc_string content_type = {0};
    sc_string filename = {0};
    sc_bytes owned = {0};
    sc_status status;

    if (out == nullptr || bytes == nullptr || bytes->len == 0) {
        return sc_status_invalid_argument("sc.browser_tool.screenshot_invalid_argument");
    }
    status = sc_string_from_cstr(alloc, "image/png", &content_type);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "browser-screenshot.png", &filename);
    }
    if (sc_status_is_ok(status)) {
        status = sc_bytes_from_buf(alloc, sc_buf_from_parts(bytes->ptr, bytes->len), &owned);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&content_type);
        sc_string_clear(&filename);
        sc_bytes_clear(&owned);
        return status;
    }
    sc_string_clear(&out->attachment_content_type);
    sc_string_clear(&out->attachment_filename);
    sc_bytes_clear(&out->attachment_bytes);
    out->attachment_content_type = content_type;
    out->attachment_filename = filename;
    out->attachment_bytes = owned;
    out->attachment_delivery = delivery;
    return sc_status_ok();
}

static sc_status browser_close(browser_tool *tool, sc_str session_name, sc_allocator *alloc, sc_string *out)
{
    browser_session *session = browser_find_session(tool, session_name);
    sc_json_value *root = nullptr;
    sc_string params = {0};
    sc_status status = sc_status_ok();

    if (session == nullptr) {
        return sc_string_from_cstr(alloc, "closed", out);
    }
    if (session->target_id.len > 0) {
        status = append_target_close_params(alloc, sc_string_as_str(&session->target_id), &params);
        if (sc_status_is_ok(status)) {
            status = cdp_call(tool,
                              session,
                              sc_str_from_cstr("Target.closeTarget"),
                              sc_string_as_str(&params),
                              sc_str_from_cstr(""),
                              alloc,
                              &root);
        }
        sc_json_destroy(root);
        sc_string_clear(&params);
    }
    browser_session_clear(session);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "closed", out);
    }
    return status;
}

static sc_status browser_config_domain_check(const browser_tool *tool, sc_str url)
{
    sc_string raw = {0};
    sc_url parsed = {0};
    sc_status status;

    if (tool == nullptr || tool->base.context.config == nullptr || url.ptr == nullptr || url.len == 0) {
        return sc_status_ok();
    }
    status = sc_config_get_prop(tool->base.context.config,
                                sc_str_from_cstr("browser.allowed_domains"),
                                tool->base.alloc,
                                &raw);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    status = sc_url_parse(tool->base.alloc, url, &parsed);
    if (sc_status_is_ok(status) && !domain_list_allows(sc_string_as_str(&raw), &parsed)) {
        status = sc_status_security_denied("sc.browser_tool.domain_not_allowed");
    }
    sc_url_clear(&parsed);
    sc_string_clear(&raw);
    return status;
}

static sc_status browser_get_optional_timeout_ms(const sc_tool_call *call, int64_t *out)
{
    sc_json_value *value = nullptr;
    double parsed = 0.0;
    int64_t timeout_ms = 0;

    if (call == nullptr || call->args == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    value = sc_json_object_get(call->args, sc_str_from_cstr("timeout_ms"));
    if (value == nullptr || sc_json_is_null(value)) {
        *out = 0;
        return sc_status_ok();
    }
    if (!sc_json_as_number(value, &parsed) || parsed != parsed || parsed < 1.0 ||
        parsed > (double)(INT64_MAX / 1'000'000) ||
        (double)(int64_t)parsed != parsed) {
        return sc_status_invalid_argument("sc.browser_tool.timeout_invalid");
    }
    timeout_ms = (int64_t)parsed;
    *out = timeout_ms;
    return sc_status_ok();
}

static sc_status browser_get_screenshot_delivery_arg(const sc_tool_call *call, sc_attachment_delivery *out)
{
    sc_str format = {0};
    sc_status status;

    if (call == nullptr || call->args == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_screenshot_tool.invalid_argument");
    }
    *out = SC_ATTACHMENT_DELIVERY_PHOTO;
    status = sc_tool_get_optional_string_arg(call, sc_str_from_cstr("format"), &format);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (format.len == 0 || sc_str_equal(format, sc_str_from_cstr("photo"))) {
        *out = SC_ATTACHMENT_DELIVERY_PHOTO;
        return sc_status_ok();
    }
    if (sc_str_equal(format, sc_str_from_cstr("document"))) {
        *out = SC_ATTACHMENT_DELIVERY_DOCUMENT;
        return sc_status_ok();
    }
    return sc_status_invalid_argument("sc.browser_screenshot_tool.format_invalid");
}

static bool browser_action_known(sc_str action)
{
    return sc_str_equal(action, sc_str_from_cstr("open")) ||
           sc_str_equal(action, sc_str_from_cstr("snapshot")) ||
           sc_str_equal(action, sc_str_from_cstr("click")) ||
           sc_str_equal(action, sc_str_from_cstr("wait")) ||
           sc_str_equal(action, sc_str_from_cstr("screenshot")) ||
           sc_str_equal(action, sc_str_from_cstr("text")) ||
           sc_str_equal(action, sc_str_from_cstr("get")) ||
           sc_str_equal(action, sc_str_from_cstr("get_text")) ||
           sc_str_equal(action, sc_str_from_cstr("title")) ||
           sc_str_equal(action, sc_str_from_cstr("get_title")) ||
           sc_str_equal(action, sc_str_from_cstr("close")) ||
           sc_str_equal(action, sc_str_from_cstr("diagnose"));
}

static bool browser_action_requires_url(sc_str action)
{
    return sc_str_equal(action, sc_str_from_cstr("open"));
}

static bool browser_action_requires_target(sc_str action)
{
    return sc_str_equal(action, sc_str_from_cstr("click")) ||
           sc_str_equal(action, sc_str_from_cstr("wait"));
}

static bool domain_list_allows(sc_str raw_json, const sc_url *url)
{
    sc_json_value *array = nullptr;
    sc_json_parse_error error = {0};
    bool allowed = false;

    if (raw_json.ptr == nullptr || raw_json.len == 0 || url == nullptr ||
        !sc_status_is_ok(sc_json_parse(sc_allocator_heap(), raw_json, &array, &error))) {
        return true;
    }
    for (size_t i = 0; i < sc_json_array_len(array); i += 1) {
        sc_str domain = {0};
        if (!sc_json_as_str(sc_json_array_get(array, i), &domain)) {
            continue;
        }
        if (sc_str_equal(domain, sc_str_from_cstr("*")) ||
            sc_url_host_matches_domain(url, domain)) {
            allowed = true;
            break;
        }
    }
    sc_json_destroy(array);
    return allowed;
}

static browser_session *browser_find_session(browser_tool *tool, sc_str session_name)
{
    sc_str normalized = session_name.len == 0 ? sc_str_from_cstr("default") : session_name;
    if (tool == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < tool->sessions.len; i += 1) {
        browser_session *session = sc_vec_at(&tool->sessions, i);
        if (session != nullptr && session->active &&
            sc_str_equal(sc_string_as_str(&session->name), normalized)) {
            return session;
        }
    }
    return nullptr;
}

static sc_status browser_get_or_create_session(browser_tool *tool, sc_str session_name, browser_session **out)
{
    browser_session fresh = {0};
    browser_session *session = browser_find_session(tool, session_name);
    sc_str normalized = session_name.len == 0 ? sc_str_from_cstr("default") : session_name;
    sc_status status;

    if (out == nullptr || tool == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    if (session != nullptr) {
        *out = session;
        return sc_status_ok();
    }
    for (size_t i = 0; i < tool->sessions.len; i += 1) {
        session = sc_vec_at(&tool->sessions, i);
        if (session != nullptr && !session->active) {
            browser_session_clear(session);
            status = sc_string_from_str(tool->base.alloc, normalized, &session->name);
            if (sc_status_is_ok(status)) {
                sc_vec_init(&session->refs, tool->base.alloc, sizeof(sc_string));
                session->active = true;
                *out = session;
            }
            return status;
        }
    }
    status = sc_string_from_str(tool->base.alloc, normalized, &fresh.name);
    if (sc_status_is_ok(status)) {
        sc_vec_init(&fresh.refs, tool->base.alloc, sizeof(sc_string));
        fresh.active = true;
        status = sc_vec_push(&tool->sessions, &fresh);
    }
    if (!sc_status_is_ok(status)) {
        browser_session_clear(&fresh);
        return status;
    }
    *out = sc_vec_at(&tool->sessions, tool->sessions.len - 1);
    return sc_status_ok();
}

static sc_status browser_session_connect(browser_tool *tool, browser_session *session)
{
#ifndef SC_HAVE_TP_WEBSOCKET_CLIENT
    (void)tool;
    (void)session;
    return sc_status_unsupported("sc.browser_tool.websocket_client_unavailable");
#else
    sc_string cdp_url = {0};
    sc_string path = {0};
    sc_url parsed = {0};
    sc_string_builder builder = {0};
    sc_status status;
    bool secure = false;

    if (tool == nullptr || session == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    if (session->client != nullptr) {
        ws_client_close(session->client);
        ws_client_destroy(session->client);
        session->client = nullptr;
    }
    status = browser_get_config_string(tool,
                                       sc_str_from_cstr("browser.lightpanda.cdp_url"),
                                       tool->base.alloc,
                                       &cdp_url);
    if (sc_status_is_ok(status)) {
        status = browser_validate_lightpanda_url(tool,
                                                 sc_str_from_cstr("browser.lightpanda.cdp_url"),
                                                 sc_string_as_str(&cdp_url));
    }
    if (sc_status_is_ok(status)) {
        status = sc_url_parse(tool->base.alloc, sc_string_as_str(&cdp_url), &parsed);
    }
    if (sc_status_is_ok(status) && browser_cdp_url_needs_discovery(&parsed)) {
        sc_string discovered_url = {0};
        sc_status discover_status = browser_discover_cdp_url(tool, tool->base.alloc, &discovered_url);
        if (sc_status_is_ok(discover_status)) {
            sc_url_clear(&parsed);
            sc_string_clear(&cdp_url);
            cdp_url = discovered_url;
            discovered_url = (sc_string){0};
            status = browser_validate_lightpanda_url(tool,
                                                     sc_str_from_cstr("browser.lightpanda.cdp_url"),
                                                     sc_string_as_str(&cdp_url));
            if (sc_status_is_ok(status)) {
                status = sc_url_parse(tool->base.alloc, sc_string_as_str(&cdp_url), &parsed);
            }
        } else if (discover_status.code == SC_ERR_SECURITY_DENIED) {
            status = discover_status;
        } else {
            sc_status_clear(&discover_status);
        }
        sc_string_clear(&discovered_url);
    }
    if (sc_status_is_ok(status)) {
        secure = sc_str_equal(sc_string_as_str(&parsed.scheme), sc_str_from_cstr("wss"));
        if (!secure && !sc_str_equal(sc_string_as_str(&parsed.scheme), sc_str_from_cstr("ws"))) {
            status = sc_status_invalid_argument("sc.browser_tool.invalid_cdp_url");
        }
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, tool->base.alloc);
        status = sc_string_builder_append(&builder, sc_string_as_str(&parsed.path));
        if (sc_status_is_ok(status) && parsed.query.len > 0) {
            status = sc_string_builder_append_cstr(&builder, "?");
        }
        if (sc_status_is_ok(status) && parsed.query.len > 0) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&parsed.query));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &path);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    if (sc_status_is_ok(status)) {
        session->client = ws_client_create();
        if (session->client == nullptr) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status)) {
        uint16_t port = parsed.has_port ? parsed.port : (secure ? 443 : 80);
        bool connected = secure ?
                             ws_client_connect_secure(session->client, parsed.host.ptr, port, path.ptr) :
                             ws_client_connect(session->client, parsed.host.ptr, port, path.ptr);
        if (!connected) {
            status = sc_status_make(SC_ERR_IO,
                                    "sc.browser_tool.cdp_connect_failed",
                                    "CDP websocket connection failed; start Lightpanda or Chromium remote debugging, or set browser.lightpanda.cdp_url to the running local endpoint.");
        }
    }
    if (!sc_status_is_ok(status) && session->client != nullptr) {
        ws_client_destroy(session->client);
        session->client = nullptr;
    }
    sc_url_clear(&parsed);
    sc_string_clear(&path);
    sc_string_clear(&cdp_url);
    return status;
#endif
}

static void browser_session_clear(browser_session *session)
{
    if (session == nullptr) {
        return;
    }
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
    if (session->client != nullptr) {
        ws_client_close(session->client);
        ws_client_destroy(session->client);
    }
#endif
    browser_session_clear_refs(session);
    sc_vec_clear(&session->refs);
    sc_string_clear(&session->name);
    sc_string_clear(&session->target_id);
    sc_string_clear(&session->cdp_session_id);
    *session = (browser_session){0};
}

static void browser_session_clear_refs(browser_session *session)
{
    if (session == nullptr) {
        return;
    }
    for (size_t i = 0; i < session->refs.len; i += 1) {
        sc_string *ref = sc_vec_at(&session->refs, i);
        sc_string_clear(ref);
    }
    session->refs.len = 0;
}

static sc_status browser_session_add_ref(browser_session *session, sc_str selector)
{
    sc_string owned = {0};
    sc_status status;
    if (session == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    status = sc_string_from_str(session->refs.alloc, selector, &owned);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&session->refs, &owned);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&owned);
    }
    return status;
}

static sc_status browser_resolve_target(browser_session *session, sc_str target, sc_str *out)
{
    char *end = nullptr;
    long index = 0;

    if (session == nullptr || out == nullptr || target.ptr == nullptr || target.len == 0) {
        return sc_status_invalid_argument("sc.browser_tool.target_required");
    }
    if (target.ptr[0] != '@') {
        *out = target;
        return sc_status_ok();
    }
    if (target.len < 2 || target.len > 16) {
        return sc_status_invalid_argument("sc.browser_tool.target_not_found");
    }
    char buffer[16] = {0};
    (void)memcpy(buffer, target.ptr + 1, target.len - 1);
    index = strtol(buffer, &end, 10);
    if (end == buffer || *end != '\0' || index <= 0 || (size_t)index > session->refs.len) {
        return sc_status_invalid_argument("sc.browser_tool.target_not_found");
    }
    sc_string *selector = sc_vec_at(&session->refs, (size_t)index - 1);
    if (selector == nullptr || selector->len == 0) {
        return sc_status_invalid_argument("sc.browser_tool.target_not_found");
    }
    *out = sc_string_as_str(selector);
    return sc_status_ok();
}

static sc_status browser_get_config_string(const browser_tool *tool, sc_str path, sc_allocator *alloc, sc_string *out)
{
    sc_status status;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    status = sc_config_get_prop(tool->base.context.config, path, alloc, out);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        if (sc_str_equal(path, sc_str_from_cstr("browser.lightpanda.cdp_url"))) {
            return sc_string_from_cstr(alloc, "ws://127.0.0.1:9222", out);
        }
        if (sc_str_equal(path, sc_str_from_cstr("browser.lightpanda.version_url"))) {
            return sc_string_from_cstr(alloc, "http://127.0.0.1:9222/json/version", out);
        }
        return sc_string_from_cstr(alloc, "", out);
    }
    return status;
}

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static bool browser_cdp_url_needs_discovery(const sc_url *url)
{
    if (url == nullptr) {
        return false;
    }
    return sc_str_equal(sc_string_as_str(&url->path), sc_str_from_cstr("/")) && url->query.len == 0;
}

static sc_status browser_discover_cdp_url(browser_tool *tool, sc_allocator *alloc, sc_string *out)
{
    sc_string version_url = {0};
    sc_http_response response = {0};
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_str websocket_url = {0};
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }

    status = browser_get_config_string(tool,
                                       sc_str_from_cstr("browser.lightpanda.version_url"),
                                       alloc,
                                       &version_url);
    if (sc_status_is_ok(status)) {
        status = browser_validate_lightpanda_url(tool,
                                                 sc_str_from_cstr("browser.lightpanda.version_url"),
                                                 sc_string_as_str(&version_url));
    }
    if (sc_status_is_ok(status)) {
        sc_http_request request = {
            .struct_size = sizeof(request),
            .method = sc_str_from_cstr("GET"),
            .url = sc_string_as_str(&version_url),
            .max_response_bytes = 65'536,
            .timeout_ms = 1000,
            .connect_timeout_ms = 1000,
            .follow_location = false,
        };
        status = sc_http_client_perform_sync(alloc, &request, &response);
    }
    if (sc_status_is_ok(status) && (response.http_status < 200 || response.http_status >= 300)) {
        status = sc_status_io("sc.browser_tool.version_probe_failed");
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_parse(alloc,
                               sc_str_from_parts((const char *)response.body.ptr, response.body.len),
                               &root,
                               &error);
    }
    if (sc_status_is_ok(status) &&
        (!sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("webSocketDebuggerUrl")), &websocket_url) ||
         websocket_url.len == 0)) {
        status = sc_status_parse("sc.browser_tool.version_missing_websocket_url");
    }
    if (sc_status_is_ok(status)) {
        status = browser_validate_lightpanda_url(tool,
                                                 sc_str_from_cstr("browser.lightpanda.cdp_url"),
                                                 websocket_url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, websocket_url, out);
    }

    sc_json_destroy(root);
    sc_http_response_clear(&response);
    sc_string_clear(&version_url);
    return status;
}
#endif

static int64_t browser_get_config_int(const browser_tool *tool, sc_str path, int64_t fallback)
{
    if (tool == nullptr) {
        return fallback;
    }
    return sc_config_get_int(tool->base.context.config, path, fallback);
}

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static bool browser_get_config_bool(const browser_tool *tool, sc_str path, bool fallback)
{
    if (tool == nullptr) {
        return fallback;
    }
    return sc_config_get_bool(tool->base.context.config, path, fallback);
}

static sc_status browser_validate_lightpanda_url(browser_tool *tool, sc_str path, sc_str value)
{
    sc_url parsed = {0};
    sc_status status = sc_url_parse(tool->base.alloc, value, &parsed);

    if (sc_status_is_ok(status) &&
        browser_get_config_bool(tool, sc_str_from_cstr("browser.lightpanda.require_loopback"), true) &&
        !url_is_loopback(&parsed)) {
        status = sc_status_security_denied("sc.browser_tool.cdp_endpoint_not_loopback");
    }
    if (sc_status_is_ok(status) && sc_str_equal(path, sc_str_from_cstr("browser.lightpanda.cdp_url")) &&
        !sc_str_equal(sc_string_as_str(&parsed.scheme), sc_str_from_cstr("ws")) &&
        !sc_str_equal(sc_string_as_str(&parsed.scheme), sc_str_from_cstr("wss"))) {
        status = sc_status_invalid_argument("sc.browser_tool.invalid_cdp_url");
    }
    sc_url_clear(&parsed);
    return status;
}

static bool url_is_loopback(const sc_url *url)
{
    sc_str host = url == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&url->host);
    if (sc_str_equal(host, sc_str_from_cstr("localhost")) ||
        sc_str_equal(host, sc_str_from_cstr("127.0.0.1"))) {
        return true;
    }
    if (host.len > 4 && memcmp(host.ptr, "127.", 4) == 0) {
        return true;
    }
    return false;
}
#endif

static int64_t browser_deadline_from_ms(int64_t timeout_ms)
{
    sc_instant now = {0};
    if (timeout_ms <= 0 || !sc_status_is_ok(sc_clock_monotonic(&now))) {
        return 0;
    }
    if (timeout_ms > INT64_MAX / 1000000) {
        timeout_ms = INT64_MAX / 1000000;
    }
    return now.ns + (timeout_ms * 1000000);
}

static bool browser_deadline_expired(int64_t deadline_ns)
{
    sc_instant now = {0};
    if (deadline_ns <= 0 || !sc_status_is_ok(sc_clock_monotonic(&now))) {
        return false;
    }
    return now.ns >= deadline_ns;
}

static void browser_sleep_ms(uint32_t delay_ms)
{
    struct timespec requested = {
        .tv_sec = (time_t)(delay_ms / 1000U),
        .tv_nsec = (long)(delay_ms % 1000U) * 1000000L,
    };
    (void)nanosleep(&requested, nullptr);
}

static sc_status append_json_string(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        unsigned char ch = (unsigned char)value.ptr[i];
        char escaped[8] = {0};
        if (ch == '"' || ch == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)ch;
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, 2));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else if (ch == '\r') {
            status = sc_string_builder_append_cstr(builder, "\\r");
        } else if (ch == '\t') {
            status = sc_string_builder_append_cstr(builder, "\\t");
        } else if (ch < 0x20U) {
            int written = snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            if (written != 6) {
                status = sc_status_io("sc.browser_tool.json_escape_failed");
            } else {
                status = sc_string_builder_append(builder, sc_str_from_parts(escaped, 6));
            }
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts((const char *)&value.ptr[i], 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static sc_status cdp_error_status(sc_allocator *alloc, const sc_json_value *root)
{
    const sc_json_value *error = root == nullptr ? nullptr : sc_json_object_get(root, sc_str_from_cstr("error"));
    sc_str message = {0};
    sc_string owned = {0};
    sc_status status;

    if (error == nullptr || !sc_json_as_str(sc_json_object_get(error, sc_str_from_cstr("message")), &message) ||
        message.len == 0) {
        return sc_status_io("sc.browser_tool.cdp_error");
    }
    if (message.len > 512) {
        message.len = 512;
    }
    status = sc_string_from_str(alloc, message, &owned);
    if (sc_status_is_ok(status)) {
        status = sc_status_make_owned(alloc, SC_ERR_IO, "sc.browser_tool.cdp_error", owned.ptr);
    }
    sc_string_clear(&owned);
    return status;
}

static sc_status append_cdp_message(sc_allocator *alloc,
                                    uint64_t id,
                                    sc_str method,
                                    sc_str params_json,
                                    sc_str session_id,
                                    sc_string *out)
{
    char id_text[32] = {0};
    sc_string_builder builder = {0};
    sc_status status;
    int written = snprintf(id_text, sizeof(id_text), "%llu", (unsigned long long)id);

    if (written <= 0 || (size_t)written >= sizeof(id_text)) {
        return sc_status_io("sc.browser_tool.id_format_failed");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"id\":");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, id_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"method\":");
    }
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, method);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"params\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, params_json.len == 0 ? sc_str_from_cstr("{}") : params_json);
    }
    if (sc_status_is_ok(status) && session_id.len > 0) {
        status = sc_string_builder_append_cstr(&builder, ",\"sessionId\":");
    }
    if (sc_status_is_ok(status) && session_id.len > 0) {
        status = append_json_string(&builder, session_id);
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
#endif

static sc_status append_runtime_eval_params(sc_allocator *alloc, sc_str expression, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"expression\":");
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, expression);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"returnByValue\":true,\"awaitPromise\":true}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_page_navigate_params(sc_allocator *alloc, sc_str url, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"url\":");
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, url);
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

static sc_status append_page_capture_screenshot_params(sc_allocator *alloc, bool capture_beyond_viewport, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(
        &builder,
        capture_beyond_viewport ? "{\"format\":\"png\",\"fromSurface\":true,\"captureBeyondViewport\":true}" : "{\"format\":\"png\"}");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_target_create_params(sc_allocator *alloc, sc_str url, sc_string *out)
{
    return append_page_navigate_params(alloc, url, out);
}

static sc_status append_target_attach_params(sc_allocator *alloc, sc_str target_id, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"targetId\":");
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, target_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"flatten\":true}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_target_close_params(sc_allocator *alloc, sc_str target_id, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"targetId\":");
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, target_id);
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

static sc_status cdp_call(browser_tool *tool,
                          browser_session *session,
                          sc_str method,
                          sc_str params_json,
                          sc_str session_id,
                          sc_allocator *alloc,
                          sc_json_value **out)
{
#ifndef SC_HAVE_TP_WEBSOCKET_CLIENT
    (void)tool;
    (void)session;
    (void)method;
    (void)params_json;
    (void)session_id;
    (void)alloc;
    (void)out;
    return sc_status_unsupported("sc.browser_tool.websocket_client_unavailable");
#else
    sc_string message = {0};
    char *buffer = nullptr;
    size_t received = 0;
    size_t max_bytes = 0;
    uint64_t id = 0;
    sc_status status;

    if (tool == nullptr || session == nullptr || session->client == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    *out = nullptr;
    id = tool->next_id;
    tool->next_id += 1;
    status = append_cdp_message(alloc, id, method, params_json, session_id, &message);
    if (sc_status_is_ok(status) &&
        !ws_client_send_text(session->client, message.ptr, message.len)) {
        status = sc_status_io("sc.browser_tool.cdp_send_failed");
    }
    max_bytes = (size_t)browser_get_config_int(tool,
                                               sc_str_from_cstr("browser.lightpanda.max_message_bytes"),
                                               1048576);
    if (sc_str_equal(method, sc_str_from_cstr("Page.captureScreenshot"))) {
        size_t screenshot_max = (size_t)browser_get_config_int(tool,
                                                               sc_str_from_cstr("browser.screenshot.max_bytes"),
                                                               4'194'304);
        size_t encoded_max = 0;
        size_t candidate = 0;
        if (!sc_size_mul_overflow(screenshot_max, 2, &encoded_max) &&
            !sc_size_add_overflow(encoded_max, 4096, &candidate) &&
            candidate > max_bytes) {
            max_bytes = candidate;
        }
    }
    if (sc_status_is_ok(status)) {
        buffer = sc_alloc(alloc, max_bytes + 1, _Alignof(char));
        if (buffer == nullptr) {
            status = sc_status_no_memory();
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < 128; i += 1) {
        sc_json_value *root = nullptr;
        sc_json_parse_error error = {0};
        if (!ws_client_receive_text(session->client, buffer, max_bytes + 1, &received)) {
            status = sc_status_io("sc.browser_tool.cdp_receive_failed");
            break;
        }
        if (received > max_bytes) {
            status = sc_status_parse("sc.browser_tool.cdp_message_too_large");
            break;
        }
        status = sc_json_parse(alloc, sc_str_from_parts(buffer, received), &root, &error);
        if (!sc_status_is_ok(status)) {
            sc_json_destroy(root);
            break;
        }
        if (!cdp_response_matches(root, id)) {
            sc_json_destroy(root);
            continue;
        }
        if (sc_json_object_get(root, sc_str_from_cstr("error")) != nullptr) {
            status = cdp_error_status(alloc, root);
            sc_json_destroy(root);
            break;
        }
        *out = root;
        break;
    }
    if (sc_status_is_ok(status) && *out == nullptr) {
        status = sc_status_timeout("sc.browser_tool.cdp_timeout");
    }
    if (buffer != nullptr) {
        sc_free(alloc, buffer, max_bytes + 1, _Alignof(char));
    }
    sc_string_clear(&message);
    return status;
#endif
}

#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
static bool cdp_response_matches(const sc_json_value *root, uint64_t id)
{
    double number = 0.0;
    if (!sc_json_as_number(sc_json_object_get(root, sc_str_from_cstr("id")), &number)) {
        return false;
    }
    return number >= 0.0 && (uint64_t)number == id;
}
#endif

static sc_status cdp_result_string(const sc_json_value *root, sc_str key, sc_str *out)
{
    const sc_json_value *result = sc_json_object_get(root, sc_str_from_cstr("result"));
    return json_string_field(result, key, out);
}

static sc_status cdp_eval_value_string(const sc_json_value *root, sc_allocator *alloc, sc_string *out)
{
    const sc_json_value *result = sc_json_object_get(root, sc_str_from_cstr("result"));
    const sc_json_value *remote = sc_json_object_get(result, sc_str_from_cstr("result"));
    sc_str text = {0};
    double number = 0.0;
    bool boolean = false;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    if (sc_json_as_str(sc_json_object_get(remote, sc_str_from_cstr("value")), &text)) {
        return sc_string_from_str(alloc, text, out);
    }
    if (sc_json_as_number(sc_json_object_get(remote, sc_str_from_cstr("value")), &number)) {
        char buffer[64] = {0};
        int written = snprintf(buffer, sizeof(buffer), "%.17g", number);
        if (written <= 0 || (size_t)written >= sizeof(buffer)) {
            return sc_status_io("sc.browser_tool.eval_format_failed");
        }
        return sc_string_from_str(alloc, sc_str_from_parts(buffer, (size_t)written), out);
    }
    if (sc_json_as_bool(sc_json_object_get(remote, sc_str_from_cstr("value")), &boolean)) {
        return sc_string_from_cstr(alloc, boolean ? "true" : "false", out);
    }
    return sc_status_parse("sc.browser_tool.eval_missing_value");
}

static sc_status json_string_field(const sc_json_value *object, sc_str key, sc_str *out)
{
    if (out == nullptr || !sc_json_as_str(sc_json_object_get(object, key), out)) {
        return sc_status_parse("sc.browser_tool.cdp_missing_string");
    }
    return sc_status_ok();
}

static sc_status parse_snapshot_result(browser_session *session, sc_str json_text, sc_allocator *alloc, sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_str text = {0};
    sc_status status = sc_json_parse(alloc, json_text, &root, &error);

    if (sc_status_is_ok(status)) {
        status = json_string_field(root, sc_str_from_cstr("text"), &text);
    }
    if (sc_status_is_ok(status)) {
        browser_session_clear_refs(session);
        sc_json_value *refs = sc_json_object_get(root, sc_str_from_cstr("refs"));
        for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(refs); i += 1) {
            sc_str selector = {0};
            if (sc_json_as_str(sc_json_array_get(refs, i), &selector) && selector.len > 0) {
                status = browser_session_add_ref(session, selector);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, text, out);
    }
    sc_json_destroy(root);
    return status;
}

static sc_status base64_decode(sc_allocator *alloc, sc_str input, sc_bytes *out)
{
    uint32_t buffer = 0;
    uint32_t bits = 0;
    bool padding = false;
    sc_status status = sc_status_ok();

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.browser_tool.base64_invalid_argument");
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
            sc_bytes_clear(out);
            return sc_status_parse("sc.browser_tool.base64_invalid");
        }
        buffer = (buffer << 6u) | (uint32_t)value;
        bits += 6u;
        if (bits >= 8u) {
            uint8_t byte = 0;
            bits -= 8u;
            byte = (uint8_t)((buffer >> bits) & 0xFFu);
            status = sc_bytes_append(out, sc_buf_from_parts(&byte, 1));
            if (!sc_status_is_ok(status)) {
                sc_bytes_clear(out);
                return status;
            }
            buffer &= (1u << bits) - 1u;
        }
    }
    if (out->len == 0) {
        sc_bytes_clear(out);
        return sc_status_parse("sc.browser_tool.base64_empty");
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

static bool command_available(const char *name)
{
    const char *path = nullptr;
    size_t name_len = name == nullptr ? 0 : strlen(name);

    if (name_len == 0) {
        return false;
    }
    if (strchr(name, '/') != nullptr) {
        return access(name, X_OK) == 0;
    }
    path = getenv("PATH");
    while (path != nullptr && *path != '\0') {
        const char *end = strchr(path, ':');
        size_t dir_len = end == nullptr ? strlen(path) : (size_t)(end - path);
        char candidate[4096] = {0};
        int written = dir_len == 0 ?
                          snprintf(candidate, sizeof(candidate), "./%s", name) :
                          snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)dir_len, path, name);
        if (written > 0 && (size_t)written < sizeof(candidate) && access(candidate, X_OK) == 0) {
            return true;
        }
        if (end == nullptr) {
            break;
        }
        path = end + 1;
    }
    return access("/usr/bin/lightpanda", X_OK) == 0 ||
           access("/usr/local/bin/lightpanda", X_OK) == 0 ||
           access("/bin/lightpanda", X_OK) == 0;
}

static sc_status http_probe(sc_str url, sc_allocator *alloc, bool *out)
{
    sc_http_response response = {0};
    sc_http_request request = {
        .struct_size = sizeof(request),
        .method = sc_str_from_cstr("GET"),
        .url = url,
        .max_response_bytes = 4096,
        .timeout_ms = 1000,
        .connect_timeout_ms = 1000,
        .follow_location = false,
    };
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.browser_tool.invalid_argument");
    }
    status = sc_http_client_perform_sync(alloc, &request, &response);
    *out = sc_status_is_ok(status) && response.http_status >= 200 && response.http_status < 300;
    sc_http_response_clear(&response);
    return status;
}
