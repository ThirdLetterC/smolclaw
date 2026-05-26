#pragma once

#include <stdint.h>

#include "sc/allocator.h"
#include "sc/config.h"
#include "sc/log.h"
#include "sc/result.h"
#include "sc/sandbox.h"
#include "sc/string.h"
#include "sc/time.h"
#include "sc/url.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

typedef enum sc_autonomy_level {
    SC_AUTONOMY_READ_ONLY = 0,
    SC_AUTONOMY_SUPERVISED,
    SC_AUTONOMY_FULL,
    SC_AUTONOMY_OFF = SC_AUTONOMY_READ_ONLY,
    SC_AUTONOMY_AUTONOMOUS = SC_AUTONOMY_FULL
} sc_autonomy_level;

typedef enum sc_tool_risk {
    SC_TOOL_RISK_READONLY = 0,
    SC_TOOL_RISK_SIDE_EFFECT,
    SC_TOOL_RISK_NETWORK,
    SC_TOOL_RISK_SHELL,
    SC_TOOL_RISK_DESTRUCTIVE
} sc_tool_risk;

typedef enum sc_private_network_policy {
    SC_PRIVATE_NETWORK_BLOCK = 0,
    SC_PRIVATE_NETWORK_ALLOW
} sc_private_network_policy;

typedef enum sc_approval_decision {
    SC_APPROVAL_PENDING = 0,
    SC_APPROVAL_APPROVED,
    SC_APPROVAL_DENIED,
    SC_APPROVAL_EXPIRED
} sc_approval_decision;

typedef struct sc_security_policy {
    sc_allocator *alloc;
    sc_autonomy_level autonomy;
    sc_string workspace_root;
    sc_vec allowed_tools;
    sc_vec auto_approved_tools;
    sc_vec always_ask_tools;
    sc_vec denied_tools;
    sc_vec allowed_paths;
    sc_vec denied_paths;
    sc_vec allowed_domains;
    sc_vec denied_domains;
    sc_vec allowed_commands;
    sc_vec forbidden_commands;
    sc_vec shell_env_passthrough;
    sc_vec sandbox_allowed_domains;
    sc_vec sandbox_allow_devices;
    sc_string sandbox_container_runtime;
    sc_string sandbox_docker_path;
    sc_string sandbox_podman_path;
    sc_string sandbox_image_name;
    sc_private_network_policy private_network_policy;
    sc_sandbox_backend sandbox_backend;
    sc_sandbox_network_policy sandbox_network;
    bool shell_enabled;
    bool workspace_only;
    bool url_credentials_allowed;
    bool receipts_enabled;
    bool receipts_show_in_response;
    bool receipts_inject_system_prompt;
    int64_t approval_timeout_ms;
    int64_t sandbox_memory_limit_mb;
    int64_t sandbox_max_subprocesses;
    sc_vec otp_actions;
    sc_string otp_code;
    bool otp_enabled;
    sc_vec sandbox_fallback_order;
    bool sandbox_allow_noop_fallback;
} sc_security_policy;

typedef struct sc_security_tool_request {
    size_t struct_size;
    sc_str tool_name;
    sc_tool_risk risk;
    sc_str path_arg;
    bool path_must_exist;
    sc_str url_arg;
    sc_str shell_arg;
    sc_str device_arg;
    sc_str otp_code;
} sc_security_tool_request;

typedef struct sc_approval_request {
    size_t struct_size;
    sc_string id;
    sc_string tool_name;
    sc_string summary;
    sc_tool_risk risk;
    sc_wall_time created_at;
    int64_t timeout_ms;
} sc_approval_request;

typedef struct sc_approval_response {
    size_t struct_size;
    sc_string request_id;
    sc_approval_decision decision;
    sc_string reason;
    sc_wall_time decided_at;
} sc_approval_response;

typedef struct sc_audit_record {
    sc_string event_type;
    sc_string summary;
    sc_wall_time timestamp;
    uint64_t previous_hash;
    uint64_t hash;
} sc_audit_record;

