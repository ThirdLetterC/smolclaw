// cppcheck-suppress-file redundantInitialization
#include "sc/bootstrap.h"

#include "sc/autonomy.h"
#include "sc/channel.h"
#include "sc/config.h"
#include "sc/gateway.h"
#include "sc/log.h"
#include "sc/memory.h"
#include "sc/provider.h"
#include "sc/runtime.h"
#include "sc/security.h"
#include "sc/tool.h"
#include "sc/version.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    BOOT_MAX_RELIABLE_PROVIDERS = 8,
    BOOT_MAX_ROUTER_ROUTES = 8,
    BOOT_MAX_PROVIDER_HANDLES = BOOT_MAX_RELIABLE_PROVIDERS + 2,
    BOOT_MAX_CHANNELS = 8,
    BOOT_MAX_TOOLS = 40
};

typedef struct boot_owned {
    sc_provider *provider;
    sc_provider *provider_handles[BOOT_MAX_PROVIDER_HANDLES];
    size_t provider_handle_count;
    sc_memory *memory;
    sc_transcriber *transcriber;
    sc_tts *tts;
    sc_agent *agent;
    sc_channel *channels[BOOT_MAX_CHANNELS];
    size_t channel_count;
    sc_channel_orchestrator *orchestrator;
    sc_gateway_server *gateway;
    sc_observer *gateway_observer;
    sc_delivery_target *delivery;
    sc_cron_job_store cron_jobs;
    sc_cron_run_store cron_runs;
    bool cron_initialized;
    sc_heartbeat_state heartbeat;
    bool heartbeat_initialized;
    sc_string heartbeat_state_path;
    sc_tool *tools[BOOT_MAX_TOOLS];
    size_t tool_count;
    sc_security_policy policy;
    sc_estop_state estop;
} boot_owned;

typedef struct boot_loop_context {
    boot_owned *owned;
    sc_allocator *alloc;
    bool continue_channel_errors;
} boot_loop_context;

struct sc_boot_session {
    sc_allocator *alloc;
    boot_owned owned;
    sc_runtime_turn_id next_turn_id;
};

typedef sc_status (*tool_constructor_fn)(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);

static sc_status read_file(sc_allocator *alloc, sc_str path, sc_string *out);
static sc_status copy_c_path(sc_allocator *alloc, sc_str path, const char *error_key, sc_string *out);
static sc_status path_dirname(sc_allocator *alloc, sc_str path, sc_string *out);
static sc_status join_path(sc_allocator *alloc, sc_str left, sc_str right, sc_string *out);
static sc_status ensure_dir(sc_allocator *alloc, sc_str path);
static sc_status get_prop_copy(const sc_config *config, sc_str path, sc_allocator *alloc, sc_string *out);
static sc_status build_workspace_path(sc_allocator *alloc,
                                      const sc_boot_options *options,
                                      sc_str config_path,
                                      sc_string *out);
static sc_status init_workspace_state(sc_allocator *alloc, sc_str workspace);
static sc_status load_estop_from_workspace(sc_allocator *alloc, sc_str workspace, sc_estop_state *estop);
static sc_status build_identity(sc_allocator *alloc, sc_str workspace, sc_string *out);
static sc_status append_workspace_file(sc_allocator *alloc,
                                       sc_string_builder *builder,
                                       sc_str workspace,
                                       const char *name);
static sc_status create_provider_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned);
static sc_status boot_add_provider_handle(boot_owned *owned, sc_provider *provider);
static sc_status create_named_provider(sc_allocator *alloc,
                                       const sc_config *config,
                                       sc_str provider,
                                       sc_provider **out);
static sc_status create_memory_from_config(sc_allocator *alloc, const sc_config *config, sc_str workspace, sc_memory **out);
static sc_status create_tools(sc_allocator *alloc,
                              const sc_config *config,
                              sc_memory *memory,
                              const sc_security_policy *policy,
                              const sc_estop_state *estop,
                              boot_owned *owned);
static sc_status append_tool_if_exposed(sc_allocator *alloc,
                                        const sc_config *config,
                                        const sc_tool_context *context,
                                        boot_owned *owned,
                                        sc_str name,
                                        sc_tool_risk risk,
                                        uint64_t capability_category,
                                        sc_tool_side_effect side_effect,
                                        tool_constructor_fn constructor);
static bool tool_filter_groups_exclude(const sc_config *config,
                                       sc_allocator *alloc,
                                       sc_str name,
                                       sc_tool_risk risk,
                                       uint64_t capability_category,
                                       sc_tool_side_effect side_effect);
static bool string_array_prop_contains(const sc_config *config, sc_allocator *alloc, sc_str path, sc_str value);
static bool config_prop_nonempty(const sc_config *config, sc_allocator *alloc, sc_str path);
static bool tool_name_is_public_restricted(sc_str name);
static sc_status create_channels_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned);
static sc_status create_transcriber_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned);
static sc_status create_tts_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned);
static sc_status create_telegram_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out);
static sc_status create_webhook_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out);
static sc_status create_rabbitmq_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out);
static sc_status create_mail_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out);
static sc_status create_orchestrator_from_config(sc_allocator *alloc,
                                                 const sc_config *config,
                                                 sc_str workspace,
                                                 boot_owned *owned,
                                                 sc_vec *allowed,
                                                 sc_str **allowed_senders,
                                                 sc_string *allowed_raw,
                                                 sc_string *stream_mode,
                                                 sc_string *session_db,
                                                 sc_string *session_dir);
static sc_status create_gateway_from_config(sc_allocator *alloc,
                                            const sc_config *config,
                                            const sc_boot_options *options,
                                            boot_owned *owned);
static sc_status create_delivery_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned);
static sc_status create_cron_from_config(sc_allocator *alloc, const sc_config *config, sc_str workspace, boot_owned *owned);
static sc_status create_heartbeat_from_config(sc_allocator *alloc, const sc_config *config, sc_str workspace, boot_owned *owned);
static sc_status parse_string_array(sc_allocator *alloc, sc_str input, sc_vec *out);
static void clear_string_vec(sc_vec *vec);
static sc_status build_channel_common_configs(sc_allocator *alloc,
                                              const sc_config *config,
                                              const boot_owned *owned,
                                              sc_channel_common_config *common_configs,
                                              sc_vec *allow_values,
                                              sc_vec *deny_values,
                                              sc_str **allow_views,
                                              sc_str **deny_views,
                                              size_t capacity,
                                              size_t *out_count);
static sc_status build_channel_common_config(sc_allocator *alloc,
                                             const sc_config *config,
                                             sc_str channel_name,
                                             sc_channel_common_config *out,
                                             sc_vec *allow_values,
                                             sc_vec *deny_values,
                                             sc_str **allow_views,
                                             sc_str **deny_views,
                                             bool *configured);
static sc_status parse_optional_string_array_prop(sc_allocator *alloc,
                                                  const sc_config *config,
                                                  sc_str path,
                                                  sc_vec *out);
static sc_status string_vec_to_views(sc_allocator *alloc, const sc_vec *values, sc_str **out);
static sc_status build_channel_config_path(sc_allocator *alloc, sc_str channel_name, const char *field, sc_string *out);
static bool parse_autonomy_level(sc_str value, sc_autonomy_level *out);
static const char *bootstrap_status_code_name(sc_status_code code);
static const char *bootstrap_bool_name(bool value);
static void clear_channel_common_storage(sc_allocator *alloc,
                                         sc_vec *allow_values,
                                         sc_vec *deny_values,
                                         sc_str **allow_views,
                                         sc_str **deny_views,
                                         size_t capacity);
static sc_status get_model_prop(sc_allocator *alloc,
                                const sc_config *config,
                                sc_str provider,
                                const char *name,
                                sc_string *out);
static sc_status build_model_prop_path(sc_allocator *alloc, sc_str provider, const char *name, sc_string *out);
static sc_status try_secret_store_prop(const sc_config *config,
                                       sc_allocator *alloc,
                                       sc_str path,
                                       sc_string *out,
                                       bool *found);
static sc_status try_model_secret_store_prop(const sc_config *config,
                                             sc_allocator *alloc,
                                             sc_str provider,
                                             const char *name,
                                             sc_string *out,
                                             bool *found);
static sc_status get_model_route_prop(sc_allocator *alloc,
                                      const sc_config *config,
                                      sc_str provider,
                                      size_t index,
                                      const char *name,
                                      sc_string *out);
static bool model_prop_present(sc_allocator *alloc, const sc_config *config, sc_str provider, const char *name);
static sc_str provider_default_credential_env(sc_str kind);
static bool provider_is_compatible_preset(sc_str kind);
static double get_model_double(sc_allocator *alloc,
                               const sc_config *config,
                               sc_str provider,
                               const char *name,
                               double fallback);
static int64_t get_model_int(sc_allocator *alloc,
                             const sc_config *config,
                             sc_str provider,
                             const char *name,
                             int64_t fallback);
static bool get_model_bool(sc_allocator *alloc,
                           const sc_config *config,
                           sc_str provider,
                           const char *name,
                           bool fallback);
static sc_status resolve_default_provider(sc_allocator *alloc, const sc_config *config, sc_string *out);
static sc_status resolve_default_model(sc_allocator *alloc, const sc_config *config, sc_string *out);
static sc_status run_runtime_loop(boot_owned *owned, sc_allocator *alloc, bool once, size_t max_polls, bool hard_shutdown);
static sc_status close_runtime_transports(void *user_data, sc_allocator *alloc);
static sc_status run_gateway_poll_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc);
static sc_status run_channel_poll_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc);
static sc_status run_cron_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc);
static sc_status run_heartbeat_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc);
static sc_status run_cron_due_jobs(boot_owned *owned, sc_allocator *alloc);
static sc_status run_heartbeat_tick(boot_owned *owned);
static bool provider_is(sc_str provider, const char *name);
static sc_status boot_prepare_agent(sc_allocator *alloc,
                                    const sc_boot_options *options,
                                    boot_owned *owned,
                                    sc_string *workspace,
                                    sc_string *identity,
                                    sc_string *model,
                                    sc_config *config,
                                    sc_config_diag *diag,
                                    sc_secret_store **secret_store,
                                    sc_string *config_body);
static void boot_owned_clear(boot_owned *owned);

