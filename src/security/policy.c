#include "security/security_internal.h"

#include "sc/json.h"

#include <strings.h>
#include <string.h>

static sc_autonomy_level autonomy_from_str(sc_str value);
static bool tool_in_allowlist(const sc_security_policy *policy, sc_str tool_name);
static bool request_requires_approval(const sc_security_policy *policy, const sc_security_tool_request *request);
static sc_status load_auto_approved_tools(sc_security_policy *policy, const sc_config *config);
static sc_status load_default_policy_lists(sc_security_policy *policy);
static sc_status load_default_sandbox_fallback_order(sc_security_policy *policy);
static sc_status load_list_prop(const sc_config *config,
                                sc_str path,
                                sc_status (*add)(sc_security_policy *, sc_str),
                                sc_security_policy *policy);
static sc_status load_sandbox_fallback_order(sc_security_policy *policy, const sc_config *config);
static sc_status sandbox_fallback_order_add(sc_security_policy *policy, sc_sandbox_backend backend);
static sc_status load_optional_bool(const sc_config *config, sc_str path, bool *out);
static sc_status load_optional_int(const sc_config *config, sc_str path, int64_t *out);
static sc_status load_optional_string(const sc_config *config, sc_str path, sc_allocator *alloc, sc_string *out);
static sc_status validate_shell_policy(const sc_security_policy *policy, sc_str command);
static sc_status validate_otp_policy(const sc_security_policy *policy, const sc_security_tool_request *request);
static sc_status validate_device_policy(const sc_security_policy *policy, const sc_security_tool_request *request);
static bool command_contains_danger(sc_str command);
static sc_status command_basename(sc_allocator *alloc, sc_str command, sc_string *out);