typedef struct sc_audit_chain {
    sc_allocator *alloc;
    sc_vec records;
} sc_audit_chain;

typedef struct sc_tool_receipt {
    sc_string tool_name;
    sc_string args_summary;
    sc_string output_summary;
    sc_string token;
    sc_wall_time started_at;
    sc_wall_time ended_at;
    bool success;
    uint64_t previous_hash;
    uint64_t hash;
    sc_string policy_decision;
    sc_string failure_reason;
    sc_string outcome;
} sc_tool_receipt;

typedef struct sc_receipt_chain {
    sc_allocator *alloc;
    sc_vec receipts;
    unsigned char session_key[32];
    bool key_initialized;
} sc_receipt_chain;

typedef struct sc_estop_state {
    sc_allocator *alloc;
    bool active;
    sc_string reason;
} sc_estop_state;

void sc_security_policy_init(sc_security_policy *policy, sc_allocator *alloc);
sc_status sc_security_policy_init_defaults(sc_security_policy *policy, sc_allocator *alloc);
sc_status sc_security_policy_from_config(sc_security_policy *policy, const sc_config *config);
sc_status sc_security_policy_set_workspace(sc_security_policy *policy, sc_str workspace_root);
sc_status sc_security_policy_add_allowed_tool(sc_security_policy *policy, sc_str tool_name);
sc_status sc_security_policy_add_auto_approved_tool(sc_security_policy *policy, sc_str tool_name);
sc_status sc_security_policy_add_always_ask_tool(sc_security_policy *policy, sc_str tool_name);
sc_status sc_security_policy_add_denied_tool(sc_security_policy *policy, sc_str tool_name);
sc_status sc_security_policy_add_allowed_path(sc_security_policy *policy, sc_str path);
sc_status sc_security_policy_add_denied_path(sc_security_policy *policy, sc_str path);
sc_status sc_security_policy_add_allowed_domain(sc_security_policy *policy, sc_str domain);
sc_status sc_security_policy_add_denied_domain(sc_security_policy *policy, sc_str domain);
sc_status sc_security_policy_add_allowed_command(sc_security_policy *policy, sc_str command);
sc_status sc_security_policy_add_forbidden_command(sc_security_policy *policy, sc_str command);
sc_status sc_security_policy_add_shell_env_passthrough(sc_security_policy *policy, sc_str env_name);
sc_status sc_security_policy_add_otp_action(sc_security_policy *policy, sc_str tool_name);
sc_status sc_security_policy_add_sandbox_allowed_domain(sc_security_policy *policy, sc_str domain);
sc_status sc_security_policy_add_sandbox_allow_device(sc_security_policy *policy, sc_str path);
sc_status sc_security_validate_tool(const sc_security_policy *policy, sc_str tool_name);
sc_status sc_security_validate_request(const sc_security_policy *policy,
                                       const sc_estop_state *estop,
                                       const sc_security_tool_request *request,
                                       bool *approval_required);
bool sc_security_requires_approval(const sc_security_policy *policy, sc_tool_risk risk);
void sc_security_policy_clear(sc_security_policy *policy);

sc_status sc_workspace_resolve(const sc_security_policy *policy,
                               sc_str input_path,
                               bool must_exist,
                               sc_allocator *alloc,
                               sc_string *out);
sc_status sc_security_validate_path(const sc_security_policy *policy, sc_str input_path, bool must_exist);

sc_status sc_security_validate_url(const sc_security_policy *policy, sc_str url_text);
sc_status sc_security_validate_redirect(const sc_security_policy *policy,
                                        sc_str original_url,
                                        sc_str redirect_url);
sc_status sc_security_validate_sandbox_network(const sc_security_policy *policy, sc_str url_text);
sc_status sc_security_validate_prompt_injection(sc_str text);
sc_status sc_security_validate_outbound_message(sc_str text);
sc_status sc_security_validate_pairing(bool required, bool paired, sc_str subject);