sc_status sc_runtime_boot(sc_allocator *alloc, const sc_boot_options *options)
{
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options load = {0};
    sc_string config_body = {0};
    sc_string workspace = {0};
    sc_string identity = {0};
    sc_string model = {0};
    sc_string session_db = {0};
    sc_string session_dir = {0};
    sc_string allowed_raw = {0};
    sc_string stream_mode = {0};
    sc_vec allowed = {0};
    sc_str *allowed_senders = nullptr;
    sc_secret_store *secret_store = nullptr;
    boot_owned owned = {0};
    sc_status status = sc_status_ok();

    if (options == nullptr || options->config_path.ptr == nullptr || options->config_path.len == 0) {
        return sc_status_invalid_argument("sc.bootstrap.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&allowed, alloc, sizeof(sc_string));

    status = read_file(alloc, options->config_path, &config_body);
    if (sc_status_is_ok(status)) {
        load.explicit_file = (sc_config_source){
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = options->config_path,
            .body = sc_string_as_str(&config_body),
            .present = true,
        };
        status = sc_config_load(alloc, &load, &config, &diag);
    }
    if (sc_status_is_ok(status)) {
        status = sc_secret_store_file_new(alloc, sc_str_from_parts(nullptr, 0), &secret_store);
        if (sc_status_is_ok(status)) {
            status = sc_config_attach_secret_store(&config, secret_store, true);
        } else if (status.error_key != nullptr && strcmp(status.error_key, "sc.secret_store.file_unsupported") == 0) {
            sc_status_clear(&status);
            status = sc_status_ok();
        }
    }
    if (sc_status_is_ok(status)) {
        status = build_workspace_path(alloc, options, options->config_path, &workspace);
    }
    if (sc_status_is_ok(status)) {
        status = ensure_dir(alloc, sc_string_as_str(&workspace));
    }
    if (sc_status_is_ok(status)) {
        status = init_workspace_state(alloc, sc_string_as_str(&workspace));
    }
    if (sc_status_is_ok(status)) {
        status = sc_security_policy_from_config(&owned.policy, &config);
    }
    if (sc_status_is_ok(status)) {
        status = sc_security_policy_set_workspace(&owned.policy, sc_string_as_str(&workspace));
    }
    if (sc_status_is_ok(status)) {
        sc_estop_init(&owned.estop, alloc);
        status = load_estop_from_workspace(alloc, sc_string_as_str(&workspace), &owned.estop);
    }
    if (sc_status_is_ok(status)) {
        status = build_identity(alloc, sc_string_as_str(&workspace), &identity);
    }
    if (sc_status_is_ok(status)) {
        status = create_provider_from_config(alloc, &config, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_memory_from_config(alloc, &config, sc_string_as_str(&workspace), &owned.memory);
    }
    if (sc_status_is_ok(status)) {
        status = create_cron_from_config(alloc, &config, sc_string_as_str(&workspace), &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_tools(alloc, &config, owned.memory, &owned.policy, &owned.estop, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = resolve_default_model(alloc, &config, &model);
    }
    if (sc_status_is_ok(status)) {
        sc_agent_options agent_options = {
            .struct_size = sizeof(sc_agent_options),
            .provider = owned.provider,
            .memory = owned.memory,
            .tools = owned.tools,
            .tool_count = owned.tool_count,
            .policy = &owned.policy,
            .estop = &owned.estop,
            .model = sc_string_as_str(&model),
            .identity = sc_string_as_str(&identity),
            .workspace = sc_string_as_str(&workspace),
            .runtime_environment = sc_str_from_cstr("smolclaw-c/schema-v2"),
            .memory_namespace = sc_str_from_cstr("default"),
            .turn_namespace = sc_str_from_cstr("default"),
            .max_history_messages = (size_t)sc_config_get_int(&config, sc_str_from_cstr("agent.max_history_messages"), 50),
            .max_tool_iterations = (size_t)sc_config_get_int(&config, sc_str_from_cstr("agent.max_tool_iterations"), 10),
            .max_tool_output_bytes = (size_t)sc_config_get_int(&config, sc_str_from_cstr("agent.max_tool_result_chars"), 50000),
            .max_prompt_bytes = (size_t)sc_config_get_int(&config, sc_str_from_cstr("agent.max_system_prompt_chars"), 0),
        };
        status = sc_agent_new(alloc, &agent_options, &owned.agent);
    }
    if (sc_status_is_ok(status)) {
        status = create_channels_from_config(alloc, &config, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_transcriber_from_config(alloc, &config, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_tts_from_config(alloc, &config, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_orchestrator_from_config(alloc,
                                                 &config,
                                                 sc_string_as_str(&workspace),
                                                 &owned,
                                                 &allowed,
                                                 &allowed_senders,
                                                 &allowed_raw,
                                                 &stream_mode,
                                                 &session_db,
                                                 &session_dir);
    }
    if (sc_status_is_ok(status)) {
        status = create_gateway_from_config(alloc, &config, options, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_delivery_from_config(alloc, &config, &owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_heartbeat_from_config(alloc, &config, sc_string_as_str(&workspace), &owned);
    }
    if (sc_status_is_ok(status)) {
        bool hard_shutdown =
            options->struct_size >= offsetof(sc_boot_options, hard_shutdown) + sizeof(options->hard_shutdown) &&
            options->hard_shutdown;
        status = run_runtime_loop(&owned, alloc, options->once, options->max_polls, hard_shutdown);
    }

    sc_free(alloc, allowed_senders, allowed.len * sizeof(*allowed_senders), _Alignof(sc_str));
    clear_string_vec(&allowed);
    boot_owned_clear(&owned);
    sc_string_clear(&stream_mode);
    sc_string_clear(&allowed_raw);
    sc_string_clear(&session_dir);
    sc_string_clear(&session_db);
    sc_string_clear(&model);
    sc_string_clear(&identity);
    sc_string_clear(&workspace);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_secret_store_destroy(secret_store);
    sc_string_clear(&config_body);
    return status;
}

sc_status sc_boot_session_open(sc_allocator *alloc, const sc_boot_options *options, sc_boot_session **out)
{
    sc_boot_session *session = nullptr;
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_secret_store *secret_store = nullptr;
    sc_string config_body = {0};
    sc_string workspace = {0};
    sc_string identity = {0};
    sc_string model = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || options->config_path.ptr == nullptr || options->config_path.len == 0) {
        return sc_status_invalid_argument("sc.bootstrap.session_invalid_argument");
    }
    *out = nullptr;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    session = sc_alloc(alloc, sizeof(*session), _Alignof(sc_boot_session));
    if (session == nullptr) {
        return sc_status_no_memory();
    }
    *session = (sc_boot_session){.alloc = alloc, .next_turn_id = 1};

    status = boot_prepare_agent(alloc,
                                options,
                                &session->owned,
                                &workspace,
                                &identity,
                                &model,
                                &config,
                                &diag,
                                &secret_store,
                                &config_body);
    if (sc_status_is_ok(status)) {
        *out = session;
        session = nullptr;
    }

    if (session != nullptr) {
        boot_owned_clear(&session->owned);
        sc_free(alloc, session, sizeof(*session), _Alignof(sc_boot_session));
    }
    sc_secret_store_destroy(secret_store);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&model);
    sc_string_clear(&identity);
    sc_string_clear(&workspace);
    sc_string_clear(&config_body);
    return status;
}

sc_status sc_boot_session_process(sc_boot_session *session,
                                  sc_str input,
                                  sc_allocator *alloc,
                                  sc_runtime_response *out)
{
    return sc_boot_session_process_ex(session, input, nullptr, nullptr, nullptr, nullptr, alloc, out);
}

sc_status sc_boot_session_process_ex(sc_boot_session *session,
                                     sc_str input,
                                     sc_turn_tool_approval_fn approval,
                                     void *approval_user_data,
                                     sc_turn_event_fn event_callback,
                                     void *event_callback_user_data,
                                     sc_allocator *alloc,
                                     sc_runtime_response *out)
{
    sc_agent_turn_result result = {0};
    sc_turn turn = {0};
    sc_status status = sc_status_ok();

    if (session == nullptr || session->owned.agent == nullptr || out == nullptr || input.ptr == nullptr || input.len == 0) {
        return sc_status_invalid_argument("sc.bootstrap.session_process_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_runtime_response){.struct_size = sizeof(*out)};
    out->turn_id = session->next_turn_id++;
    turn = (sc_turn){
        .struct_size = sizeof(turn),
        .input = input,
        .session_id = sc_str_from_cstr("cli"),
        .turn_id = out->turn_id,
        .request_tool_approval = approval,
        .request_tool_approval_user_data = approval_user_data,
        .event_callback = event_callback,
        .event_callback_user_data = event_callback_user_data,
    };
    status = sc_agent_process_message(session->owned.agent, &turn, alloc, &result);
    out->output = result.output;
    result.output = (sc_string){0};
    out->receipts = result.receipts;
    result.receipts = (sc_receipt_chain){0};
    out->events = result.events;
    result.events = (sc_vec){0};
    out->provider_call_count = result.provider_call_count;
    out->tool_call_count = result.tool_call_count;
    out->input_tokens = result.input_tokens;
    out->output_tokens = result.output_tokens;
    out->total_tokens = result.total_tokens;
    out->cost_usd = result.cost_usd;
    out->budget_exceeded = result.budget_exceeded;
    out->model_switched = result.model_switched;
    out->active_model = result.active_model;
    result.active_model = (sc_string){0};
    out->cancelled = result.cancelled;
    out->timed_out = result.timed_out;
    sc_agent_turn_result_clear(&result);
    return status;
}

void sc_boot_session_destroy(sc_boot_session *session)
{
    if (session == nullptr) {
        return;
    }
    boot_owned_clear(&session->owned);
    sc_free(session->alloc, session, sizeof(*session), _Alignof(sc_boot_session));
}

static sc_status boot_prepare_agent(sc_allocator *alloc,
                                    const sc_boot_options *options,
                                    boot_owned *owned,
                                    sc_string *workspace,
                                    sc_string *identity,
                                    sc_string *model,
                                    sc_config *config,
                                    sc_config_diag *diag,
                                    sc_secret_store **secret_store,
                                    sc_string *config_body)
{
    sc_config_load_options load = {0};
    sc_status status = sc_status_ok();

    if (alloc == nullptr || options == nullptr || owned == nullptr || workspace == nullptr || identity == nullptr ||
        model == nullptr || config == nullptr || diag == nullptr || secret_store == nullptr || config_body == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.prepare_invalid_argument");
    }

    status = read_file(alloc, options->config_path, config_body);
    if (sc_status_is_ok(status)) {
        load.explicit_file = (sc_config_source){
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = options->config_path,
            .body = sc_string_as_str(config_body),
            .present = true,
        };
        status = sc_config_load(alloc, &load, config, diag);
    }
    if (sc_status_is_ok(status)) {
        status = sc_secret_store_file_new(alloc, sc_str_from_parts(nullptr, 0), secret_store);
        if (sc_status_is_ok(status)) {
            status = sc_config_attach_secret_store(config, *secret_store, true);
        } else if (status.error_key != nullptr && strcmp(status.error_key, "sc.secret_store.file_unsupported") == 0) {
            sc_status_clear(&status);
            status = sc_status_ok();
        }
    }
    if (sc_status_is_ok(status)) {
        status = build_workspace_path(alloc, options, options->config_path, workspace);
    }
    if (sc_status_is_ok(status)) {
        status = ensure_dir(alloc, sc_string_as_str(workspace));
    }
    if (sc_status_is_ok(status)) {
        status = init_workspace_state(alloc, sc_string_as_str(workspace));
    }
    if (sc_status_is_ok(status)) {
        status = sc_security_policy_from_config(&owned->policy, config);
    }
    if (sc_status_is_ok(status)) {
        status = sc_security_policy_set_workspace(&owned->policy, sc_string_as_str(workspace));
    }
    if (sc_status_is_ok(status)) {
        sc_estop_init(&owned->estop, alloc);
        status = load_estop_from_workspace(alloc, sc_string_as_str(workspace), &owned->estop);
    }
    if (sc_status_is_ok(status)) {
        status = build_identity(alloc, sc_string_as_str(workspace), identity);
    }
    if (sc_status_is_ok(status)) {
        status = create_provider_from_config(alloc, config, owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_memory_from_config(alloc, config, sc_string_as_str(workspace), &owned->memory);
    }
    if (sc_status_is_ok(status)) {
        status = create_cron_from_config(alloc, config, sc_string_as_str(workspace), owned);
    }
    if (sc_status_is_ok(status)) {
        status = create_tools(alloc, config, owned->memory, &owned->policy, &owned->estop, owned);
    }
    if (sc_status_is_ok(status)) {
        status = resolve_default_model(alloc, config, model);
    }
    if (sc_status_is_ok(status)) {
        sc_agent_options agent_options = {
            .struct_size = sizeof(sc_agent_options),
            .provider = owned->provider,
            .memory = owned->memory,
            .tools = owned->tools,
            .tool_count = owned->tool_count,
            .policy = &owned->policy,
            .estop = &owned->estop,
            .model = sc_string_as_str(model),
            .identity = sc_string_as_str(identity),
            .workspace = sc_string_as_str(workspace),
            .runtime_environment = sc_str_from_cstr("smolclaw-c/schema-v2"),
            .memory_namespace = sc_str_from_cstr("default"),
            .turn_namespace = sc_str_from_cstr("default"),
            .max_history_messages = (size_t)sc_config_get_int(config, sc_str_from_cstr("agent.max_history_messages"), 50),
            .max_tool_iterations = (size_t)sc_config_get_int(config, sc_str_from_cstr("agent.max_tool_iterations"), 10),
            .max_tool_output_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("agent.max_tool_result_chars"), 50000),
            .max_prompt_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("agent.max_system_prompt_chars"), 0),
        };
        status = sc_agent_new(alloc, &agent_options, &owned->agent);
    }
    return status;
}

static sc_status read_file(sc_allocator *alloc, sc_str path, sc_string *out)
{
    FILE *file = nullptr;
    long size = 0;
    char *buffer = nullptr;
    sc_string path_cstr = {0};
    sc_status status = sc_status_ok();

    if (path.ptr == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.read_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = copy_c_path(alloc, path, "sc.bootstrap.read_invalid_path", &path_cstr);
    if (!sc_status_is_ok(status)) {
        goto cleanup;
    }
    file = fopen(path_cstr.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.bootstrap.read_open_failed");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        status = sc_status_io("sc.bootstrap.read_stat_failed");
        goto cleanup;
    }
    if ((uintmax_t)size > (uintmax_t)SIZE_MAX - 1U) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    buffer = sc_alloc(alloc, (size_t)size + 1, _Alignof(char));
    if (buffer == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    if (size > 0 && fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        status = sc_status_io("sc.bootstrap.read_failed");
    }
    if (sc_status_is_ok(status)) {
        buffer[size] = '\0';
        *out = (sc_string){.ptr = buffer, .len = (size_t)size, .alloc = alloc};
        buffer = nullptr;
    }

cleanup:
    if (file != nullptr && fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.bootstrap.read_close_failed");
    }
    if (buffer != nullptr) {
        sc_free(alloc, buffer, (size_t)(size < 0 ? 0 : size) + 1, _Alignof(char));
    }
    sc_string_clear(&path_cstr);
    return status;
}

static sc_status copy_c_path(sc_allocator *alloc, sc_str path, const char *error_key, sc_string *out)
{
    if (path.ptr == nullptr || path.len == 0 || out == nullptr) {
        return sc_status_invalid_argument(error_key);
    }
    if (memchr(path.ptr, '\0', path.len) != nullptr) {
        return sc_status_invalid_argument(error_key);
    }
    return sc_string_from_str(alloc, path, out);
}

static sc_status path_dirname(sc_allocator *alloc, sc_str path, sc_string *out)
{
    size_t len = path.len;
    while (len > 0 && path.ptr[len - 1] != '/') {
        len -= 1;
    }
    if (len == 0) {
        return sc_string_from_cstr(alloc, ".", out);
    }
    return sc_string_from_str(alloc, sc_str_from_parts(path.ptr, len - 1), out);
}

static sc_status join_path(sc_allocator *alloc, sc_str left, sc_str right, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    if (right.ptr != nullptr && right.len > 0 && right.ptr[0] == '/') {
        return sc_string_from_str(alloc, right, out);
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, left);
    if (sc_status_is_ok(status) && (left.len == 0 || left.ptr[left.len - 1] != '/')) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, right);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status ensure_dir(sc_allocator *alloc, sc_str path)
{
    char *mutable_path = nullptr;
    sc_status status = sc_status_ok();

    if (path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.bootstrap.mkdir_invalid_argument");
    }

    mutable_path = sc_alloc(alloc, path.len + 1, _Alignof(char));
    if (mutable_path == nullptr) {
        return sc_status_no_memory();
    }
    memcpy(mutable_path, path.ptr, path.len);
    mutable_path[path.len] = '\0';

    for (size_t i = 1; sc_status_is_ok(status) && i < path.len; i += 1) {
        if (mutable_path[i] != '/') {
            continue;
        }
        mutable_path[i] = '\0';
        if (mutable_path[0] != '\0' && mkdir(mutable_path, 0700) != 0 && errno != EEXIST) {
            status = sc_status_io("sc.bootstrap.mkdir_failed");
        }
        mutable_path[i] = '/';
    }
    if (sc_status_is_ok(status) && mkdir(mutable_path, 0700) != 0 && errno != EEXIST) {
        status = sc_status_io("sc.bootstrap.mkdir_failed");
    }

    sc_free(alloc, mutable_path, path.len + 1, _Alignof(char));
    return status;
}

static sc_status get_prop_copy(const sc_config *config, sc_str path, sc_allocator *alloc, sc_string *out)
{
    return sc_config_get_prop(config, path, alloc, out);
}

static sc_status get_model_prop(sc_allocator *alloc,
                                const sc_config *config,
                                sc_str provider,
                                const char *name,
                                sc_string *out)
{
    sc_string path = {0};
    sc_status status = build_model_prop_path(alloc, provider, name, &path);

    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_string_as_str(&path), alloc, out);
    }
    sc_string_clear(&path);
    return status;
}

static sc_status build_model_prop_path(sc_allocator *alloc, sc_str provider, const char *name, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (name == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.provider_path_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "providers.models.");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, provider);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status try_secret_store_prop(const sc_config *config,
                                       sc_allocator *alloc,
                                       sc_str path,
                                       sc_string *out,
                                       bool *found)
{
    sc_status status = sc_status_ok();

    if (found != nullptr) {
        *found = false;
    }
    if (out != nullptr) {
        *out = (sc_string){0};
    }
    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.secret_store_invalid_argument");
    }
    if (!config->secret_store_enabled || config->secret_store == nullptr) {
        return sc_status_ok();
    }
    status = sc_secret_store_get(config->secret_store, path, alloc, out);
    if (!sc_status_is_ok(status)) {
        if (status.error_key != nullptr && strcmp(status.error_key, "sc.secret_store.not_found") == 0) {
            sc_status_clear(&status);
            return sc_status_ok();
        }
        return status;
    }
    if (out->len == 0) {
        sc_string_secure_clear(out);
        return sc_status_ok();
    }
    if (found != nullptr) {
        *found = true;
    }
    return sc_status_ok();
}

static sc_status try_model_secret_store_prop(const sc_config *config,
                                             sc_allocator *alloc,
                                             sc_str provider,
                                             const char *name,
                                             sc_string *out,
                                             bool *found)
{
    sc_string path = {0};
    sc_status status = build_model_prop_path(alloc, provider, name, &path);

    if (sc_status_is_ok(status)) {
        status = try_secret_store_prop(config, alloc, sc_string_as_str(&path), out, found);
    }
    sc_string_clear(&path);
    return status;
}

static sc_status get_model_route_prop(sc_allocator *alloc,
                                      const sc_config *config,
                                      sc_str provider,
                                      size_t index,
                                      const char *name,
                                      sc_string *out)
{
    sc_string_builder builder = {0};
    sc_string path = {0};
    char index_text[32] = {0};
    int written = snprintf(index_text, sizeof(index_text), "%zu", index);
    sc_status status = sc_status_ok();

    if (written < 0 || (size_t)written >= sizeof(index_text)) {
        return sc_status_no_memory();
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "providers.models.");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, provider);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".routes.");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, index_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &path);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_string_as_str(&path), alloc, out);
    }
    sc_string_clear(&path);
    return status;
}

static double get_model_double(sc_allocator *alloc,
                               const sc_config *config,
                               sc_str provider,
                               const char *name,
                               double fallback)
{
    sc_string value = {0};
    char *end = nullptr;
    double parsed = fallback;

    if (sc_status_is_ok(get_model_prop(alloc, config, provider, name, &value))) {
        parsed = strtod(value.ptr, &end);
        if (end == value.ptr) {
            parsed = fallback;
        }
    }
    sc_string_clear(&value);
    return parsed;
}

static int64_t get_model_int(sc_allocator *alloc,
                             const sc_config *config,
                             sc_str provider,
                             const char *name,
                             int64_t fallback)
{
    sc_string value = {0};
    char *end = nullptr;
    int64_t parsed = fallback;

    if (sc_status_is_ok(get_model_prop(alloc, config, provider, name, &value))) {
        parsed = strtoll(value.ptr, &end, 10);
        if (end == value.ptr) {
            parsed = fallback;
        }
    }
    sc_string_clear(&value);
    return parsed;
}

static bool get_model_bool(sc_allocator *alloc,
                           const sc_config *config,
                           sc_str provider,
                           const char *name,
                           bool fallback)
{
    sc_string value = {0};
    bool parsed = fallback;

    if (sc_status_is_ok(get_model_prop(alloc, config, provider, name, &value))) {
        sc_str text = sc_string_as_str(&value);
        parsed = (text.len == 4 && memcmp(text.ptr, "true", 4) == 0) ||
                 (text.len == 1 && text.ptr[0] == '1');
        if (!parsed &&
            !((text.len == 5 && memcmp(text.ptr, "false", 5) == 0) ||
              (text.len == 1 && text.ptr[0] == '0'))) {
            parsed = fallback;
        }
    }
    sc_string_clear(&value);
    return parsed;
}

static bool model_prop_present(sc_allocator *alloc, const sc_config *config, sc_str provider, const char *name)
{
    sc_string value = {0};
    sc_status status = get_model_prop(alloc, config, provider, name, &value);
    bool present = sc_status_is_ok(status);
    sc_status_clear(&status);
    sc_string_clear(&value);
    return present;
}

static sc_str provider_default_credential_env(sc_str kind)
{
    if (provider_is(kind, "anthropic")) {
        return sc_str_from_cstr("ANTHROPIC_API_KEY");
    }
    if (provider_is(kind, "openai") || provider_is(kind, "azure-openai")) {
        return sc_str_from_cstr("OPENAI_API_KEY");
    }
    if (provider_is(kind, "openrouter")) {
        return sc_str_from_cstr("OPENROUTER_API_KEY");
    }
    if (provider_is(kind, "gemini")) {
        return sc_str_from_cstr("GEMINI_API_KEY");
    }
    if (provider_is(kind, "copilot")) {
        return sc_str_from_cstr("GITHUB_COPILOT_TOKEN");
    }
    if (provider_is(kind, "bedrock")) {
        return sc_str_from_cstr("AWS_ACCESS_KEY_ID");
    }
    if (provider_is(kind, "groq")) {
        return sc_str_from_cstr("GROQ_API_KEY");
    }
    if (provider_is(kind, "mistral")) {
        return sc_str_from_cstr("MISTRAL_API_KEY");
    }
    if (provider_is(kind, "xai") || provider_is(kind, "grok")) {
        return sc_str_from_cstr("XAI_API_KEY");
    }
    if (provider_is(kind, "deepseek")) {
        return sc_str_from_cstr("DEEPSEEK_API_KEY");
    }
    if (provider_is(kind, "moonshot")) {
        return sc_str_from_cstr("MOONSHOT_API_KEY");
    }
    if (provider_is(kind, "zai") || provider_is(kind, "z.ai") || provider_is(kind, "glm")) {
        return sc_str_from_cstr("ZAI_API_KEY");
    }
    if (provider_is(kind, "minimax")) {
        return sc_str_from_cstr("MINIMAX_API_KEY");
    }
    if (provider_is(kind, "qianfan")) {
        return sc_str_from_cstr("QIANFAN_API_KEY");
    }
    if (provider_is(kind, "venice")) {
        return sc_str_from_cstr("VENICE_API_KEY");
    }
    if (provider_is(kind, "vercel-ai-gateway") || provider_is(kind, "vercel")) {
        return sc_str_from_cstr("VERCEL_AI_GATEWAY_API_KEY");
    }
    if (provider_is(kind, "cloudflare-gateway") || provider_is(kind, "cloudflare")) {
        return sc_str_from_cstr("CLOUDFLARE_API_TOKEN");
    }
    if (provider_is(kind, "opencode")) {
        return sc_str_from_cstr("OPENCODE_API_KEY");
    }
    if (provider_is(kind, "synthetic")) {
        return sc_str_from_cstr("SYNTHETIC_API_KEY");
    }
    return sc_str_from_cstr("");
}

static bool provider_is_compatible_preset(sc_str kind)
{
    return provider_is(kind, "groq") ||
           provider_is(kind, "mistral") ||
           provider_is(kind, "xai") ||
           provider_is(kind, "grok") ||
           provider_is(kind, "deepseek") ||
           provider_is(kind, "moonshot") ||
           provider_is(kind, "zai") ||
           provider_is(kind, "z.ai") ||
           provider_is(kind, "glm") ||
           provider_is(kind, "minimax") ||
           provider_is(kind, "qianfan") ||
           provider_is(kind, "venice") ||
           provider_is(kind, "vercel-ai-gateway") ||
           provider_is(kind, "vercel") ||
           provider_is(kind, "cloudflare-gateway") ||
           provider_is(kind, "cloudflare") ||
           provider_is(kind, "opencode") ||
           provider_is(kind, "synthetic");
}

static sc_status resolve_default_provider(sc_allocator *alloc, const sc_config *config, sc_string *out)
{
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.provider_invalid_argument");
    }

    status = get_prop_copy(config, sc_str_from_cstr("providers.fallback"), alloc, out);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = get_prop_copy(config, sc_str_from_cstr("provider.default"), alloc, out);
    }
    if (sc_status_is_ok(status) && out->len == 0) {
        sc_string_clear(out);
        status = sc_string_from_cstr(alloc, "mock", out);
    }
    return status;
}

static sc_status resolve_default_model(sc_allocator *alloc, const sc_config *config, sc_string *out)
{
    sc_string provider = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.provider_invalid_argument");
    }

    status = resolve_default_provider(alloc, config, &provider);
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, sc_string_as_str(&provider), "model", out);
    }
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = get_prop_copy(config, sc_str_from_cstr("provider.default_model"), alloc, out);
    }

    sc_string_clear(&provider);
    return status;
}