void sc_security_policy_init(sc_security_policy *policy, sc_allocator *alloc)
{
    if (policy == nullptr) {
        return;
    }
    *policy = (sc_security_policy){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_vec_init(&policy->allowed_tools, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->auto_approved_tools, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->always_ask_tools, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->denied_tools, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->allowed_paths, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->denied_paths, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->allowed_domains, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->denied_domains, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->allowed_commands, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->forbidden_commands, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->shell_env_passthrough, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->otp_actions, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->sandbox_allowed_domains, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->sandbox_allow_devices, policy->alloc, sizeof(sc_string));
    sc_vec_init(&policy->sandbox_fallback_order, policy->alloc, sizeof(sc_sandbox_backend));
}

sc_status sc_security_policy_init_defaults(sc_security_policy *policy, sc_allocator *alloc)
{
    if (policy == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    sc_security_policy_init(policy, alloc);
    policy->autonomy = SC_AUTONOMY_SUPERVISED;
    policy->private_network_policy = SC_PRIVATE_NETWORK_BLOCK;
    policy->sandbox_backend = SC_SANDBOX_BACKEND_AUTO;
    policy->sandbox_network = SC_SANDBOX_NETWORK_FULL;
    policy->shell_enabled = false;
    policy->workspace_only = true;
    policy->url_credentials_allowed = false;
    policy->receipts_enabled = true;
    policy->receipts_show_in_response = false;
    policy->receipts_inject_system_prompt = true;
    policy->approval_timeout_ms = 30000;
    sc_status status = sc_string_from_cstr(policy->alloc, "alpine:3.23.4", &policy->sandbox_image_name);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = load_default_policy_lists(policy);
    if (sc_status_is_ok(status)) {
        status = load_default_sandbox_fallback_order(policy);
    }
    return status;
}

sc_status sc_security_policy_from_config(sc_security_policy *policy, const sc_config *config)
{
    sc_status status = sc_security_policy_init_defaults(policy, config == nullptr ? nullptr : config->alloc);
    sc_string value = {0};

    if (!sc_status_is_ok(status) || config == nullptr) {
        return status;
    }
    status = load_optional_string(config, sc_str_from_cstr("autonomy.level"), policy->alloc, &value);
    if (sc_status_is_ok(status) && value.len > 0) {
        policy->autonomy = autonomy_from_str(sc_string_as_str(&value));
    } else if (sc_status_is_ok(status)) {
        policy->autonomy = autonomy_from_str(sc_string_as_str(&config->runtime_autonomy_level));
    }
    sc_string_clear(&value);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (config->runtime_workspace_path.len > 0) {
        status = sc_security_policy_set_workspace(policy, sc_string_as_str(&config->runtime_workspace_path));
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config, sc_str_from_cstr("autonomy.workspace_only"), &policy->workspace_only);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config, sc_str_from_cstr("autonomy.shell_enabled"), &policy->shell_enabled);
    }
    if (sc_status_is_ok(status)) {
        status = load_auto_approved_tools(policy, config);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.always_ask"), sc_security_policy_add_always_ask_tool, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.always_ask.tools"), sc_security_policy_add_always_ask_tool, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.never_allow"), sc_security_policy_add_denied_tool, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.never_allow.tools"), sc_security_policy_add_denied_tool, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.forbidden_paths"), sc_security_policy_add_denied_path, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.allowed_commands"), sc_security_policy_add_allowed_command, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.forbidden_commands"), sc_security_policy_add_forbidden_command, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.shell_env_passthrough"), sc_security_policy_add_shell_env_passthrough, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config, sc_str_from_cstr("security.otp.enabled"), &policy->otp_enabled);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config, sc_str_from_cstr("security.otp.code"), policy->alloc, &policy->otp_code);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("security.otp.actions"), sc_security_policy_add_otp_action, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("security.sandbox.allowed_domains"), sc_security_policy_add_sandbox_allowed_domain, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("security.sandbox.allow_devices"), sc_security_policy_add_sandbox_allow_device, policy);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config, sc_str_from_cstr("security.sandbox.backend"), policy->alloc, &value);
        if (sc_status_is_ok(status) && value.len > 0) {
            policy->sandbox_backend = sc_sandbox_backend_from_str(sc_string_as_str(&value));
        }
        sc_string_clear(&value);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config,
                                      sc_str_from_cstr("security.sandbox.container_runtime"),
                                      policy->alloc,
                                      &policy->sandbox_container_runtime);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config,
                                      sc_str_from_cstr("security.sandbox.docker_path"),
                                      policy->alloc,
                                      &policy->sandbox_docker_path);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config,
                                      sc_str_from_cstr("security.sandbox.podman_path"),
                                      policy->alloc,
                                      &policy->sandbox_podman_path);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config,
                                      sc_str_from_cstr("security.sandbox.image_name"),
                                      policy->alloc,
                                      &policy->sandbox_image_name);
    }
    if (sc_status_is_ok(status)) {
        status = load_sandbox_fallback_order(policy, config);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config,
                                    sc_str_from_cstr("security.sandbox.allow_noop_fallback"),
                                    &policy->sandbox_allow_noop_fallback);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_string(config, sc_str_from_cstr("security.sandbox.network"), policy->alloc, &value);
        if (sc_status_is_ok(status) && value.len > 0) {
            policy->sandbox_network = sc_sandbox_network_policy_from_str(sc_string_as_str(&value));
        }
        sc_string_clear(&value);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_int(config, sc_str_from_cstr("security.sandbox.memory_limit_mb"), &policy->sandbox_memory_limit_mb);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_int(config, sc_str_from_cstr("security.sandbox.max_subprocesses"), &policy->sandbox_max_subprocesses);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config, sc_str_from_cstr("agent.tool_receipts.enabled"), &policy->receipts_enabled);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config, sc_str_from_cstr("agent.tool_receipts.show_in_response"), &policy->receipts_show_in_response);
    }
    if (sc_status_is_ok(status)) {
        status = load_optional_bool(config, sc_str_from_cstr("agent.tool_receipts.inject_system_prompt"), &policy->receipts_inject_system_prompt);
    }
    return status;
}

sc_status sc_security_policy_set_workspace(sc_security_policy *policy, sc_str workspace_root)
{
    if (policy == nullptr || (workspace_root.len > 0 && workspace_root.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    sc_string_clear(&policy->workspace_root);
    if (workspace_root.len == 0) {
        return sc_status_ok();
    }
    return sc_string_from_str(policy->alloc, workspace_root, &policy->workspace_root);
}

sc_status sc_security_policy_add_allowed_tool(sc_security_policy *policy, sc_str tool_name)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->allowed_tools, policy->alloc, tool_name);
}