sc_status sc_approval_request_new(sc_allocator *alloc,
                                  sc_str tool_name,
                                  sc_str summary,
                                  sc_tool_risk risk,
                                  int64_t timeout_ms,
                                  sc_approval_request *out);
sc_status sc_approval_response_new(sc_allocator *alloc,
                                   sc_str request_id,
                                   sc_approval_decision decision,
                                   sc_str reason,
                                   sc_approval_response *out);
bool sc_approval_response_allows(const sc_approval_request *request, const sc_approval_response *response);
void sc_approval_request_clear(sc_approval_request *request);
void sc_approval_response_clear(sc_approval_response *response);

void sc_audit_chain_init(sc_audit_chain *chain, sc_allocator *alloc);
sc_status sc_audit_chain_append(sc_audit_chain *chain, sc_str event_type, sc_str summary);
bool sc_audit_chain_verify(const sc_audit_chain *chain);
sc_status sc_audit_chain_save_file(const sc_audit_chain *chain, sc_str path);
sc_status sc_audit_chain_load_file(sc_allocator *alloc, sc_str path, sc_audit_chain *out);
void sc_audit_chain_clear(sc_audit_chain *chain);

void sc_receipt_chain_init(sc_receipt_chain *chain, sc_allocator *alloc);
sc_status sc_receipt_chain_append(sc_receipt_chain *chain,
                                  sc_str tool_name,
                                  sc_str args_summary,
                                  sc_str output_summary,
                                  bool success);
sc_status sc_receipt_chain_append_ex(sc_receipt_chain *chain,
                                     sc_str tool_name,
                                     sc_str args_summary,
                                     sc_str output_summary,
                                     bool success,
                                     sc_str policy_decision,
                                     sc_str failure_reason,
                                     sc_str outcome);
bool sc_receipt_chain_verify(const sc_receipt_chain *chain);
bool sc_receipt_chain_verify_token(const sc_receipt_chain *chain, size_t index, sc_str token);
sc_status sc_receipt_chain_save_file(const sc_receipt_chain *chain, sc_str path);
sc_status sc_receipt_chain_load_file(sc_allocator *alloc, sc_str path, sc_receipt_chain *out);
void sc_receipt_chain_clear(sc_receipt_chain *chain);

void sc_estop_init(sc_estop_state *estop, sc_allocator *alloc);
sc_status sc_estop_trip(sc_estop_state *estop, sc_str reason);
void sc_estop_reset(sc_estop_state *estop);
sc_status sc_estop_check(const sc_estop_state *estop);
sc_status sc_estop_write_file(const sc_estop_state *estop, sc_str path);
sc_status sc_estop_read_file(sc_allocator *alloc, sc_str path, sc_estop_state *out);
void sc_estop_clear(sc_estop_state *estop);

sc_status sc_sandbox_noop_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_landlock_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_bubblewrap_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_firejail_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_docker_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_podman_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_container_new(sc_allocator *alloc, sc_str runtime, sc_sandbox **out);
sc_status sc_sandbox_seatbelt_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_appcontainer_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_select_baseline(sc_allocator *alloc, sc_sandbox **out, sc_audit_chain *audit);
sc_status sc_sandbox_select_backend(sc_allocator *alloc,
                                    sc_sandbox_backend backend,
                                    sc_sandbox **out,
                                    sc_audit_chain *audit);
sc_sandbox_backend sc_sandbox_backend_from_str(sc_str value);
sc_str sc_sandbox_backend_to_str(sc_sandbox_backend backend);
sc_sandbox_network_policy sc_sandbox_network_policy_from_str(sc_str value);
sc_str sc_sandbox_network_policy_to_str(sc_sandbox_network_policy policy);

sc_str sc_security_redact_value_for_key(const char *key, sc_str value);

SC_END_DECLS