static sc_status build_workspace_path(sc_allocator *alloc,
                                      const sc_boot_options *options,
                                      sc_str config_path,
                                      sc_string *out)
{
    sc_string root = {0};
    sc_status status = sc_status_ok();
    if (options->workspace_path.len > 0) {
        return sc_string_from_str(alloc, options->workspace_path, out);
    }
    status = path_dirname(alloc, config_path, &root);
    if (sc_status_is_ok(status)) {
        status = join_path(alloc, sc_string_as_str(&root), sc_str_from_cstr("workspace"), out);
    }
    sc_string_clear(&root);
    return status;
}

static sc_status init_workspace_state(sc_allocator *alloc, sc_str workspace)
{
    const char *dirs[] = {"memory", "sessions", "receipts", "cache", "state"};
    sc_string dir = {0};
    sc_string state_dir = {0};
    sc_string state_path = {0};
    sc_estop_state estop = {0};
    sc_status status = sc_status_ok();

    if (workspace.ptr == nullptr || workspace.len == 0) {
        return sc_status_invalid_argument("sc.bootstrap.workspace_state_invalid_argument");
    }

    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(dirs); i += 1) {
        status = join_path(alloc, workspace, sc_str_from_cstr(dirs[i]), &dir);
        if (sc_status_is_ok(status)) {
            status = ensure_dir(alloc, sc_string_as_str(&dir));
        }
        sc_string_clear(&dir);
    }

    if (sc_status_is_ok(status)) {
        status = join_path(alloc, workspace, sc_str_from_cstr("state"), &state_dir);
    }
    if (sc_status_is_ok(status)) {
        status = join_path(alloc, sc_string_as_str(&state_dir), sc_str_from_cstr("emergency_stop.state"), &state_path);
    }
    if (sc_status_is_ok(status)) {
        FILE *file = fopen(state_path.ptr, "rb");
        if (file != nullptr) {
            (void)fclose(file);
        } else if (errno == ENOENT) {
            sc_estop_init(&estop, alloc);
            sc_estop_reset(&estop);
            status = sc_estop_write_file(&estop, sc_string_as_str(&state_path));
        } else {
            status = sc_status_io("sc.bootstrap.estop_state_open_failed");
        }
    }

    sc_estop_clear(&estop);
    sc_string_clear(&state_path);
    sc_string_clear(&state_dir);
    sc_string_clear(&dir);
    return status;
}

static sc_status load_estop_from_workspace(sc_allocator *alloc, sc_str workspace, sc_estop_state *estop)
{
    sc_string state_dir = {0};
    sc_string state_path = {0};
    sc_status status = sc_status_ok();

    if (estop == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.estop_invalid_argument");
    }

    status = join_path(alloc, workspace, sc_str_from_cstr("state"), &state_dir);
    if (sc_status_is_ok(status)) {
        status = join_path(alloc, sc_string_as_str(&state_dir), sc_str_from_cstr("emergency_stop.state"), &state_path);
    }
    if (sc_status_is_ok(status)) {
        FILE *file = fopen(state_path.ptr, "rb");
        if (file == nullptr && errno == ENOENT) {
            status = sc_status_ok();
        } else if (file == nullptr) {
            status = sc_status_io("sc.bootstrap.estop_read_open_failed");
        } else {
            (void)fclose(file);
            status = sc_estop_read_file(alloc, sc_string_as_str(&state_path), estop);
        }
    }

    sc_string_clear(&state_path);
    sc_string_clear(&state_dir);
    return status;
}