sc_status sc_security_policy_add_auto_approved_tool(sc_security_policy *policy, sc_str tool_name)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->auto_approved_tools, policy->alloc, tool_name);
}

sc_status sc_security_policy_add_always_ask_tool(sc_security_policy *policy, sc_str tool_name)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->always_ask_tools, policy->alloc, tool_name);
}

sc_status sc_security_policy_add_denied_tool(sc_security_policy *policy, sc_str tool_name)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->denied_tools, policy->alloc, tool_name);
}

sc_status sc_security_policy_add_allowed_path(sc_security_policy *policy, sc_str path)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->allowed_paths, policy->alloc, path);
}

sc_status sc_security_policy_add_denied_path(sc_security_policy *policy, sc_str path)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->denied_paths, policy->alloc, path);
}

sc_status sc_security_policy_add_allowed_domain(sc_security_policy *policy, sc_str domain)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->allowed_domains, policy->alloc, domain);
}

sc_status sc_security_policy_add_denied_domain(sc_security_policy *policy, sc_str domain)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->denied_domains, policy->alloc, domain);
}

sc_status sc_security_policy_add_allowed_command(sc_security_policy *policy, sc_str command)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->allowed_commands, policy->alloc, command);
}

sc_status sc_security_policy_add_forbidden_command(sc_security_policy *policy, sc_str command)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->forbidden_commands, policy->alloc, command);
}

sc_status sc_security_policy_add_shell_env_passthrough(sc_security_policy *policy, sc_str env_name)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->shell_env_passthrough, policy->alloc, env_name);
}

sc_status sc_security_policy_add_otp_action(sc_security_policy *policy, sc_str tool_name)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->otp_actions, policy->alloc, tool_name);
}

sc_status sc_security_policy_add_sandbox_allowed_domain(sc_security_policy *policy, sc_str domain)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->sandbox_allowed_domains, policy->alloc, domain);
}

sc_status sc_security_policy_add_sandbox_allow_device(sc_security_policy *policy, sc_str path)
{
    return policy == nullptr ? sc_status_invalid_argument("sc.security.invalid_argument")
                          : sc_security_list_add(&policy->sandbox_allow_devices, policy->alloc, path);
}