static sc_status build_identity(sc_allocator *alloc, sc_str workspace, sc_string *out)
{
    sc_string_builder builder = {0};
    const char *files[] = {"AGENTS.md", "SOUL.md", "USER.md", "IDENTITY.md", "MEMORY.md", "TOOLS.md", "HEARTBEAT.md"};
    sc_status status = sc_status_ok();
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(files); i += 1) {
        status = append_workspace_file(alloc, &builder, workspace, files[i]);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_status_ok();
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_workspace_file(sc_allocator *alloc,
                                       sc_string_builder *builder,
                                       sc_str workspace,
                                       const char *name)
{
    sc_string path = {0};
    sc_string body = {0};
    sc_status status = join_path(alloc, workspace, sc_str_from_cstr(name), &path);
    if (sc_status_is_ok(status)) {
        status = read_file(alloc, sc_string_as_str(&path), &body);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n# ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, sc_string_as_str(&body));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    sc_string_clear(&body);
    sc_string_clear(&path);
    return status;
}

static sc_status create_provider_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned)
{
    sc_string provider = {0};
    sc_string kind = {0};
    sc_string fallback_raw = {0};
    sc_vec fallbacks = {0};
    sc_provider *providers[BOOT_MAX_RELIABLE_PROVIDERS] = {0};
    size_t provider_count = 0;
    sc_status status = resolve_default_provider(alloc, config, &provider);

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.provider_invalid_argument");
    }
    sc_vec_init(&fallbacks, alloc, sizeof(sc_string));
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, sc_string_as_str(&provider), "kind", &kind);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_str(alloc, sc_string_as_str(&provider), &kind);
        }
    }
    if (sc_status_is_ok(status) && provider_is(sc_string_as_str(&kind), "reliable")) {
        status = get_model_prop(alloc, config, sc_string_as_str(&provider), "fallback_providers", &fallback_raw);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = get_prop_copy(config, sc_str_from_cstr("reliability.fallback_providers"), alloc, &fallback_raw);
        }
        if (!sc_status_is_ok(status)) {
            status = sc_status_invalid_argument("sc.bootstrap.reliable_missing_fallbacks");
        } else {
            status = parse_string_array(alloc, sc_string_as_str(&fallback_raw), &fallbacks);
        }
        for (size_t i = 0; sc_status_is_ok(status) && i < fallbacks.len && provider_count < SC_ARRAY_LEN(providers); i += 1) {
            const sc_string *fallback = sc_vec_at_const(&fallbacks, i);
            if (fallback != nullptr) {
                status = create_named_provider(alloc, config, sc_string_as_str(fallback), &providers[provider_count]);
                if (sc_status_is_ok(status)) {
                    status = boot_add_provider_handle(owned, providers[provider_count]);
                    if (sc_status_is_ok(status)) {
                        provider_count += 1;
                    } else {
                        sc_provider_destroy(providers[provider_count]);
                        providers[provider_count] = nullptr;
                    }
                }
            }
        }
        if (sc_status_is_ok(status)) {
            status = sc_provider_reliable_new_with_options(alloc,
                                                           providers,
                                                           provider_count,
                                                           (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.max_retries"), 2),
                                                           (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.retry_backoff_ms"), 250),
                                                           &owned->provider);
        }
    } else if (sc_status_is_ok(status) && provider_is(sc_string_as_str(&kind), "router")) {
        sc_string default_provider = {0};
        sc_string tool_provider_name = {0};
        sc_string route_hints[BOOT_MAX_ROUTER_ROUTES] = {0};
        sc_provider *default_handle = nullptr;
        sc_provider *tool_handle = nullptr;
        sc_provider_route routes[BOOT_MAX_ROUTER_ROUTES] = {0};
        size_t route_count = 0;

        status = get_model_prop(alloc, config, sc_string_as_str(&provider), "default", &default_provider);
        if (sc_status_is_ok(status)) {
            status = create_named_provider(alloc, config, sc_string_as_str(&default_provider), &default_handle);
            if (sc_status_is_ok(status)) {
                status = boot_add_provider_handle(owned, default_handle);
                if (!sc_status_is_ok(status)) {
                    sc_provider_destroy(default_handle);
                    default_handle = nullptr;
                }
            }
        }
        if (sc_status_is_ok(status)) {
            status = get_model_prop(alloc, config, sc_string_as_str(&provider), "tool_provider", &tool_provider_name);
            if (sc_status_is_ok(status) && tool_provider_name.len > 0) {
                status = create_named_provider(alloc, config, sc_string_as_str(&tool_provider_name), &tool_handle);
                if (sc_status_is_ok(status)) {
                    status = boot_add_provider_handle(owned, tool_handle);
                    if (!sc_status_is_ok(status)) {
                        sc_provider_destroy(tool_handle);
                        tool_handle = nullptr;
                    }
                }
            } else {
                sc_status_clear(&status);
                status = sc_status_ok();
            }
        }
        for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(routes); i += 1) {
            sc_string hint = {0};
            sc_string routed_provider = {0};
            sc_provider *route_handle = nullptr;
            status = get_model_route_prop(alloc, config, sc_string_as_str(&provider), i, "hint", &hint);
            if (!sc_status_is_ok(status)) {
                sc_status_clear(&status);
                status = sc_status_ok();
                sc_string_clear(&hint);
                break;
            }
            status = get_model_route_prop(alloc, config, sc_string_as_str(&provider), i, "provider", &routed_provider);
            if (sc_status_is_ok(status)) {
                status = create_named_provider(alloc, config, sc_string_as_str(&routed_provider), &route_handle);
            }
            if (sc_status_is_ok(status)) {
                status = boot_add_provider_handle(owned, route_handle);
                if (sc_status_is_ok(status)) {
                    route_hints[route_count] = hint;
                    hint = (sc_string){0};
                    routes[route_count] = (sc_provider_route){
                        .struct_size = sizeof(routes[route_count]),
                        .hint = sc_string_as_str(&route_hints[route_count]),
                        .provider = route_handle,
                    };
                    route_count += 1;
                } else {
                    sc_provider_destroy(route_handle);
                    route_handle = nullptr;
                }
            }
            sc_string_clear(&routed_provider);
            sc_string_clear(&hint);
        }
        if (sc_status_is_ok(status)) {
            if (route_count > 0) {
                status = sc_provider_router_routes_new(alloc, default_handle, routes, route_count, nullptr, &owned->provider);
            } else {
                status = sc_provider_router_new(alloc, default_handle, tool_handle, nullptr, &owned->provider);
            }
        }
        for (size_t i = 0; i < SC_ARRAY_LEN(route_hints); i += 1) {
            sc_string_clear(&route_hints[i]);
        }
        sc_string_clear(&tool_provider_name);
        sc_string_clear(&default_provider);
    } else {
        if (sc_status_is_ok(status)) {
            status = create_named_provider(alloc, config, sc_string_as_str(&provider), &providers[provider_count]);
            if (sc_status_is_ok(status)) {
                status = boot_add_provider_handle(owned, providers[provider_count]);
                if (sc_status_is_ok(status)) {
                    provider_count += 1;
                } else {
                    sc_provider_destroy(providers[provider_count]);
                    providers[provider_count] = nullptr;
                }
            }
        }
        if (sc_status_is_ok(status)) {
            status = get_prop_copy(config, sc_str_from_cstr("reliability.fallback_providers"), alloc, &fallback_raw);
            if (!sc_status_is_ok(status)) {
                sc_status_clear(&status);
                status = get_model_prop(alloc, config, sc_string_as_str(&provider), "fallback_providers", &fallback_raw);
            }
            if (!sc_status_is_ok(status)) {
                sc_status_clear(&status);
                status = sc_status_ok();
            } else {
                status = parse_string_array(alloc, sc_string_as_str(&fallback_raw), &fallbacks);
            }
        }
        for (size_t i = 0; sc_status_is_ok(status) && i < fallbacks.len && provider_count < SC_ARRAY_LEN(providers); i += 1) {
            const sc_string *fallback = sc_vec_at_const(&fallbacks, i);
            if (fallback != nullptr && !sc_str_equal(sc_string_as_str(fallback), sc_string_as_str(&provider))) {
                status = create_named_provider(alloc, config, sc_string_as_str(fallback), &providers[provider_count]);
                if (sc_status_is_ok(status)) {
                    status = boot_add_provider_handle(owned, providers[provider_count]);
                    if (sc_status_is_ok(status)) {
                        provider_count += 1;
                    } else {
                        sc_provider_destroy(providers[provider_count]);
                        providers[provider_count] = nullptr;
                    }
                }
            }
        }
        if (sc_status_is_ok(status) && provider_count == 1) {
            owned->provider = providers[0];
        } else if (sc_status_is_ok(status)) {
            status = sc_provider_reliable_new_with_options(alloc,
                                                           providers,
                                                           provider_count,
                                                           (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.max_retries"), 2),
                                                           (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.retry_backoff_ms"), 250),
                                                           &owned->provider);
        }
    }
    clear_string_vec(&fallbacks);
    sc_string_clear(&fallback_raw);
    sc_string_clear(&kind);
    sc_string_clear(&provider);
    return status;
}

static sc_status boot_add_provider_handle(boot_owned *owned, sc_provider *provider)
{
    if (owned == nullptr || provider == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.provider_handle_invalid_argument");
    }
    if (owned->provider_handle_count >= SC_ARRAY_LEN(owned->provider_handles)) {
        return sc_status_unsupported("sc.bootstrap.too_many_provider_handles");
    }
    owned->provider_handles[owned->provider_handle_count] = provider;
    owned->provider_handle_count += 1;
    return sc_status_ok();
}

static sc_status create_named_provider(sc_allocator *alloc,
                                       const sc_config *config,
                                       sc_str provider,
                                       sc_provider **out)
{
    sc_string kind = {0};
    sc_string model = {0};
    sc_string api_key = {0};
    sc_string credential_env = {0};
    sc_string generic_credential_env = {0};
    sc_string secret_value = {0};
    sc_string session_token = {0};
    sc_string base_url = {0};
    sc_string deployment = {0};
    sc_string api_version = {0};
    sc_string openrouter_referer = {0};
    sc_string openrouter_title = {0};
    sc_string generation_config = {0};
    sc_string safety_settings = {0};
    sc_string thinking_level = {0};
    sc_string reasoning_effort = {0};
    sc_string options_json = {0};
    sc_string format_json = {0};
    sc_string region = {0};
    sc_string command = {0};
    sc_string mcp_server_name = {0};
    sc_string mcp_tool = {0};
    sc_string mcp_prompt_field = {0};
    sc_mcp_server_view mcp_server = {0};
    bool mcp_server_resolved = false;
    bool secret_found = false;
    bool think_set = false;
    bool think = false;
    sc_status status = get_model_prop(alloc, config, provider, "kind", &kind);

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.provider_invalid_argument");
    }
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_str(alloc, provider, &kind);
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "model", &model);
    }
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = get_prop_copy(config, sc_str_from_cstr("provider.default_model"), alloc, &model);
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "api_key", &api_key);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = get_prop_copy(config, sc_str_from_cstr("provider.api_key"), alloc, &api_key);
            if (!sc_status_is_ok(status)) {
                sc_status_clear(&status);
                status = sc_string_from_cstr(alloc, "", &api_key);
            }
        }
    }
    if (sc_status_is_ok(status) && api_key.len == 0) {
        sc_string stored_api_key = {0};
        secret_found = false;
        status = try_model_secret_store_prop(config, alloc, provider, "api_key", &stored_api_key, &secret_found);
        if (sc_status_is_ok(status) && !secret_found) {
            status = try_secret_store_prop(config,
                                           alloc,
                                           sc_str_from_cstr("provider.api_key"),
                                           &stored_api_key,
                                           &secret_found);
        }
        if (sc_status_is_ok(status) && secret_found) {
            if (provider_is(sc_string_as_str(&kind), "bedrock")) {
                sc_string_secure_clear(&api_key);
                api_key = stored_api_key;
            } else {
                sc_string_secure_clear(&secret_value);
                secret_value = stored_api_key;
            }
            stored_api_key = (sc_string){0};
        }
        sc_string_secure_clear(&stored_api_key);
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "credential_env", &credential_env);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_str(alloc, provider_default_credential_env(sc_string_as_str(&kind)), &credential_env);
        }
    }
    if (sc_status_is_ok(status) && secret_value.len == 0) {
        status = get_model_prop(alloc, config, provider, "secret_access_key", &secret_value);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            secret_found = false;
            status = try_model_secret_store_prop(config, alloc, provider, "secret_access_key", &secret_value, &secret_found);
            if (sc_status_is_ok(status) && !secret_found) {
                status = sc_string_from_cstr(alloc, "", &secret_value);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "session_token", &session_token);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            secret_found = false;
            status = try_model_secret_store_prop(config, alloc, provider, "session_token", &session_token, &secret_found);
            if (sc_status_is_ok(status) && !secret_found) {
                status = sc_string_from_cstr(alloc, "", &session_token);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("provider.api_key_env"), alloc, &generic_credential_env);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "SMOLCLAW_PROVIDER_API_KEY", &generic_credential_env);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "base_url", &base_url);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &base_url);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "deployment", &deployment);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &deployment);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "api_version", &api_version);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &api_version);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "openrouter_referer", &openrouter_referer);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &openrouter_referer);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "openrouter_title", &openrouter_title);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &openrouter_title);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "generation_config", &generation_config);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &generation_config);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "options", &options_json);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &options_json);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "safety_settings", &safety_settings);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &safety_settings);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "reasoning_effort", &reasoning_effort);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &reasoning_effort);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "thinking_level", &thinking_level);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = reasoning_effort.len > 0 ? sc_string_from_str(alloc, sc_string_as_str(&reasoning_effort), &thinking_level) :
                                                sc_string_from_cstr(alloc, "", &thinking_level);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "format", &format_json);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &format_json);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "region", &region);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &region);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "command", &command);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &command);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "mcp_server", &mcp_server_name);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "claude-code", &mcp_server_name);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "mcp_tool", &mcp_tool);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "query", &mcp_tool);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_model_prop(alloc, config, provider, "mcp_prompt_field", &mcp_prompt_field);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "prompt", &mcp_prompt_field);
        }
    }
    if (sc_status_is_ok(status)) {
        think_set = model_prop_present(alloc, config, provider, "think");
        think = get_model_bool(alloc, config, provider, "think", true);
    }
    if (sc_status_is_ok(status)) {
        sc_provider_thinking_level parsed_thinking = SC_PROVIDER_THINKING_DEFAULT;
        status = sc_provider_parse_thinking_level(sc_string_as_str(&thinking_level), &parsed_thinking);
        if (!sc_status_is_ok(status)) {
            goto cleanup;
        }
        if (provider_is(sc_string_as_str(&kind), "claude-code")) {
            if (!sc_config_find_mcp_server(config, sc_string_as_str(&mcp_server_name), &mcp_server)) {
                status = sc_status_unsupported("sc.provider_claude_code.mcp_server_missing");
                goto cleanup;
            }
            if (!mcp_server.enabled) {
                status = sc_status_unsupported("sc.provider_claude_code.mcp_server_disabled");
                goto cleanup;
            }
            if (mcp_server.transport == SC_MCP_TRANSPORT_STDIO && mcp_server.command.len == 0) {
                status = sc_status_invalid_argument("sc.provider_claude_code.mcp_command_missing");
                goto cleanup;
            }
            if ((mcp_server.transport == SC_MCP_TRANSPORT_HTTP || mcp_server.transport == SC_MCP_TRANSPORT_SSE) &&
                mcp_server.url.len == 0) {
                status = sc_status_invalid_argument("sc.provider_claude_code.mcp_url_missing");
                goto cleanup;
            }
            if (mcp_server.transport == SC_MCP_TRANSPORT_UNKNOWN) {
                status = sc_status_invalid_argument("sc.provider_claude_code.mcp_transport_unknown");
                goto cleanup;
            }
            mcp_server_resolved = true;
        }
        sc_provider_options options = {
            .struct_size = sizeof(options),
            .provider_name = provider,
            .base_url = sc_string_as_str(&base_url),
            .api_key = sc_string_as_str(&api_key),
            .credential_env = sc_string_as_str(&credential_env),
            .generic_credential_env = sc_string_as_str(&generic_credential_env),
            .secret_value = sc_string_as_str(&secret_value),
            .session_token = sc_string_as_str(&session_token),
            .default_model = sc_string_as_str(&model),
            .deployment = sc_string_as_str(&deployment),
            .api_version = sc_string_as_str(&api_version),
            .timeout_ms = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.timeout_ms"), 30000),
            .temperature = get_model_double(alloc, config, provider, "temperature", 0.0),
            .top_p = get_model_double(alloc, config, provider, "top_p", 0.0),
            .top_k = get_model_int(alloc, config, provider, "top_k", 0),
            .max_output_tokens = get_model_int(alloc, config, provider, "max_output_tokens", 0),
            .generation_config_json = sc_string_as_str(&generation_config),
            .safety_settings_json = sc_string_as_str(&safety_settings),
            .reasoning_effort = sc_string_as_str(&reasoning_effort),
            .options_json = sc_string_as_str(&options_json),
            .format_json = sc_string_as_str(&format_json),
            .region = sc_string_as_str(&region),
            .command = mcp_server_resolved ? mcp_server.command : sc_string_as_str(&command),
            .mcp_server = sc_string_as_str(&mcp_server_name),
            .mcp_tool = sc_string_as_str(&mcp_tool),
            .mcp_prompt_field = sc_string_as_str(&mcp_prompt_field),
            .mcp_transport = mcp_server_resolved ? sc_mcp_transport_to_str(mcp_server.transport) : sc_str_from_cstr(""),
            .mcp_args = mcp_server_resolved ? mcp_server.args : sc_str_from_cstr(""),
            .mcp_url = mcp_server_resolved ? mcp_server.url : sc_str_from_cstr(""),
            .mcp_headers = mcp_server_resolved ? mcp_server.headers : sc_str_from_cstr(""),
            .streaming = get_model_bool(alloc, config, provider, "streaming", false),
            .validate_model = get_model_bool(alloc, config, provider, "validate_model", true),
            .max_retries = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.max_retries"), 2),
            .retry_backoff_ms = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.retry_backoff_ms"), 250),
            .merge_system_into_user = get_model_bool(alloc, config, provider, "merge_system_into_user", false),
            .thinking_level = parsed_thinking,
            .input_cost_per_million = get_model_double(alloc, config, provider, "input_cost_per_million", 0.0),
            .output_cost_per_million = get_model_double(alloc, config, provider, "output_cost_per_million", 0.0),
            .openrouter_referer = sc_string_as_str(&openrouter_referer),
            .openrouter_title = sc_string_as_str(&openrouter_title),
            .allow_loopback = get_model_bool(alloc, config, provider, "allow_loopback", false),
            .native_tool_streaming = get_model_bool(alloc, config, provider, "native_tool_streaming", false),
            .think = think,
            .think_set = think_set,
        };
        if (provider_is(sc_string_as_str(&kind), "gemini")) {
            status = sc_provider_gemini_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "anthropic")) {
            status = sc_provider_anthropic_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "openai")) {
            status = sc_provider_openai_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "openrouter")) {
            status = sc_provider_openrouter_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "openai-compatible")) {
            status = sc_provider_openai_compatible_http_new(alloc, &options, out);
        } else if (provider_is_compatible_preset(sc_string_as_str(&kind))) {
            status = sc_provider_openai_compatible_preset_new(alloc, sc_string_as_str(&kind), &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "azure-openai")) {
            status = sc_provider_azure_openai_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "ollama")) {
            status = sc_provider_ollama_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "llamacpp") || provider_is(sc_string_as_str(&kind), "llama.cpp")) {
            status = sc_provider_llamacpp_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "sglang")) {
            status = sc_provider_sglang_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "vllm")) {
            status = sc_provider_vllm_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "bedrock")) {
            status = sc_provider_bedrock_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "gemini-cli")) {
            status = sc_provider_gemini_cli_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "copilot")) {
            status = sc_provider_copilot_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "claude-code")) {
            status = sc_provider_claude_code_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "telnyx")) {
            status = sc_provider_telnyx_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "kilocli")) {
            status = sc_provider_kilocli_new(alloc, &options, out);
        } else if (provider_is(sc_string_as_str(&kind), "mock")) {
            status = sc_provider_mock_new(alloc, SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("mock assistant"), out);
        } else {
            status = sc_status_unsupported("sc.bootstrap.provider_kind_unsupported");
        }
    }
cleanup:
    sc_string_clear(&mcp_prompt_field);
    sc_string_clear(&mcp_tool);
    sc_string_clear(&mcp_server_name);
    sc_string_clear(&command);
    sc_string_clear(&region);
    sc_string_clear(&format_json);
    sc_string_clear(&options_json);
    sc_string_clear(&reasoning_effort);
    sc_string_clear(&thinking_level);
    sc_string_clear(&safety_settings);
    sc_string_clear(&generation_config);
    sc_string_clear(&openrouter_title);
    sc_string_clear(&openrouter_referer);
    sc_string_clear(&api_version);
    sc_string_clear(&deployment);
    sc_string_clear(&base_url);
    sc_string_secure_clear(&session_token);
    sc_string_secure_clear(&secret_value);
    sc_string_clear(&generic_credential_env);
    sc_string_clear(&credential_env);
    sc_string_secure_clear(&api_key);
    sc_string_clear(&model);
    sc_string_clear(&kind);
    return status;
}

static sc_status create_memory_from_config(sc_allocator *alloc, const sc_config *config, sc_str workspace, sc_memory **out)
{
    sc_string backend = {0};
    sc_string dir_rel = {0};
    sc_string db_rel = {0};
    sc_string dir = {0};
    sc_string path = {0};
    sc_status status = get_prop_copy(config, sc_str_from_cstr("memory.backend"), alloc, &backend);
    if (!sc_status_is_ok(status) || !sc_str_equal(sc_string_as_str(&backend), sc_str_from_cstr("sqlite"))) {
        sc_status_clear(&status);
        status = sc_memory_none_new(alloc, out);
    } else {
        status = sc_string_from_cstr(alloc, "memory", &dir_rel);
        if (sc_status_is_ok(status)) {
            status = join_path(alloc, workspace, sc_string_as_str(&dir_rel), &dir);
        }
        if (sc_status_is_ok(status)) {
            status = ensure_dir(alloc, sc_string_as_str(&dir));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_cstr(alloc, "memory/brain.db", &db_rel);
        }
        if (sc_status_is_ok(status)) {
            status = join_path(alloc, workspace, sc_string_as_str(&db_rel), &path);
        }
        if (sc_status_is_ok(status)) {
            status = sc_memory_sqlite_open(alloc, sc_string_as_str(&path), out);
        }
    }
    sc_string_clear(&path);
    sc_string_clear(&dir);
    sc_string_clear(&db_rel);
    sc_string_clear(&dir_rel);
    sc_string_clear(&backend);
    return status;
}

static sc_status create_tools(sc_allocator *alloc,
                              const sc_config *config,
                              sc_memory *memory,
                              const sc_security_policy *policy,
                              const sc_estop_state *estop,
                              boot_owned *owned)
{
    sc_tool_context context = {
        .struct_size = sizeof(context),
        .policy = policy,
        .estop = estop,
        .memory = memory,
        .max_output_bytes = 50000,
        .timeout_ms = sc_config_get_int(config, sc_str_from_cstr("reliability.timeout_ms"), 30000),
        .config = config,
        .cron_jobs = owned == nullptr || !owned->cron_initialized ? nullptr : &owned->cron_jobs,
        .cron_runs = owned == nullptr || !owned->cron_initialized ? nullptr : &owned->cron_runs,
        .tools = owned == nullptr ? nullptr : owned->tools,
        .tool_capacity = owned == nullptr ? 0 : SC_ARRAY_LEN(owned->tools),
    };
    sc_status status = sc_status_ok();

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.tools_invalid_argument");
    }
    if (sc_config_get_bool(config, sc_str_from_cstr("hardware.enabled"), false) && SC_ENABLE_HARDWARE == 0) {
        return sc_status_unsupported("sc.bootstrap.hardware_feature_disabled");
    }

    status = append_tool_if_exposed(alloc,
                                    config,
                                    &context,
                                    owned,
                                    sc_str_from_cstr("tool_diagnostics"),
                                    SC_TOOL_RISK_READONLY,
                                    SC_TOOL_CAPABILITY_NONE,
                                    SC_TOOL_SIDE_EFFECT_READ,
                                    sc_tool_tool_diagnostics_new);
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("policy_explain"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_policy_explain_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("tool_registry_list"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_tool_registry_list_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("dependency_status"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_dependency_status_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("capability_matrix"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_capability_matrix_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("resource_usage"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_resource_usage_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("approval_test"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_WRITE,
                                        sc_tool_approval_test_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("file_read"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_FILESYSTEM,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_file_read_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("file_write"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        SC_TOOL_CAPABILITY_FILESYSTEM,
                                        SC_TOOL_SIDE_EFFECT_WRITE,
                                        sc_tool_file_write_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("file_list"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_FILESYSTEM,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_file_list_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("content_search"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_FILESYSTEM,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_content_search_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("glob_search"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_FILESYSTEM,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_glob_search_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("shell"),
                                        SC_TOOL_RISK_SHELL,
                                        SC_TOOL_CAPABILITY_PROCESS,
                                        SC_TOOL_SIDE_EFFECT_PROCESS,
                                        sc_tool_shell_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("http"),
                                        SC_TOOL_RISK_NETWORK,
                                        SC_TOOL_CAPABILITY_NETWORK,
                                        SC_TOOL_SIDE_EFFECT_NETWORK,
                                        sc_tool_http_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("web_search"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NETWORK,
                                        SC_TOOL_SIDE_EFFECT_NETWORK,
                                        sc_tool_web_search_new);
    }
    if (sc_status_is_ok(status) && sc_config_get_bool(config, sc_str_from_cstr("browser.enabled"), true)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("browser"),
                                        SC_TOOL_RISK_NETWORK,
                                        SC_TOOL_CAPABILITY_BROWSER,
                                        SC_TOOL_SIDE_EFFECT_PROCESS,
                                        sc_tool_browser_new);
    }
    if (sc_status_is_ok(status) && sc_config_get_bool(config, sc_str_from_cstr("browser.enabled"), true)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("browser_screenshot"),
                                        SC_TOOL_RISK_NETWORK,
                                        SC_TOOL_CAPABILITY_BROWSER,
                                        SC_TOOL_SIDE_EFFECT_PROCESS,
                                        sc_tool_browser_screenshot_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("pdf_extract"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_FILESYSTEM,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_pdf_extract_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("time"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_time_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("memory_store"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        SC_TOOL_CAPABILITY_MEMORY,
                                        SC_TOOL_SIDE_EFFECT_WRITE,
                                        sc_tool_memory_store_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("memory_recall"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_MEMORY,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_memory_recall_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("memory_search"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_MEMORY,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_memory_search_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("memory_pin"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        SC_TOOL_CAPABILITY_MEMORY,
                                        SC_TOOL_SIDE_EFFECT_WRITE,
                                        sc_tool_memory_pin_new);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("memory_forget"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        SC_TOOL_CAPABILITY_MEMORY,
                                        SC_TOOL_SIDE_EFFECT_WRITE,
                                        sc_tool_memory_forget_new);
    }
    if (sc_status_is_ok(status) &&
        (sc_config_get_bool(config, sc_str_from_cstr("sop.enabled"), false) ||
         config_prop_nonempty(config, alloc, sc_str_from_cstr("sop.path")))) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("sop_inspect"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_sop_inspect_new);
        if (sc_status_is_ok(status)) {
            status = append_tool_if_exposed(alloc,
                                            config,
                                            &context,
                                            owned,
                                            sc_str_from_cstr("sop_advance"),
                                            SC_TOOL_RISK_READONLY,
                                            SC_TOOL_CAPABILITY_NONE,
                                            SC_TOOL_SIDE_EFFECT_READ,
                                            sc_tool_sop_advance_new);
        }
    }
    if (sc_status_is_ok(status) &&
        owned->cron_initialized &&
        (sc_config_get_bool(config, sc_str_from_cstr("cron.enabled"), false) ||
         sc_config_get_bool(config, sc_str_from_cstr("cron.tools_enabled"), false))) {
        status = append_tool_if_exposed(alloc,
                                        config,
                                        &context,
                                        owned,
                                        sc_str_from_cstr("cron_list"),
                                        SC_TOOL_RISK_READONLY,
                                        SC_TOOL_CAPABILITY_NONE,
                                        SC_TOOL_SIDE_EFFECT_READ,
                                        sc_tool_cron_list_new);
        if (sc_status_is_ok(status)) {
            status = append_tool_if_exposed(alloc,
                                            config,
                                            &context,
                                            owned,
                                            sc_str_from_cstr("cron_upsert"),
                                            SC_TOOL_RISK_SIDE_EFFECT,
                                            SC_TOOL_CAPABILITY_NONE,
                                            SC_TOOL_SIDE_EFFECT_WRITE,
                                            sc_tool_cron_upsert_new);
        }
        if (sc_status_is_ok(status)) {
            status = append_tool_if_exposed(alloc,
                                            config,
                                            &context,
                                            owned,
                                            sc_str_from_cstr("cron_remove"),
                                            SC_TOOL_RISK_SIDE_EFFECT,
                                            SC_TOOL_CAPABILITY_NONE,
                                            SC_TOOL_SIDE_EFFECT_WRITE,
                                            sc_tool_cron_remove_new);
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && config != nullptr && i < sc_config_mcp_server_count(config); ++i) {
        sc_mcp_server_view server = {0};
        sc_tool **discovered = nullptr;
        size_t discovered_count = 0;
        if (!sc_config_mcp_server_at(config, i, &server) || !server.enabled) {
            continue;
        }
        if (owned->tool_count >= SC_ARRAY_LEN(owned->tools)) {
            status = sc_status_unsupported("sc.bootstrap.too_many_tools");
            break;
        }
        if (tool_filter_groups_exclude(config,
                                       alloc,
                                       server.name,
                                       server.transport == SC_MCP_TRANSPORT_STDIO ? SC_TOOL_RISK_SIDE_EFFECT : SC_TOOL_RISK_NETWORK,
                                       SC_TOOL_CAPABILITY_MCP,
                                       server.transport == SC_MCP_TRANSPORT_STDIO ? SC_TOOL_SIDE_EFFECT_PROCESS :
                                                                                   SC_TOOL_SIDE_EFFECT_NETWORK)) {
            continue;
        }
        if (!server.deferred_loading) {
            status = sc_tool_mcp_server_discover(alloc,
                                                 &context,
                                                 server.name,
                                                 sc_mcp_transport_to_str(server.transport),
                                                 server.command,
                                                 server.args,
                                                 server.url,
                                                 server.headers,
                                                 &discovered,
                                                 &discovered_count);
        } else {
            status = sc_status_unsupported("sc.bootstrap.mcp_deferred_descriptor");
        }
        if (sc_status_is_ok(status)) {
            if (owned->tool_count + discovered_count > SC_ARRAY_LEN(owned->tools)) {
                status = sc_status_unsupported("sc.bootstrap.too_many_tools");
            } else {
                for (size_t j = 0; j < discovered_count; j += 1) {
                    owned->tools[owned->tool_count++] = discovered[j];
                    discovered[j] = nullptr;
                }
            }
            for (size_t j = 0; j < discovered_count; j += 1) {
                sc_tool_destroy(discovered[j]);
            }
            sc_free(alloc, discovered, discovered_count * sizeof(*discovered), _Alignof(sc_tool *));
        } else {
            sc_status_clear(&status);
            status = sc_tool_mcp_server_new(alloc,
                                            &context,
                                            server.name,
                                            sc_mcp_transport_to_str(server.transport),
                                            server.command,
                                            server.args,
                                            server.url,
                                            server.headers,
                                            &owned->tools[owned->tool_count]);
            if (sc_status_is_ok(status)) {
                owned->tool_count += 1;
            }
        }
    }
    return status;
}

static sc_status append_tool_if_exposed(sc_allocator *alloc,
                                        const sc_config *config,
                                        const sc_tool_context *context,
                                        boot_owned *owned,
                                        sc_str name,
                                        sc_tool_risk risk,
                                        uint64_t capability_category,
                                        sc_tool_side_effect side_effect,
                                        tool_constructor_fn constructor)
{
    if (owned == nullptr || constructor == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.tool_append_invalid_argument");
    }
    if (tool_filter_groups_exclude(config, alloc, name, risk, capability_category, side_effect)) {
        return sc_status_ok();
    }
    if (owned->tool_count >= SC_ARRAY_LEN(owned->tools)) {
        return sc_status_unsupported("sc.bootstrap.too_many_tools");
    }
    sc_status status = constructor(alloc, context, &owned->tools[owned->tool_count]);
    if (sc_status_is_ok(status)) {
        owned->tool_count += 1;
    }
    return status;
}

static bool tool_filter_groups_exclude(const sc_config *config,
                                       sc_allocator *alloc,
                                       sc_str name,
                                       sc_tool_risk risk,
                                       uint64_t capability_category,
                                       sc_tool_side_effect side_effect)
{
    if (config == nullptr) {
        return false;
    }
    if (string_array_prop_contains(config, alloc, sc_str_from_cstr("tool_filter_groups"), sc_str_from_cstr("read_only")) &&
        risk != SC_TOOL_RISK_READONLY) {
        return true;
    }
    if (string_array_prop_contains(config, alloc, sc_str_from_cstr("tool_filter_groups"), sc_str_from_cstr("public_safe")) &&
        (tool_name_is_public_restricted(name) || risk != SC_TOOL_RISK_READONLY ||
         side_effect == SC_TOOL_SIDE_EFFECT_WRITE || side_effect == SC_TOOL_SIDE_EFFECT_DESTRUCTIVE)) {
        return true;
    }
    if (string_array_prop_contains(config, alloc, sc_str_from_cstr("tool_filter_groups"), sc_str_from_cstr("no_network")) &&
        (capability_category & (SC_TOOL_CAPABILITY_NETWORK | SC_TOOL_CAPABILITY_BROWSER)) != 0) {
        return true;
    }
    if (string_array_prop_contains(config, alloc, sc_str_from_cstr("tool_filter_groups"), sc_str_from_cstr("no_mcp")) &&
        (capability_category & SC_TOOL_CAPABILITY_MCP) != 0) {
        return true;
    }
    return false;
}

static bool string_array_prop_contains(const sc_config *config, sc_allocator *alloc, sc_str path, sc_str value)
{
    sc_string raw = {0};
    sc_vec values = {0};
    sc_status status = sc_status_ok();
    bool found = false;

    if (config == nullptr) {
        return false;
    }
    sc_vec_init(&values, alloc == nullptr ? sc_allocator_heap() : alloc, sizeof(sc_string));
    status = get_prop_copy(config, path, alloc, &raw);
    if (sc_status_is_ok(status)) {
        status = parse_string_array(alloc, sc_string_as_str(&raw), &values);
    }
    if (sc_status_is_ok(status)) {
        for (size_t i = 0; i < values.len; i += 1) {
            const sc_string *item = sc_vec_at_const(&values, i);
            if (item != nullptr && sc_str_equal(sc_string_as_str(item), value)) {
                found = true;
                break;
            }
        }
    } else {
        sc_status_clear(&status);
    }
    clear_string_vec(&values);
    sc_string_clear(&raw);
    return found;
}

static bool config_prop_nonempty(const sc_config *config, sc_allocator *alloc, sc_str path)
{
    sc_string value = {0};
    sc_status status = sc_status_ok();
    bool nonempty = false;

    status = get_prop_copy(config, path, alloc, &value);
    if (sc_status_is_ok(status)) {
        nonempty = value.len > 0;
    } else {
        sc_status_clear(&status);
    }
    sc_string_clear(&value);
    return nonempty;
}

static bool tool_name_is_public_restricted(sc_str name)
{
    static const char *restricted[] = {
        "shell",
        "file_write",
        "browser",
        "browser_screenshot",
        "memory_store",
        "memory_pin",
        "memory_forget",
        "memory_purge",
        "cron_upsert",
        "cron_remove",
    };
    for (size_t i = 0; i < SC_ARRAY_LEN(restricted); i += 1) {
        if (sc_str_equal(name, sc_str_from_cstr(restricted[i]))) {
            return true;
        }
    }
    return false;
}

static sc_status create_channels_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned)
{
    sc_status status = sc_status_ok();
    bool telegram_enabled = sc_config_get_bool(config, sc_str_from_cstr("channels.telegram.enabled"), false);
    bool webhook_enabled = sc_config_get_bool(config, sc_str_from_cstr("channels.webhook.enabled"), false);
    bool rabbitmq_enabled = sc_config_get_bool(config, sc_str_from_cstr("channels.rabbitmq.enabled"), false);
    bool mail_enabled = sc_config_get_bool(config, sc_str_from_cstr("channels.mail.enabled"), false);
    sc_string enabled_raw = {0};
    sc_vec enabled = {0};

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.channels_invalid_argument");
    }
    sc_vec_init(&enabled, alloc, sizeof(sc_string));
    if (sc_status_is_ok(get_prop_copy(config, sc_str_from_cstr("channels.enabled"), alloc, &enabled_raw))) {
        status = parse_string_array(alloc, sc_string_as_str(&enabled_raw), &enabled);
        for (size_t i = 0; sc_status_is_ok(status) && i < enabled.len; i += 1) {
            const sc_string *name = sc_vec_at_const(&enabled, i);
            if (name != nullptr && sc_str_equal(sc_string_as_str(name), sc_str_from_cstr("telegram"))) {
                telegram_enabled = true;
            } else if (name != nullptr && sc_str_equal(sc_string_as_str(name), sc_str_from_cstr("webhook"))) {
                webhook_enabled = true;
            } else if (name != nullptr && sc_str_equal(sc_string_as_str(name), sc_str_from_cstr("rabbitmq"))) {
                rabbitmq_enabled = true;
            } else if (name != nullptr && sc_str_equal(sc_string_as_str(name), sc_str_from_cstr("mail"))) {
                mail_enabled = true;
            }
        }
    }
    if (sc_status_is_ok(status) && telegram_enabled) {
        if (owned->channel_count >= SC_ARRAY_LEN(owned->channels)) {
            status = sc_status_invalid_argument("sc.bootstrap.channels_full");
        } else {
            status = create_telegram_from_config(alloc, config, &owned->channels[owned->channel_count]);
            if (sc_status_is_ok(status)) {
                owned->channel_count += 1;
            }
        }
    }
    if (sc_status_is_ok(status) && webhook_enabled) {
        if (owned->channel_count >= SC_ARRAY_LEN(owned->channels)) {
            status = sc_status_invalid_argument("sc.bootstrap.channels_full");
        } else {
            status = create_webhook_from_config(alloc, config, &owned->channels[owned->channel_count]);
            if (sc_status_is_ok(status)) {
                owned->channel_count += 1;
            }
        }
    }
    if (sc_status_is_ok(status) && rabbitmq_enabled) {
        if (owned->channel_count >= SC_ARRAY_LEN(owned->channels)) {
            status = sc_status_invalid_argument("sc.bootstrap.channels_full");
        } else {
            status = create_rabbitmq_from_config(alloc, config, &owned->channels[owned->channel_count]);
            if (sc_status_is_ok(status)) {
                owned->channel_count += 1;
            }
        }
    }
    if (sc_status_is_ok(status) && mail_enabled) {
        if (owned->channel_count >= SC_ARRAY_LEN(owned->channels)) {
            status = sc_status_invalid_argument("sc.bootstrap.channels_full");
        } else {
            status = create_mail_from_config(alloc, config, &owned->channels[owned->channel_count]);
            if (sc_status_is_ok(status)) {
                owned->channel_count += 1;
            }
        }
    }
    clear_string_vec(&enabled);
    sc_string_clear(&enabled_raw);
    return status;
}

static sc_status create_tts_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned)
{
    sc_string backend = {0};
    sc_string base_url = {0};
    sc_string voice = {0};
    sc_string speaker = {0};
    sc_string length_scale = {0};
    sc_string noise_scale = {0};
    sc_string noise_w_scale = {0};
    sc_status status = sc_status_ok();

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.tts_invalid_argument");
    }
    if (!sc_config_get_bool(config, sc_str_from_cstr("media.tts.enabled"), false)) {
        return sc_status_ok();
    }
    status = get_prop_copy(config, sc_str_from_cstr("media.tts.backend"), alloc, &backend);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "piper", &backend);
    }
    if (sc_status_is_ok(status) && !sc_str_equal(sc_string_as_str(&backend), sc_str_from_cstr("piper"))) {
        status = sc_status_unsupported("sc.bootstrap.tts_backend_unsupported");
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.base_url"), alloc, &base_url);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "http://127.0.0.1:5000", &base_url);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.voice"), alloc, &voice);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &voice);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.speaker"), alloc, &speaker);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &speaker);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_status scale_status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.length_scale"), alloc, &length_scale);
        if (!sc_status_is_ok(scale_status)) {
            sc_status_clear(&scale_status);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_status scale_status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.noise_scale"), alloc, &noise_scale);
        if (!sc_status_is_ok(scale_status)) {
            sc_status_clear(&scale_status);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_status scale_status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.noise_w_scale"), alloc, &noise_w_scale);
        if (!sc_status_is_ok(scale_status)) {
            sc_status_clear(&scale_status);
        }
    }
    if (sc_status_is_ok(status)) {
        int64_t speaker_id = sc_config_get_int(config, sc_str_from_cstr("media.tts.piper.speaker_id"), -1);
        sc_tts_piper_options options = {
            .struct_size = sizeof(options),
            .base_url = sc_string_as_str(&base_url),
            .default_voice = sc_string_as_str(&voice),
            .speaker = sc_string_as_str(&speaker),
            .speaker_id = speaker_id,
            .speaker_id_set = speaker_id >= 0,
            .length_scale = length_scale.len == 0 ? 0.0 : strtod(length_scale.ptr, nullptr),
            .length_scale_set = length_scale.len > 0,
            .noise_scale = noise_scale.len == 0 ? 0.0 : strtod(noise_scale.ptr, nullptr),
            .noise_scale_set = noise_scale.len > 0,
            .noise_w_scale = noise_w_scale.len == 0 ? 0.0 : strtod(noise_w_scale.ptr, nullptr),
            .noise_w_scale_set = noise_w_scale.len > 0,
            .timeout_ms = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("media.tts.piper.timeout_ms"), 30000),
            .max_audio_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("media.tts.piper.max_audio_bytes"), 8388608),
        };
        status = sc_tts_piper_new(alloc, &options, &owned->tts);
    }
    sc_string_clear(&noise_w_scale);
    sc_string_clear(&noise_scale);
    sc_string_clear(&length_scale);
    sc_string_clear(&speaker);
    sc_string_clear(&voice);
    sc_string_clear(&base_url);
    sc_string_clear(&backend);
    return status;
}

static sc_status create_transcriber_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned)
{
    sc_string backend = {0};
    sc_string endpoint_url = {0};
    sc_string model = {0};
    sc_string language = {0};
    sc_string prompt = {0};
    sc_string temperature = {0};
    sc_string temperature_inc = {0};
    sc_string response_format = {0};
    sc_status status = sc_status_ok();

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.asr_invalid_argument");
    }
    if (!sc_config_get_bool(config, sc_str_from_cstr("media.asr.enabled"), false)) {
        return sc_status_ok();
    }
    status = get_prop_copy(config, sc_str_from_cstr("media.asr.backend"), alloc, &backend);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "whisper_cpp", &backend);
    }
    if (sc_status_is_ok(status) && !sc_str_equal(sc_string_as_str(&backend), sc_str_from_cstr("whisper_cpp"))) {
        status = sc_status_unsupported("sc.bootstrap.asr_backend_unsupported");
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.endpoint_url"), alloc, &endpoint_url);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "http://127.0.0.1:2022/v1/audio/transcriptions", &endpoint_url);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.model"), alloc, &model);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "whisper-1", &model);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.language"), alloc, &language);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &language);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.prompt"), alloc, &prompt);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &prompt);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.temperature"), alloc, &temperature);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &temperature);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.temperature_inc"), alloc, &temperature_inc);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &temperature_inc);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("media.asr.whisper.response_format"), alloc, &response_format);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "json", &response_format);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_transcriber_whisper_options options = {
            .struct_size = sizeof(options),
            .endpoint_url = sc_string_as_str(&endpoint_url),
            .model = sc_string_as_str(&model),
            .default_language = sc_string_as_str(&language),
            .prompt = sc_string_as_str(&prompt),
            .temperature = sc_string_as_str(&temperature),
            .temperature_inc = sc_string_as_str(&temperature_inc),
            .response_format = sc_string_as_str(&response_format),
            .timeout_ms = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("media.asr.whisper.timeout_ms"), 30000),
            .max_audio_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("media.asr.whisper.max_audio_bytes"), 26214400),
            .max_response_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("media.asr.whisper.max_response_bytes"), 65536),
        };
        status = sc_transcriber_whisper_new(alloc, &options, &owned->transcriber);
    }
    sc_string_clear(&response_format);
    sc_string_clear(&temperature_inc);
    sc_string_clear(&temperature);
    sc_string_clear(&prompt);
    sc_string_clear(&language);
    sc_string_clear(&model);
    sc_string_clear(&endpoint_url);
    sc_string_clear(&backend);
    return status;
}