sc_status sc_security_validate_tool(const sc_security_policy *policy, sc_str tool_name)
{
    if (policy == nullptr || tool_name.len == 0 || tool_name.ptr == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    if (sc_security_list_contains(&policy->denied_tools, tool_name)) {
        return sc_status_security_denied("sc.security.tool_denied");
    }
    if (!tool_in_allowlist(policy, tool_name)) {
        return sc_status_security_denied("sc.security.tool_not_allowed");
    }
    return sc_status_ok();
}

sc_status sc_security_validate_request(const sc_security_policy *policy,
                                       const sc_estop_state *estop,
                                       const sc_security_tool_request *request,
                                       bool *approval_required)
{
    sc_status status;

    if (policy == nullptr || request == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    if (approval_required != nullptr) {
        *approval_required = false;
    }
    status = sc_estop_check(estop);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_security_validate_tool(policy, request->tool_name);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (request->risk == SC_TOOL_RISK_SHELL && !policy->shell_enabled) {
        return sc_status_security_denied("sc.security.shell_denied");
    }
    if (policy->autonomy == SC_AUTONOMY_READ_ONLY && request->risk != SC_TOOL_RISK_READONLY) {
        return sc_status_security_denied("sc.security.read_only_denied");
    }
    if (policy->autonomy == SC_AUTONOMY_SUPERVISED &&
        (request->risk == SC_TOOL_RISK_DESTRUCTIVE || request->risk == SC_TOOL_RISK_SHELL) &&
        !sc_security_list_contains(&policy->auto_approved_tools, request->tool_name)) {
        return sc_status_security_denied("sc.security.high_risk_denied");
    }
    /*
     * Validate each boundary independently so security denials keep a precise
     * error key instead of collapsing into a generic tool failure.
     */
    if (request->shell_arg.len > 0) {
        status = validate_shell_policy(policy, request->shell_arg);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    if (request->path_arg.len > 0) {
        status = sc_security_validate_path(policy, request->path_arg, request->path_must_exist);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    if (request->url_arg.len > 0) {
        status = sc_security_validate_url(policy, request->url_arg);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        status = sc_security_validate_sandbox_network(policy, request->url_arg);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    status = validate_device_policy(policy, request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = validate_otp_policy(policy, request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (approval_required != nullptr) {
        *approval_required = request_requires_approval(policy, request);
    }
    return sc_status_ok();
}

bool sc_security_requires_approval(const sc_security_policy *policy, sc_tool_risk risk)
{
    if (policy == nullptr) {
        return true;
    }
    if (policy->autonomy == SC_AUTONOMY_READ_ONLY) {
        return risk != SC_TOOL_RISK_READONLY;
    }
    if (policy->autonomy == SC_AUTONOMY_SUPERVISED) {
        return risk != SC_TOOL_RISK_READONLY && risk != SC_TOOL_RISK_DESTRUCTIVE && risk != SC_TOOL_RISK_SHELL;
    }
    return false;
}

void sc_security_policy_clear(sc_security_policy *policy)
{
    if (policy == nullptr) {
        return;
    }
    sc_string_clear(&policy->workspace_root);
    sc_security_string_list_clear(&policy->allowed_tools);
    sc_security_string_list_clear(&policy->auto_approved_tools);
    sc_security_string_list_clear(&policy->always_ask_tools);
    sc_security_string_list_clear(&policy->denied_tools);
    sc_security_string_list_clear(&policy->allowed_paths);
    sc_security_string_list_clear(&policy->denied_paths);
    sc_security_string_list_clear(&policy->allowed_domains);
    sc_security_string_list_clear(&policy->denied_domains);
    sc_security_string_list_clear(&policy->allowed_commands);
    sc_security_string_list_clear(&policy->forbidden_commands);
    sc_security_string_list_clear(&policy->shell_env_passthrough);
    sc_security_string_list_clear(&policy->otp_actions);
    sc_security_string_list_clear(&policy->sandbox_allowed_domains);
    sc_security_string_list_clear(&policy->sandbox_allow_devices);
    sc_string_clear(&policy->sandbox_container_runtime);
    sc_string_clear(&policy->sandbox_docker_path);
    sc_string_clear(&policy->sandbox_podman_path);
    sc_string_clear(&policy->sandbox_image_name);
    sc_vec_clear(&policy->sandbox_fallback_order);
    sc_string_secure_clear(&policy->otp_code);
    *policy = (sc_security_policy){0};
}

uint64_t sc_security_hash_init(void)
{
    return UINT64_C(1469598103934665603);
}

uint64_t sc_security_hash_bytes(uint64_t hash, sc_str input)
{
    for (size_t i = 0; i < input.len; ++i) {
        hash ^= (unsigned char)input.ptr[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t sc_security_hash_u64(uint64_t hash, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i) {
        unsigned char byte = (unsigned char)((value >> (i * 8u)) & 0xFFu);
        hash ^= byte;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

uint64_t sc_security_hash_bool(uint64_t hash, bool value)
{
    return sc_security_hash_u64(hash, value ? 1u : 0u);
}

sc_status sc_security_list_add(sc_vec *list, sc_allocator *alloc, sc_str value)
{
    sc_string copy = {0};
    sc_status status;

    if (list == nullptr || value.len == 0 || value.ptr == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    status = sc_string_from_str(alloc, value, &copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_vec_push(list, &copy);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&copy);
    }
    return status;
}

bool sc_security_list_contains(const sc_vec *list, sc_str value)
{
    if (list == nullptr || value.len == 0 || value.ptr == nullptr) {
        return false;
    }
    for (size_t i = 0; i < list->len; ++i) {
        const sc_string *item = sc_vec_at_const(list, i);
        if (item != nullptr && sc_str_equal(sc_string_as_str(item), value)) {
            return true;
        }
    }
    return false;
}

void sc_security_string_list_clear(sc_vec *list)
{
    if (list == nullptr) {
        return;
    }
    for (size_t i = 0; i < list->len; ++i) {
        sc_string *item = sc_vec_at(list, i);
        sc_string_clear(item);
    }
    sc_vec_clear(list);
}

bool sc_security_path_has_prefix(sc_str path, sc_str prefix)
{
    if (path.len < prefix.len || path.ptr == nullptr || prefix.ptr == nullptr) {
        return false;
    }
#ifdef __APPLE__
    /* macOS default filesystems are commonly case-insensitive; mirror that for boundary checks. */
    if (strncasecmp(path.ptr, prefix.ptr, prefix.len) != 0) {
        return false;
    }
#else
    if (memcmp(path.ptr, prefix.ptr, prefix.len) != 0) {
        return false;
    }
#endif
    return path.len == prefix.len || path.ptr[prefix.len] == '/';
}

static sc_autonomy_level autonomy_from_str(sc_str value)
{
    if (sc_str_equal(value, sc_str_from_cstr("read_only"))) {
        return SC_AUTONOMY_READ_ONLY;
    }
    if (sc_str_equal(value, sc_str_from_cstr("autonomous"))) {
        return SC_AUTONOMY_FULL;
    }
    if (sc_str_equal(value, sc_str_from_cstr("full"))) {
        return SC_AUTONOMY_FULL;
    }
    if (sc_str_equal(value, sc_str_from_cstr("off"))) {
        return SC_AUTONOMY_READ_ONLY;
    }
    return SC_AUTONOMY_SUPERVISED;
}

static bool tool_in_allowlist(const sc_security_policy *policy, sc_str tool_name)
{
    if (policy->allowed_tools.len == 0) {
        return true;
    }
    return sc_security_list_contains(&policy->allowed_tools, tool_name);
}

static bool request_requires_approval(const sc_security_policy *policy, const sc_security_tool_request *request)
{
    if (policy == nullptr || request == nullptr) {
        return true;
    }
    if (sc_security_list_contains(&policy->always_ask_tools, request->tool_name)) {
        return true;
    }
    if (sc_security_list_contains(&policy->auto_approved_tools, request->tool_name)) {
        return false;
    }
    return sc_security_requires_approval(policy, request->risk);
}

static sc_status load_default_policy_lists(sc_security_policy *policy)
{
    static const char *denied_paths[] = {"/etc", "/sys", "/boot", "~/.ssh", "~/.aws"};
    static const char *forbidden_commands[] = {"shutdown", "reboot", "mkfs"};
    static const char *shell_env[] = {"PATH", "HOME", "USER", "LANG"};
    static const char *otp_actions[] = {"shell", "browser", "browser_screenshot", "file_write"};
    sc_status status = sc_status_ok();

    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(denied_paths); i += 1) {
        status = sc_security_policy_add_denied_path(policy, sc_str_from_cstr(denied_paths[i]));
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(forbidden_commands); i += 1) {
        status = sc_security_policy_add_forbidden_command(policy, sc_str_from_cstr(forbidden_commands[i]));
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(shell_env); i += 1) {
        status = sc_security_policy_add_shell_env_passthrough(policy, sc_str_from_cstr(shell_env[i]));
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(otp_actions); i += 1) {
        status = sc_security_policy_add_otp_action(policy, sc_str_from_cstr(otp_actions[i]));
    }
    return status;
}

static sc_status load_default_sandbox_fallback_order(sc_security_policy *policy)
{
    static const sc_sandbox_backend defaults[] = {
        SC_SANDBOX_BACKEND_LANDLOCK,
        SC_SANDBOX_BACKEND_BUBBLEWRAP,
        SC_SANDBOX_BACKEND_FIREJAIL,
        SC_SANDBOX_BACKEND_DOCKER,
        SC_SANDBOX_BACKEND_PODMAN,
    };
    sc_status status = sc_status_ok();

    if (policy == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    policy->sandbox_fallback_order.len = 0;
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(defaults); i += 1) {
        status = sandbox_fallback_order_add(policy, defaults[i]);
    }
    return status;
}

static sc_status load_auto_approved_tools(sc_security_policy *policy, const sc_config *config)
{
    sc_status status = load_list_prop(config, sc_str_from_cstr("autonomy.auto_approve"), sc_security_policy_add_auto_approved_tool, policy);
    if (sc_status_is_ok(status)) {
        status = load_list_prop(config, sc_str_from_cstr("autonomy.auto_approve.tools"), sc_security_policy_add_auto_approved_tool, policy);
    }
    return status;
}

static sc_status load_sandbox_fallback_order(sc_security_policy *policy, const sc_config *config)
{
    sc_string raw = {0};
    sc_json_value *array = nullptr;
    sc_json_parse_error error = {0};
    sc_status status;

    if (policy == nullptr || config == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    status = sc_config_get_prop(config, sc_str_from_cstr("security.sandbox.fallback_order"), policy->alloc, &raw);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    status = sc_json_parse(policy->alloc, sc_string_as_str(&raw), &array, &error);
    if (sc_status_is_ok(status) && sc_json_type_of(array) == SC_JSON_ARRAY) {
        policy->sandbox_fallback_order.len = 0;
        for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(array); i += 1) {
            sc_str value = {0};
            sc_sandbox_backend backend;
            sc_json_value *item = sc_json_array_get(array, i);
            if (!sc_json_as_str(item, &value) || value.len == 0) {
                status = sc_status_parse("sc.security.sandbox_fallback_order_invalid");
            } else {
                backend = sc_sandbox_backend_from_str(value);
                if (backend == SC_SANDBOX_BACKEND_AUTO || backend == SC_SANDBOX_BACKEND_NOOP) {
                    status = sc_status_parse("sc.security.sandbox_fallback_order_invalid");
                } else {
                    status = sandbox_fallback_order_add(policy, backend);
                }
            }
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_status_parse("sc.security.sandbox_fallback_order_invalid");
    }
    if (sc_status_is_ok(status) && policy->sandbox_fallback_order.len == 0) {
        status = sc_status_parse("sc.security.sandbox_fallback_order_invalid");
    }
    sc_json_destroy(array);
    sc_string_clear(&raw);
    return status;
}

static sc_status sandbox_fallback_order_add(sc_security_policy *policy, sc_sandbox_backend backend)
{
    if (policy == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    return sc_vec_push(&policy->sandbox_fallback_order, &backend);
}

static sc_status load_optional_bool(const sc_config *config, sc_str path, bool *out)
{
    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    if (!sc_config_has_prop(config, path)) {
        return sc_status_ok();
    }
    *out = sc_config_get_bool(config, path, *out);
    return sc_status_ok();
}

static sc_status load_optional_int(const sc_config *config, sc_str path, int64_t *out)
{
    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    if (!sc_config_has_prop(config, path)) {
        return sc_status_ok();
    }
    *out = sc_config_get_int(config, path, *out);
    return sc_status_ok();
}

static sc_status load_optional_string(const sc_config *config, sc_str path, sc_allocator *alloc, sc_string *out)
{
    sc_status status;

    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    sc_string_clear(out);
    if (!sc_config_has_prop(config, path)) {
        return sc_status_ok();
    }
    status = sc_config_get_prop(config, path, alloc, out);
    if (!sc_status_is_ok(status)) {
        /* Optional string overrides are best-effort; callers enter with default values already installed. */
        sc_status_clear(&status);
        return sc_status_ok();
    }
    if (out->len == 0) {
        sc_string_clear(out);
    }
    return status;
}

static sc_status validate_shell_policy(const sc_security_policy *policy, sc_str command)
{
    sc_string basename = {0};
    sc_status status;

    if (policy == nullptr || command.ptr == nullptr || command.len == 0) {
        return sc_status_invalid_argument("sc.security.shell_command_invalid");
    }
    if (command_contains_danger(command)) {
        return sc_status_security_denied("sc.security.shell_command_denied");
    }
    status = command_basename(policy->alloc, command, &basename);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_security_list_contains(&policy->forbidden_commands, sc_string_as_str(&basename))) {
        status = sc_status_security_denied("sc.security.shell_command_forbidden");
    }
    if (sc_status_is_ok(status) && policy->allowed_commands.len > 0 &&
        !sc_security_list_contains(&policy->allowed_commands, sc_string_as_str(&basename))) {
        status = sc_status_security_denied("sc.security.shell_command_not_allowed");
    }
    sc_string_clear(&basename);
    return status;
}

static sc_status validate_otp_policy(const sc_security_policy *policy, const sc_security_tool_request *request)
{
    if (policy == nullptr || request == nullptr || !policy->otp_enabled) {
        return sc_status_ok();
    }
    if (!sc_security_list_contains(&policy->otp_actions, request->tool_name)) {
        return sc_status_ok();
    }
    if (policy->otp_code.len == 0) {
        return sc_status_security_denied("sc.security.otp_unconfigured");
    }
    if (request->otp_code.len == 0) {
        return sc_status_security_denied("sc.security.otp_required");
    }
    if (!sc_str_equal(request->otp_code, sc_string_as_str(&policy->otp_code))) {
        return sc_status_security_denied("sc.security.otp_invalid");
    }
    return sc_status_ok();
}

static sc_status validate_device_policy(const sc_security_policy *policy, const sc_security_tool_request *request)
{
    if (policy == nullptr || request == nullptr || policy->sandbox_allow_devices.len == 0 ||
        request->device_arg.len == 0) {
        return sc_status_ok();
    }
    if (sc_security_list_contains(&policy->sandbox_allow_devices, sc_str_from_cstr("*")) ||
        sc_security_list_contains(&policy->sandbox_allow_devices, request->device_arg)) {
        return sc_status_ok();
    }
    return sc_status_security_denied("sc.security.device_not_allowed");
}

static bool command_contains_danger(sc_str command)
{
    static const char *needles[] = {"rm -rf /", "mkfs", "shutdown", "reboot", ":(){", "dd if=", ">/dev/"};
    /* This is a hard denylist backstop; allowlists and sandboxing still make the primary decision. */
    for (size_t i = 0; i < SC_ARRAY_LEN(needles); i += 1) {
        size_t needle_len = strlen(needles[i]);
        for (size_t j = 0; j + needle_len <= command.len; j += 1) {
            if (memcmp(&command.ptr[j], needles[i], needle_len) == 0) {
                return true;
            }
        }
    }
    return false;
}

static sc_status command_basename(sc_allocator *alloc, sc_str command, sc_string *out)
{
    size_t start = 0;
    size_t end = 0;
    size_t slash = 0;

    if (command.ptr == nullptr || command.len == 0 || out == nullptr) {
        return sc_status_invalid_argument("sc.security.shell_command_invalid");
    }
    while (start < command.len && (command.ptr[start] == ' ' || command.ptr[start] == '\t')) {
        start += 1;
    }
    end = start;
    while (end < command.len && command.ptr[end] != ' ' && command.ptr[end] != '\t' &&
           command.ptr[end] != '\n' && command.ptr[end] != '\r') {
        end += 1;
    }
    if (start == end) {
        return sc_status_invalid_argument("sc.security.shell_command_invalid");
    }
    slash = start;
    for (size_t i = start; i < end; i += 1) {
        if (command.ptr[i] == '/') {
            slash = i + 1;
        }
    }
    if (slash >= end) {
        return sc_status_invalid_argument("sc.security.shell_command_invalid");
    }
    return sc_string_from_str(alloc, sc_str_from_parts(&command.ptr[slash], end - slash), out);
}

static sc_status load_list_prop(const sc_config *config,
                                sc_str path,
                                sc_status (*add)(sc_security_policy *, sc_str),
                                sc_security_policy *policy)
{
    sc_string raw = {0};
    sc_json_value *array = nullptr;
    sc_json_parse_error error = {0};
    sc_status status;

    if (policy == nullptr || config == nullptr || add == nullptr) {
        return sc_status_invalid_argument("sc.security.invalid_argument");
    }
    status = sc_config_get_prop(config, path, policy->alloc, &raw);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    status = sc_json_parse(policy->alloc, sc_string_as_str(&raw), &array, &error);
    if (sc_status_is_ok(status) && sc_json_type_of(array) == SC_JSON_ARRAY) {
        for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(array); ++i) {
            sc_str tool_name = {0};
            sc_json_value *item = sc_json_array_get(array, i);
            if (sc_json_as_str(item, &tool_name) && tool_name.len > 0) {
                status = add(policy, tool_name);
            }
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_status_parse("sc.security.auto_approve_invalid");
    }
    sc_json_destroy(array);
    sc_string_clear(&raw);
    return status;
}