static sc_status create_orchestrator_from_config(sc_allocator *alloc,
                                                 const sc_config *config,
                                                 sc_str workspace,
                                                 boot_owned *owned,
                                                 sc_vec *allowed,
                                                 sc_str **allowed_senders,
                                                 sc_string *allowed_raw,
                                                 sc_string *stream_mode,
                                                 sc_string *session_db,
                                                 sc_string *session_dir)
{
    sc_string db_rel = {0};
    sc_string sessions_rel = {0};
    sc_string session_backend = {0};
    sc_str session_db_path = {0};
    sc_str session_jsonl_dir = {0};
    sc_string tts_reply_mode = {0};
    sc_string tts_voice = {0};
    sc_channel_common_config common_configs[8] = {0};
    sc_vec common_allow_values[8] = {0};
    sc_vec common_deny_values[8] = {0};
    sc_str *common_allow_views[8] = {0};
    sc_str *common_deny_views[8] = {0};
    size_t common_config_count = 0;
    sc_status status = sc_status_ok();

    if (owned == nullptr || allowed == nullptr || allowed_senders == nullptr || allowed_raw == nullptr || stream_mode == nullptr || session_db == nullptr ||
        session_dir == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.orchestrator_invalid_argument");
    }
    if (owned->channel_count == 0) {
        return sc_status_ok();
    }
    status = get_prop_copy(config, sc_str_from_cstr("channels.telegram.allowed_users"), alloc, allowed_raw);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "[]", allowed_raw);
    }
    if (sc_status_is_ok(status)) {
        status = parse_string_array(alloc, sc_string_as_str(allowed_raw), allowed);
    }
    if (sc_status_is_ok(status) && allowed->len > 0) {
        *allowed_senders = sc_alloc(alloc, allowed->len * sizeof(**allowed_senders), _Alignof(sc_str));
        if (*allowed_senders == nullptr) {
            status = sc_status_no_memory();
        } else {
            for (size_t i = 0; i < allowed->len; i += 1) {
                const sc_string *sender = sc_vec_at_const(allowed, i);
                (*allowed_senders)[i] = sc_string_as_str(sender);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "sessions/sessions.db", &db_rel);
    }
    if (sc_status_is_ok(status)) {
        status = join_path(alloc, workspace, sc_string_as_str(&db_rel), session_db);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "sessions", &sessions_rel);
    }
    if (sc_status_is_ok(status)) {
        status = join_path(alloc, workspace, sc_string_as_str(&sessions_rel), session_dir);
    }
    if (sc_status_is_ok(status)) {
        status = ensure_dir(alloc, sc_string_as_str(session_dir));
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.session_backend"), alloc, &session_backend);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "sqlite", &session_backend);
        }
    }
    if (sc_status_is_ok(status)) {
        if (sc_str_equal(sc_string_as_str(&session_backend), sc_str_from_cstr("sqlite"))) {
            session_db_path = sc_string_as_str(session_db);
            session_jsonl_dir = sc_str_from_cstr("");
        } else if (sc_str_equal(sc_string_as_str(&session_backend), sc_str_from_cstr("jsonl"))) {
            session_db_path = sc_str_from_cstr("");
            session_jsonl_dir = sc_string_as_str(session_dir);
        } else {
            status = sc_status_unsupported("sc.bootstrap.session_backend_unsupported");
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.telegram.stream_mode"), alloc, stream_mode);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "off", stream_mode);
        }
    }
    if (sc_status_is_ok(status)) {
        status = build_channel_common_configs(alloc,
                                              config,
                                              owned,
                                              common_configs,
                                              common_allow_values,
                                              common_deny_values,
                                              common_allow_views,
                                              common_deny_views,
                                              SC_ARRAY_LEN(common_configs),
                                              &common_config_count);
    }
    if (sc_status_is_ok(status) && owned->tts != nullptr) {
        status = get_prop_copy(config, sc_str_from_cstr("media.tts.reply_mode"), alloc, &tts_reply_mode);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "text_and_audio", &tts_reply_mode);
        }
    }
    if (sc_status_is_ok(status) && owned->tts != nullptr) {
        status = get_prop_copy(config, sc_str_from_cstr("media.tts.piper.voice"), alloc, &tts_voice);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &tts_voice);
        }
    }
    if (sc_status_is_ok(status)) {
        int64_t seen_limit = sc_config_get_int(config, sc_str_from_cstr("channels.max_seen_message_ids"), 1024);
        sc_channel_tts_reply_mode reply_mode =
            owned->tts != nullptr && sc_str_equal(sc_string_as_str(&tts_reply_mode), sc_str_from_cstr("text_and_audio"))
                ? SC_CHANNEL_TTS_REPLY_TEXT_AND_AUDIO
                : SC_CHANNEL_TTS_REPLY_OFF;
        sc_channel_orchestrator_options orchestrator_options = {
            .struct_size = sizeof(sc_channel_orchestrator_options),
            .agent = owned->agent,
            .channels = owned->channels,
            .channel_count = owned->channel_count,
            .allowed_senders = *allowed_senders,
            .allowed_sender_count = allowed->len,
            .max_history_messages = (size_t)sc_config_get_int(config, sc_str_from_cstr("agent.max_history_messages"), 50),
            .session_persistence = sc_config_get_bool(config, sc_str_from_cstr("channels.session_persistence"), true),
            .session_db_path = session_db_path,
            .session_jsonl_dir = session_jsonl_dir,
            .ack_reactions = sc_config_get_bool(config, sc_str_from_cstr("channels.ack_reactions"), false),
            .show_tool_calls = sc_config_get_bool(config, sc_str_from_cstr("channels.show_tool_calls"), false),
            .interrupt_on_new_message = sc_config_get_bool(config, sc_str_from_cstr("channels.telegram.interrupt_on_new_message"), false),
            .stream_mode = sc_string_as_str(stream_mode),
            .approval_timeout_secs = (uint64_t)sc_config_get_int(config, sc_str_from_cstr("channels.telegram.approval_timeout_secs"), 120),
            .max_seen_message_ids = seen_limit <= 0 ? 1'024 : (size_t)seen_limit,
            .common_configs = common_configs,
            .common_config_count = common_config_count,
            .transcriber = owned->transcriber,
            .tts = owned->tts,
            .tts_reply_mode = reply_mode,
            .tts_voice = sc_string_as_str(&tts_voice),
        };
        status = sc_channel_orchestrator_new(alloc, &orchestrator_options, &owned->orchestrator);
    }
    clear_channel_common_storage(alloc,
                                 common_allow_values,
                                 common_deny_values,
                                 common_allow_views,
                                 common_deny_views,
                                 SC_ARRAY_LEN(common_configs));
    sc_string_clear(&tts_voice);
    sc_string_clear(&tts_reply_mode);
    sc_string_clear(&session_backend);
    sc_string_clear(&sessions_rel);
    sc_string_clear(&db_rel);
    return status;
}

static sc_status create_gateway_from_config(sc_allocator *alloc,
                                            const sc_config *config,
                                            const sc_boot_options *options,
                                            boot_owned *owned)
{
    sc_string bind = {0};
    sc_string pairing = {0};
    sc_string auth = {0};
    sc_status status = sc_status_ok();
    bool gateway_enabled = false;
    bool listener_enabled = false;

    if (owned == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.gateway_invalid_argument");
    }
    gateway_enabled =
        (options->struct_size >= offsetof(sc_boot_options, gateway_enabled) + sizeof(options->gateway_enabled) &&
         options->gateway_enabled) ||
        sc_config_get_bool(config, sc_str_from_cstr("gateway.enabled"), false);
    listener_enabled =
        (options->struct_size >= offsetof(sc_boot_options, gateway_listener_enabled) +
                sizeof(options->gateway_listener_enabled) &&
         options->gateway_listener_enabled) ||
        sc_config_get_bool(config, sc_str_from_cstr("gateway.listener_enabled"), false);
    if (!gateway_enabled) {
        return sc_status_ok();
    }
    status = get_prop_copy(config, sc_str_from_cstr("gateway.bind"), alloc, &bind);
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("gateway.pairing_code"), alloc, &pairing);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &pairing);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("gateway.auth_token"), alloc, &auth);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &auth);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_str gateway_bind =
            options->struct_size >= offsetof(sc_boot_options, gateway_bind) + sizeof(options->gateway_bind) &&
                options->gateway_bind.len > 0
            ? options->gateway_bind
            : sc_string_as_str(&bind);
        sc_gateway_options gateway_options = {
            .struct_size = sizeof(gateway_options),
            .agent = owned->agent,
            .config = (sc_config *)config,
            .bind = gateway_bind,
            .port = (uint16_t)sc_config_get_int(config, sc_str_from_cstr("gateway.port"), 8080),
            .public_bind_enabled = sc_config_get_bool(config, sc_str_from_cstr("gateway.public_bind_enabled"), false),
            .pairing_code = sc_string_as_str(&pairing),
            .auth_token = sc_string_as_str(&auth),
            .max_body_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("gateway.max_body_bytes"), 65536),
            .rate_limit = (size_t)sc_config_get_int(config, sc_str_from_cstr("gateway.rate_limit"), 60),
            .timeout_ms = sc_config_get_int(config, sc_str_from_cstr("gateway.timeout_ms"), 30000),
        };
        status = sc_gateway_server_new(alloc, &gateway_options, &owned->gateway);
        if (sc_status_is_ok(status) && listener_enabled) {
            sc_gateway_transport_options transport_options = {
                .struct_size = sizeof(transport_options),
                .kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
                .listener_enabled = true,
                .listen_port = gateway_options.port,
                .websocket_enabled = true,
                .jsonrpc_enabled = true,
            };
            status = sc_gateway_transport_configure(owned->gateway, &transport_options);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_gateway_server_start(owned->gateway);
    }
    if (sc_status_is_ok(status) && sc_config_get_bool(config, sc_str_from_cstr("gateway.observe"), true)) {
        status = sc_gateway_observer_new(alloc, owned->gateway, &owned->gateway_observer);
    }
    sc_string_secure_clear(&auth);
    sc_string_secure_clear(&pairing);
    sc_string_clear(&bind);
    return status;
}

static sc_status create_delivery_from_config(sc_allocator *alloc, const sc_config *config, boot_owned *owned)
{
    sc_string delivery = {0};
    sc_status status = sc_status_ok();

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.delivery_invalid_argument");
    }
    status = get_prop_copy(config, sc_str_from_cstr("runtime.delivery"), alloc, &delivery);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, owned->orchestrator == nullptr ? "stdout" : "channel", &delivery);
    }
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&delivery), sc_str_from_cstr("channel")) && owned->orchestrator != nullptr) {
        status = sc_channel_orchestrator_delivery_new(alloc, owned->orchestrator, &owned->delivery);
    } else if (sc_status_is_ok(status)) {
        status = sc_delivery_stdout_new(alloc, &owned->delivery);
    }
    sc_string_clear(&delivery);
    return status;
}

static sc_status create_cron_from_config(sc_allocator *alloc, const sc_config *config, sc_str workspace, boot_owned *owned)
{
    sc_string id = {0};
    sc_string schedule_text = {0};
    sc_string prompt = {0};
    sc_string target = {0};
    sc_string state_path = {0};
    sc_cron_job job = {.struct_size = sizeof(job), .kind = SC_CRON_JOB_AGENT, .enabled = true};
    sc_status status = sc_status_ok();

    if (owned == nullptr || workspace.ptr == nullptr || workspace.len == 0) {
        return sc_status_invalid_argument("sc.bootstrap.cron_invalid_argument");
    }
    sc_cron_job_store_init(&owned->cron_jobs, alloc);
    sc_cron_run_store_init(&owned->cron_runs, alloc);
    owned->cron_initialized = true;
    status = join_path(alloc, workspace, sc_str_from_cstr("state/cron_jobs.state"), &state_path);
    if (sc_status_is_ok(status)) {
        status = sc_cron_job_store_load_file(&owned->cron_jobs, sc_string_as_str(&state_path));
    }
    if (!sc_status_is_ok(status) || !sc_config_get_bool(config, sc_str_from_cstr("cron.enabled"), false)) {
        sc_string_clear(&state_path);
        return status;
    }
    status = get_prop_copy(config, sc_str_from_cstr("cron.id"), alloc, &id);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "config-cron", &id);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("cron.schedule"), alloc, &schedule_text);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("cron.prompt"), alloc, &prompt);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("cron.delivery_target"), alloc, &target);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "default", &target);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_schedule_parse(sc_string_as_str(&schedule_text), &job.schedule);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&id), &job.id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&prompt), &job.command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&target), &job.delivery_target);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&schedule_text), &job.schedule_text);
    }
    if (sc_status_is_ok(status)) {
        job.once = sc_config_get_bool(config, sc_str_from_cstr("cron.once"), false);
        status = sc_cron_job_store_put(&owned->cron_jobs, &job);
    }
    sc_cron_job_clear(&job);
    sc_string_clear(&target);
    sc_string_clear(&prompt);
    sc_string_clear(&schedule_text);
    sc_string_clear(&id);
    sc_string_clear(&state_path);
    return status;
}

static sc_status create_heartbeat_from_config(sc_allocator *alloc, const sc_config *config, sc_str workspace, boot_owned *owned)
{
    sc_string rel = {0};
    sc_status status = sc_status_ok();

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.heartbeat_invalid_argument");
    }
    if (!sc_config_get_bool(config, sc_str_from_cstr("heartbeat.enabled"), false)) {
        return sc_status_ok();
    }
    sc_heartbeat_state_init(&owned->heartbeat, alloc);
    owned->heartbeat_initialized = true;
    status = get_prop_copy(config, sc_str_from_cstr("heartbeat.state_path"), alloc, &owned->heartbeat_state_path);
    if (sc_status_is_ok(status) && owned->heartbeat_state_path.len > 0 && owned->heartbeat_state_path.ptr[0] != '/') {
        rel = owned->heartbeat_state_path;
        owned->heartbeat_state_path = (sc_string){0};
        status = join_path(alloc, workspace, sc_string_as_str(&rel), &owned->heartbeat_state_path);
    } else if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "heartbeat.state", &rel);
        if (sc_status_is_ok(status)) {
            status = join_path(alloc, workspace, sc_string_as_str(&rel), &owned->heartbeat_state_path);
        }
    }
    sc_string_clear(&rel);
    return status;
}

static sc_status create_telegram_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out)
{
    sc_string token = {0};
    sc_string token_env = {0};
    sc_string stream = {0};
    sc_status status = get_prop_copy(config, sc_str_from_cstr("channels.telegram.bot_token_env"), alloc, &token_env);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "SMOLCLAW_TELEGRAM_BOT_TOKEN", &token_env);
    }
    if (sc_status_is_ok(status) && token_env.len > 0) {
        const char *env_value = getenv(token_env.ptr);
        if (env_value != nullptr && env_value[0] != '\0') {
            status = sc_string_from_cstr(alloc, env_value, &token);
        }
    }
    if (sc_status_is_ok(status) && token.len == 0) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.telegram.bot_token"), alloc, &token);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &token);
        }
    }
    if (sc_status_is_ok(status) && token.len == 0) {
        bool secret_found = false;
        sc_string stored_token = {0};

        status = try_secret_store_prop(config,
                                       alloc,
                                       sc_str_from_cstr("channels.telegram.bot_token"),
                                       &stored_token,
                                       &secret_found);
        if (sc_status_is_ok(status) && secret_found) {
            sc_string_secure_clear(&token);
            token = stored_token;
            stored_token = (sc_string){0};
        }
        sc_string_secure_clear(&stored_token);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.telegram.stream_mode"), alloc, &stream);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "off", &stream);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_telegram_channel_options telegram_options = {
            .struct_size = sizeof(sc_telegram_channel_options),
            .bot_token = sc_string_as_str(&token),
            .stream_mode = sc_string_as_str(&stream),
            .poll_timeout_seconds = (long)sc_config_get_int(config, sc_str_from_cstr("channels.telegram.poll_timeout_seconds"), 30),
            .approval_timeout_seconds = (long)sc_config_get_int(config, sc_str_from_cstr("channels.telegram.approval_timeout_secs"), 120),
            .draft_update_interval_ms = (long)sc_config_get_int(config, sc_str_from_cstr("channels.telegram.draft_update_interval_ms"), 1000),
            .max_retries = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.max_retries"), 2),
            .retry_backoff_ms = (uint32_t)sc_config_get_int(config, sc_str_from_cstr("reliability.retry_backoff_ms"), 250),
            .mention_only = sc_config_get_bool(config, sc_str_from_cstr("channels.telegram.mention_only"), false),
            .interrupt_on_new_message = sc_config_get_bool(config, sc_str_from_cstr("channels.telegram.interrupt_on_new_message"), false),
            .ack_reactions = sc_config_get_bool(config, sc_str_from_cstr("channels.ack_reactions"), false),
            .post_reactions = sc_config_get_bool(config, sc_str_from_cstr("channels.telegram.post_reactions"), false),
            .message_split_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("channels.telegram.message_split_bytes"), 3900),
        };
        status = sc_channel_telegram_new(alloc, &telegram_options, out);
    }
    sc_string_clear(&stream);
    sc_string_clear(&token_env);
    sc_string_secure_clear(&token);
    return status;
}

static sc_status create_webhook_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out)
{
    sc_string bind = {0};
    sc_string path = {0};
    sc_string auth = {0};
    sc_status status = get_prop_copy(config, sc_str_from_cstr("channels.webhook.bind"), alloc, &bind);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        status = sc_string_from_cstr(alloc, "127.0.0.1", &bind);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.webhook.path"), alloc, &path);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "/webhook/smolclaw", &path);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.webhook.auth_token"), alloc, &auth);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &auth);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_webhook_channel_options options = {
            .struct_size = sizeof(options),
            .bind = sc_string_as_str(&bind),
            .port = (uint16_t)sc_config_get_int(config, sc_str_from_cstr("channels.webhook.port"), 8081),
            .path = sc_string_as_str(&path),
            .auth_token = sc_string_as_str(&auth),
            .max_body_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("channels.webhook.max_body_bytes"), 65536),
        };
        status = sc_channel_webhook_new(alloc, &options, out);
    }
    sc_string_secure_clear(&auth);
    sc_string_clear(&path);
    sc_string_clear(&bind);
    return status;
}

static sc_status create_rabbitmq_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out)
{
    sc_string url = {0};
    sc_string exchange = {0};
    sc_string routing_key = {0};
    sc_string queue = {0};
    sc_string consumer_tag = {0};
    sc_status status = get_prop_copy(config, sc_str_from_cstr("channels.rabbitmq.url"), alloc, &url);
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.rabbitmq.exchange"), alloc, &exchange);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &exchange);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.rabbitmq.routing_key"), alloc, &routing_key);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "smolclaw.inbound", &routing_key);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.rabbitmq.queue"), alloc, &queue);
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.rabbitmq.consumer_tag"), alloc, &consumer_tag);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "smolclaw-c", &consumer_tag);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_rabbitmq_channel_options options = {
            .struct_size = sizeof(options),
            .url = sc_string_as_str(&url),
            .exchange = sc_string_as_str(&exchange),
            .routing_key = sc_string_as_str(&routing_key),
            .queue = sc_string_as_str(&queue),
            .consumer_tag = sc_string_as_str(&consumer_tag),
            .prefetch = (uint16_t)sc_config_get_int(config, sc_str_from_cstr("channels.rabbitmq.prefetch"), 1),
            .durable = sc_config_get_bool(config, sc_str_from_cstr("channels.rabbitmq.durable"), true),
        };
        status = sc_channel_rabbitmq_vendor_new(alloc, &options, out);
    }
    sc_string_clear(&consumer_tag);
    sc_string_clear(&queue);
    sc_string_clear(&routing_key);
    sc_string_clear(&exchange);
    sc_string_secure_clear(&url);
    return status;
}

static sc_status create_mail_from_config(sc_allocator *alloc, const sc_config *config, sc_channel **out)
{
    sc_string inbox_url = {0};
    sc_string smtp_url = {0};
    sc_string username = {0};
    sc_string password = {0};
    sc_string from = {0};
    sc_string to = {0};
    sc_status status = get_prop_copy(config, sc_str_from_cstr("channels.mail.inbox_url"), alloc, &inbox_url);
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.mail.smtp_url"), alloc, &smtp_url);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &smtp_url);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.mail.username"), alloc, &username);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &username);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.mail.password"), alloc, &password);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &password);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.mail.from"), alloc, &from);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &from);
        }
    }
    if (sc_status_is_ok(status)) {
        status = get_prop_copy(config, sc_str_from_cstr("channels.mail.to"), alloc, &to);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &to);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_mail_channel_options options = {
            .struct_size = sizeof(options),
            .inbox_url = sc_string_as_str(&inbox_url),
            .smtp_url = sc_string_as_str(&smtp_url),
            .username = sc_string_as_str(&username),
            .password = sc_string_as_str(&password),
            .from = sc_string_as_str(&from),
            .to = sc_string_as_str(&to),
            .max_message_bytes = (size_t)sc_config_get_int(config, sc_str_from_cstr("channels.mail.max_message_bytes"), 1048576),
            .delete_after_read = sc_config_get_bool(config, sc_str_from_cstr("channels.mail.delete_after_read"), false),
        };
        status = sc_channel_mail_new(alloc, &options, out);
    }
    sc_string_clear(&to);
    sc_string_clear(&from);
    sc_string_secure_clear(&password);
    sc_string_clear(&username);
    sc_string_clear(&smtp_url);
    sc_string_secure_clear(&inbox_url);
    return status;
}

static sc_status parse_string_array(sc_allocator *alloc, sc_str input, sc_vec *out)
{
    size_t i = 0;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.array_invalid_argument");
    }
    while (i < input.len) {
        while (i < input.len && input.ptr[i] != '"') {
            i += 1;
        }
        if (i >= input.len) {
            break;
        }
        i += 1;
        size_t start = i;
        while (i < input.len && input.ptr[i] != '"') {
            i += input.ptr[i] == '\\' && i + 1 < input.len ? 2 : 1;
        }
        if (i <= input.len) {
            sc_string item = {0};
            sc_status status = sc_string_from_str(alloc, sc_str_from_parts(input.ptr + start, i - start), &item);
            if (sc_status_is_ok(status)) {
                status = sc_vec_push(out, &item);
            }
            if (!sc_status_is_ok(status)) {
                sc_string_clear(&item);
                return status;
            }
        }
        i += 1;
    }
    return sc_status_ok();
}

static void clear_string_vec(sc_vec *vec)
{
    if (vec == nullptr) {
        return;
    }
    for (size_t i = 0; i < vec->len; i += 1) {
        sc_string *item = sc_vec_at(vec, i);
        sc_string_clear(item);
    }
    sc_vec_clear(vec);
}

static sc_status build_channel_common_configs(sc_allocator *alloc,
                                              const sc_config *config,
                                              const boot_owned *owned,
                                              sc_channel_common_config *common_configs,
                                              sc_vec *allow_values,
                                              sc_vec *deny_values,
                                              sc_str **allow_views,
                                              sc_str **deny_views,
                                              size_t capacity,
                                              size_t *out_count)
{
    sc_status status = sc_status_ok();

    if (owned == nullptr || common_configs == nullptr || allow_values == nullptr || deny_values == nullptr ||
        allow_views == nullptr || deny_views == nullptr || out_count == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.channel_common_invalid_argument");
    }
    *out_count = 0;
    for (size_t i = 0; sc_status_is_ok(status) && i < owned->channel_count; i += 1) {
        const sc_channel_vtab *vtab = owned->channels[i] == nullptr ? nullptr : sc_channel_vtab_of(owned->channels[i]);
        bool configured = false;

        if (vtab == nullptr || vtab->name == nullptr) {
            continue;
        }
        if (*out_count >= capacity) {
            status = sc_status_invalid_argument("sc.bootstrap.channel_common_full");
            break;
        }
        status = build_channel_common_config(alloc,
                                             config,
                                             sc_str_from_cstr(vtab->name),
                                             &common_configs[*out_count],
                                             &allow_values[*out_count],
                                             &deny_values[*out_count],
                                             &allow_views[*out_count],
                                             &deny_views[*out_count],
                                             &configured);
        if (sc_status_is_ok(status) && configured) {
            *out_count += 1;
        } else if (sc_status_is_ok(status)) {
            clear_string_vec(&allow_values[*out_count]);
            clear_string_vec(&deny_values[*out_count]);
            sc_free(alloc, allow_views[*out_count], 0, _Alignof(sc_str));
            sc_free(alloc, deny_views[*out_count], 0, _Alignof(sc_str));
            allow_views[*out_count] = nullptr;
            deny_views[*out_count] = nullptr;
        }
    }
    return status;
}

static sc_status build_channel_common_config(sc_allocator *alloc,
                                             const sc_config *config,
                                             sc_str channel_name,
                                             sc_channel_common_config *out,
                                             sc_vec *allow_values,
                                             sc_vec *deny_values,
                                             sc_str **allow_views,
                                             sc_str **deny_views,
                                             bool *configured)
{
    sc_string allow_path = {0};
    sc_string deny_path = {0};
    sc_string autonomy_path = {0};
    sc_string autonomy = {0};
    bool autonomy_set = false;
    sc_autonomy_level autonomy_level = SC_AUTONOMY_SUPERVISED;
    sc_status status = sc_status_ok();

    if (out == nullptr || allow_values == nullptr || deny_values == nullptr || allow_views == nullptr ||
        deny_views == nullptr || configured == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.channel_common_invalid_argument");
    }
    *configured = false;
    sc_vec_init(allow_values, alloc, sizeof(sc_string));
    sc_vec_init(deny_values, alloc, sizeof(sc_string));

    status = build_channel_config_path(alloc, channel_name, "tools_allow", &allow_path);
    if (sc_status_is_ok(status)) {
        status = build_channel_config_path(alloc, channel_name, "tools_deny", &deny_path);
    }
    if (sc_status_is_ok(status)) {
        status = build_channel_config_path(alloc, channel_name, "autonomy_level", &autonomy_path);
    }
    if (sc_status_is_ok(status)) {
        status = parse_optional_string_array_prop(alloc, config, sc_string_as_str(&allow_path), allow_values);
    }
    if (sc_status_is_ok(status)) {
        status = parse_optional_string_array_prop(alloc, config, sc_string_as_str(&deny_path), deny_values);
    }
    if (sc_status_is_ok(status) && allow_values->len > 0 && deny_values->len > 0) {
        status = sc_status_invalid_argument("sc.bootstrap.channel_tools_allow_deny_conflict");
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_to_views(alloc, allow_values, allow_views);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_to_views(alloc, deny_values, deny_views);
    }
    if (sc_status_is_ok(status)) {
        sc_status autonomy_status = get_prop_copy(config, sc_string_as_str(&autonomy_path), alloc, &autonomy);
        if (!sc_status_is_ok(autonomy_status)) {
            sc_status_clear(&autonomy_status);
        } else if (autonomy.len > 0) {
            sc_autonomy_level parsed = SC_AUTONOMY_SUPERVISED;
            if (!parse_autonomy_level(sc_string_as_str(&autonomy), &parsed)) {
                status = sc_status_invalid_argument("sc.bootstrap.channel_autonomy_invalid");
            } else {
                autonomy_set = true;
                autonomy_level = parsed;
            }
        }
    }
    if (sc_status_is_ok(status)) {
        *out = (sc_channel_common_config){
            .struct_size = sizeof(*out),
            .channel_name = channel_name,
            .autonomy_level_set = autonomy_set,
            .autonomy_level = autonomy_level,
            .tools_allow = *allow_views,
            .tools_allow_count = allow_values->len,
            .tools_deny = *deny_views,
            .tools_deny_count = deny_values->len,
        };
        *configured = out->autonomy_level_set || out->tools_allow_count > 0 || out->tools_deny_count > 0;
    }
    sc_string_clear(&autonomy);
    sc_string_clear(&autonomy_path);
    sc_string_clear(&deny_path);
    sc_string_clear(&allow_path);
    return status;
}

static sc_status parse_optional_string_array_prop(sc_allocator *alloc,
                                                  const sc_config *config,
                                                  sc_str path,
                                                  sc_vec *out)
{
    sc_string raw = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.array_invalid_argument");
    }
    status = get_prop_copy(config, path, alloc, &raw);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    status = parse_string_array(alloc, sc_string_as_str(&raw), out);
    sc_string_clear(&raw);
    return status;
}

static sc_status string_vec_to_views(sc_allocator *alloc, const sc_vec *values, sc_str **out)
{
    sc_str *views = nullptr;

    if (values == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.array_views_invalid_argument");
    }
    *out = nullptr;
    if (values->len == 0) {
        return sc_status_ok();
    }
    views = sc_alloc(alloc, values->len * sizeof(*views), _Alignof(sc_str));
    if (views == nullptr) {
        return sc_status_no_memory();
    }
    for (size_t i = 0; i < values->len; i += 1) {
        const sc_string *value = sc_vec_at_const(values, i);
        views[i] = value == nullptr ? sc_str_from_cstr("") : sc_string_as_str(value);
    }
    *out = views;
    return sc_status_ok();
}

static sc_status build_channel_config_path(sc_allocator *alloc, sc_str channel_name, const char *field, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || field == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.channel_path_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "channels.");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, channel_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, field);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool parse_autonomy_level(sc_str value, sc_autonomy_level *out)
{
    if (out == nullptr) {
        return false;
    }
    if (sc_str_equal(value, sc_str_from_cstr("read_only")) || sc_str_equal(value, sc_str_from_cstr("off"))) {
        *out = SC_AUTONOMY_READ_ONLY;
        return true;
    }
    if (sc_str_equal(value, sc_str_from_cstr("supervised"))) {
        *out = SC_AUTONOMY_SUPERVISED;
        return true;
    }
    if (sc_str_equal(value, sc_str_from_cstr("full")) || sc_str_equal(value, sc_str_from_cstr("autonomous"))) {
        *out = SC_AUTONOMY_FULL;
        return true;
    }
    return false;
}

static void clear_channel_common_storage(sc_allocator *alloc,
                                         sc_vec *allow_values,
                                         sc_vec *deny_values,
                                         sc_str **allow_views,
                                         sc_str **deny_views,
                                         size_t capacity)
{
    if (allow_values == nullptr || deny_values == nullptr || allow_views == nullptr || deny_views == nullptr) {
        return;
    }
    for (size_t i = 0; i < capacity; i += 1) {
        size_t allow_count = allow_values[i].len;
        size_t deny_count = deny_values[i].len;
        clear_string_vec(&allow_values[i]);
        clear_string_vec(&deny_values[i]);
        sc_free(alloc, allow_views[i], allow_count * sizeof(*allow_views[i]), _Alignof(sc_str));
        sc_free(alloc, deny_views[i], deny_count * sizeof(*deny_views[i]), _Alignof(sc_str));
        allow_views[i] = nullptr;
        deny_views[i] = nullptr;
    }
}

static sc_status run_runtime_loop(boot_owned *owned, sc_allocator *alloc, bool once, size_t max_polls, bool hard_shutdown)
{
    sc_runtime_loop *loop = nullptr;
    boot_loop_context context = {.owned = owned, .alloc = alloc, .continue_channel_errors = !once};
    sc_status status = sc_status_ok();
    bool has_work = false;
    size_t task_count = 0;

    if (owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.loop_invalid_argument");
    }
    has_work = owned->channel_count > 0 || owned->gateway != nullptr || owned->heartbeat_initialized ||
        (owned->cron_initialized && owned->cron_jobs.jobs.len > 0);
    if (!has_work) {
        return sc_status_ok();
    }
    task_count += owned->orchestrator != nullptr && owned->channel_count > 0 ? 1U : 0U;
    task_count += owned->gateway != nullptr ? 1U : 0U;
    task_count += owned->cron_initialized && owned->cron_jobs.jobs.len > 0 ? 1U : 0U;
    task_count += owned->heartbeat_initialized ? 1U : 0U;
    sc_runtime_loop_options loop_options = {
        .struct_size = sizeof(loop_options),
        .max_iterations = once ? task_count : max_polls,
        .idle_sleep_ms = owned->channel_count == 0 ? 1000 : 0,
        .hard_shutdown = hard_shutdown,
    };
    status = sc_runtime_loop_new(alloc, &loop_options, &loop);
    if (sc_status_is_ok(status) && owned->orchestrator != nullptr && owned->channel_count > 0) {
        status = sc_runtime_loop_add_task(loop,
                                          &(sc_runtime_loop_task_options){
                                              .struct_size = sizeof(sc_runtime_loop_task_options),
                                              .name = sc_str_from_cstr("bootstrap.channels"),
                                              .run = run_channel_poll_task,
                                              .user_data = &context,
                                              .repeat = true,
                                              .run_immediately = true,
                                          });
    }
    if (sc_status_is_ok(status) && owned->gateway != nullptr) {
        status = sc_runtime_loop_add_task(loop,
                                          &(sc_runtime_loop_task_options){
                                              .struct_size = sizeof(sc_runtime_loop_task_options),
                                              .name = sc_str_from_cstr("bootstrap.gateway"),
                                              .run = run_gateway_poll_task,
                                              .user_data = &context,
                                              .interval_ms = 25,
                                              .repeat = true,
                                              .run_immediately = true,
                                          });
    }
    if (sc_status_is_ok(status) && owned->cron_initialized && owned->cron_jobs.jobs.len > 0) {
        status = sc_runtime_loop_add_task(loop,
                                          &(sc_runtime_loop_task_options){
                                              .struct_size = sizeof(sc_runtime_loop_task_options),
                                              .name = sc_str_from_cstr("bootstrap.cron"),
                                              .run = run_cron_task,
                                              .user_data = &context,
                                              .interval_ms = 1000,
                                              .repeat = true,
                                              .run_immediately = true,
                                          });
    }
    if (sc_status_is_ok(status) && owned->heartbeat_initialized) {
        status = sc_runtime_loop_add_task(loop,
                                          &(sc_runtime_loop_task_options){
                                              .struct_size = sizeof(sc_runtime_loop_task_options),
                                              .name = sc_str_from_cstr("bootstrap.heartbeat"),
                                              .run = run_heartbeat_task,
                                              .user_data = &context,
                                              .interval_ms = 1000,
                                              .repeat = true,
                                              .run_immediately = true,
                                          });
    }
    if (sc_status_is_ok(status)) {
        status = sc_runtime_loop_run(loop);
    }
    if (sc_status_is_ok(status)) {
        status = sc_runtime_loop_shutdown(loop,
                                          &(sc_runtime_shutdown_options){
                                              .struct_size = sizeof(sc_runtime_shutdown_options),
                                              .close_transports = close_runtime_transports,
                                              .user_data = owned,
                                              .hard = hard_shutdown,
                                          });
    } else if (loop != nullptr) {
        sc_status shutdown_status = sc_runtime_loop_shutdown(loop,
                                                             &(sc_runtime_shutdown_options){
                                                                 .struct_size = sizeof(sc_runtime_shutdown_options),
                                                                 .close_transports = close_runtime_transports,
                                                                 .user_data = owned,
                                                                 .hard = true,
                                                             });
        sc_status_clear(&shutdown_status);
    }
    sc_runtime_loop_destroy(loop);
    return status;
}

static sc_status close_runtime_transports(void *user_data, sc_allocator *alloc)
{
    boot_owned *owned = user_data;
    (void)alloc;

    if (owned == nullptr || owned->gateway == nullptr) {
        return sc_status_ok();
    }
    return sc_gateway_server_stop(owned->gateway);
}

static sc_status run_gateway_poll_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc)
{
    boot_loop_context *context = user_data;
    (void)alloc;

    if (context == nullptr || context->owned == nullptr || context->owned->gateway == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.gateway_task_invalid_argument");
    }
    if (cancel != nullptr && cancel->cancel_requested) {
        return sc_status_cancelled("sc.bootstrap.loop_cancelled");
    }
    return sc_gateway_server_poll(context->owned->gateway, 0);
}

static sc_status run_channel_poll_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc)
{
    boot_loop_context *context = user_data;
    sc_status status = sc_status_ok();

    if (context == nullptr || context->owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.loop_task_invalid_argument");
    }
    if (cancel != nullptr && cancel->cancel_requested) {
        return sc_status_cancelled("sc.bootstrap.loop_cancelled");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < context->owned->channel_count; i += 1) {
        sc_channel_process_result result = {0};
        status = sc_channel_orchestrator_poll(context->owned->orchestrator, context->owned->channels[i], alloc, &result);
        if (status.code == SC_ERR_TIMEOUT) {
            sc_status_clear(&status);
            status = sc_status_ok();
        } else if (!sc_status_is_ok(status) && context->continue_channel_errors) {
            const sc_channel_vtab *vtab = sc_channel_vtab_of(context->owned->channels[i]);
            char channel_index[32] = {0};
            char channel_count[32] = {0};
            (void)snprintf(channel_index, sizeof(channel_index), "%zu", i);
            (void)snprintf(channel_count, sizeof(channel_count), "%zu", context->owned->channel_count);
            sc_log_field fields[] = {
                {.key = "channel_index", .value = sc_str_from_cstr(channel_index), .secret = false},
                {.key = "channel_count", .value = sc_str_from_cstr(channel_count), .secret = false},
                {.key = "channel", .value = sc_str_from_cstr(vtab == nullptr || vtab->name == nullptr ? "unknown" : vtab->name), .secret = false},
                {.key = "channel_display",
                 .value = sc_str_from_cstr(vtab == nullptr || vtab->display_name == nullptr ? "" : vtab->display_name),
                 .secret = false},
                {.key = "status_code", .value = sc_str_from_cstr(bootstrap_status_code_name(status.code)), .secret = false},
                {.key = "error_key",
                 .value = sc_str_from_cstr(status.error_key == nullptr ? "sc.bootstrap.channel_poll_failed"
                                                                        : status.error_key),
                 .secret = false},
                {.key = "detail", .value = sc_str_from_cstr(status.message == nullptr ? "" : status.message), .secret = false},
                {.key = "processed", .value = sc_str_from_cstr(bootstrap_bool_name(result.processed)), .secret = false},
                {.key = "duplicate", .value = sc_str_from_cstr(bootstrap_bool_name(result.duplicate)), .secret = false},
                {.key = "denied", .value = sc_str_from_cstr(bootstrap_bool_name(result.denied)), .secret = false},
                {.key = "cancelled_previous", .value = sc_str_from_cstr(bootstrap_bool_name(result.cancelled_previous)), .secret = false},
                {.key = "poll_error_suppressed", .value = sc_str_from_cstr("true"), .secret = false},
            };
            sc_log_write(SC_LOG_WARN, "sc.bootstrap", "bootstrap.channel_poll_failed", fields, SC_ARRAY_LEN(fields));
            sc_status_clear(&status);
            status = sc_status_ok();
        }
        sc_channel_process_result_clear(&result);
    }
    return status;
}

static const char *bootstrap_status_code_name(sc_status_code code)
{
    switch (code) {
    case SC_OK:
        return "ok";
    case SC_ERR_INVALID_ARGUMENT:
        return "invalid_argument";
    case SC_ERR_NO_MEMORY:
        return "no_memory";
    case SC_ERR_IO:
        return "io";
    case SC_ERR_PARSE:
        return "parse";
    case SC_ERR_HTTP:
        return "http";
    case SC_ERR_SECURITY_DENIED:
        return "security_denied";
    case SC_ERR_UNSUPPORTED:
        return "unsupported";
    case SC_ERR_TIMEOUT:
        return "timeout";
    case SC_ERR_CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

static const char *bootstrap_bool_name(bool value)
{
    return value ? "true" : "false";
}

static sc_status run_cron_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc)
{
    boot_loop_context *context = user_data;
    if (context == nullptr || context->owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.loop_task_invalid_argument");
    }
    if (cancel != nullptr && cancel->cancel_requested) {
        return sc_status_cancelled("sc.bootstrap.loop_cancelled");
    }
    return run_cron_due_jobs(context->owned, alloc);
}

static sc_status run_heartbeat_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc)
{
    boot_loop_context *context = user_data;
    (void)alloc;
    if (context == nullptr || context->owned == nullptr) {
        return sc_status_invalid_argument("sc.bootstrap.loop_task_invalid_argument");
    }
    if (cancel != nullptr && cancel->cancel_requested) {
        return sc_status_cancelled("sc.bootstrap.loop_cancelled");
    }
    return run_heartbeat_tick(context->owned);
}

static sc_status run_cron_due_jobs(boot_owned *owned, sc_allocator *alloc)
{
    sc_wall_time now = {0};
    sc_status status = sc_status_ok();

    if (owned == nullptr || !owned->cron_initialized || owned->cron_jobs.jobs.len == 0) {
        return sc_status_ok();
    }
    status = sc_clock_wall(&now);
    for (size_t i = 0; sc_status_is_ok(status) && i < owned->cron_jobs.jobs.len; i += 1) {
        sc_cron_job *job = sc_vec_at(&owned->cron_jobs.jobs, i);
        if (job != nullptr && sc_cron_job_is_due(job, now)) {
            status = sc_cron_job_execute(job, owned->agent, owned->delivery, &owned->cron_runs, alloc);
        }
    }
    return status;
}

static sc_status run_heartbeat_tick(boot_owned *owned)
{
    sc_string message = {0};
    sc_status status = sc_status_ok();

    if (owned == nullptr || !owned->heartbeat_initialized) {
        return sc_status_ok();
    }
    status = sc_string_from_cstr(owned->heartbeat.alloc, "healthy", &message);
    if (sc_status_is_ok(status)) {
        status = sc_heartbeat_tick(&owned->heartbeat, sc_string_as_str(&message), owned->delivery);
    }
    if (sc_status_is_ok(status) && owned->heartbeat_state_path.len > 0) {
        status = sc_heartbeat_state_write(sc_string_as_str(&owned->heartbeat_state_path), &owned->heartbeat);
    }
    sc_string_clear(&message);
    return status;
}

static bool provider_is(sc_str provider, const char *name)
{
    return sc_str_equal(provider, sc_str_from_cstr(name));
}

static void boot_owned_clear(boot_owned *owned)
{
    if (owned == nullptr) {
        return;
    }
    if (owned->delivery != nullptr) {
        sc_delivery_target_destroy(owned->delivery);
    }
    if (owned->gateway_observer != nullptr) {
        sc_observer_destroy(owned->gateway_observer);
    }
    if (owned->gateway != nullptr) {
        sc_gateway_server_destroy(owned->gateway);
    }
    if (owned->heartbeat_initialized) {
        sc_heartbeat_state_clear(&owned->heartbeat);
    }
    sc_string_clear(&owned->heartbeat_state_path);
    if (owned->cron_initialized) {
        sc_cron_run_store_clear(&owned->cron_runs);
        sc_cron_job_store_clear(&owned->cron_jobs);
    }
    if (owned->orchestrator != nullptr) {
        sc_channel_orchestrator_destroy(owned->orchestrator);
    }
    for (size_t i = 0; i < owned->channel_count; i += 1) {
        sc_channel_destroy(owned->channels[i]);
    }
    if (owned->agent != nullptr) {
        sc_agent_destroy(owned->agent);
    }
    for (size_t i = 0; i < owned->tool_count; i += 1) {
        sc_tool_destroy(owned->tools[i]);
    }
    if (owned->memory != nullptr) {
        sc_memory_destroy(owned->memory);
    }
    if (owned->tts != nullptr) {
        sc_tts_destroy(owned->tts);
    }
    if (owned->transcriber != nullptr) {
        sc_transcriber_destroy(owned->transcriber);
    }
    if (owned->provider != nullptr &&
        !(owned->provider_handle_count == 1 && owned->provider == owned->provider_handles[0])) {
        sc_provider_destroy(owned->provider);
    }
    for (size_t i = 0; i < owned->provider_handle_count; i += 1) {
        sc_provider_destroy(owned->provider_handles[i]);
    }
    sc_estop_clear(&owned->estop);
    sc_security_policy_clear(&owned->policy);
    *owned = (boot_owned){0};
}
