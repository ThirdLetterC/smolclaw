#pragma once

#include <stdint.h>

#include "sc/async.h"
#include "sc/allocator.h"
#include "sc/contract.h"
#include "sc/json.h"
#include "sc/media.h"
#include "sc/observer.h"
#include "sc/result.h"
#include "sc/string.h"
#include "sc/tool.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: request fields are borrowed for the call. Response
 * strings are caller-owned. The handle owns impl and calls destroy exactly once.
 * The wrapper does not synchronize calls; provider implementations document
 * their own thread-safety.
 */
typedef struct sc_provider sc_provider;

typedef struct sc_provider_request {
    size_t struct_size;
    sc_str model;
    sc_str prompt;
    sc_str system_instruction;
    sc_str tool_specs_json;
    const sc_media_attachment *media;
    size_t media_count;
    sc_str media_context;
    bool cancel_requested;
    sc_str route_hint;
} sc_provider_request;

typedef enum sc_provider_mock_mode {
    SC_PROVIDER_MOCK_TEXT = 0,
    SC_PROVIDER_MOCK_TOOL_CALL,
    SC_PROVIDER_MOCK_MULTI_TOOL_CALL,
    SC_PROVIDER_MOCK_ERROR,
    SC_PROVIDER_MOCK_MALFORMED_TOOL_CALL
} sc_provider_mock_mode;

typedef enum sc_provider_stream_event_type {
    SC_PROVIDER_STREAM_TEXT_DELTA = 0,
    SC_PROVIDER_STREAM_DELTA = SC_PROVIDER_STREAM_TEXT_DELTA,
    SC_PROVIDER_STREAM_TOOL_CALL,
    SC_PROVIDER_STREAM_DONE,
    SC_PROVIDER_STREAM_ERROR,
    SC_PROVIDER_STREAM_REASONING_DELTA,
    SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_CALL,
    SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_RESULT,
    SC_PROVIDER_STREAM_FINAL_USAGE
} sc_provider_stream_event_type;

typedef enum sc_provider_mode {
    SC_PROVIDER_MODE_CHAT = 1u << 0u,
    SC_PROVIDER_MODE_STREAM = 1u << 1u,
    SC_PROVIDER_MODE_EMBEDDINGS = 1u << 2u,
    SC_PROVIDER_MODE_TOOL_CALLS = 1u << 3u,
    SC_PROVIDER_MODE_VISION = 1u << 4u,
    SC_PROVIDER_MODE_AUDIO = 1u << 5u,
} sc_provider_mode;

typedef enum sc_provider_capability {
    SC_PROVIDER_CAP_STREAMING = 1u << 16u,
    SC_PROVIDER_CAP_STREAMING_TOOL_EVENTS = 1u << 17u,
    SC_PROVIDER_CAP_REASONING_EVENTS = 1u << 18u,
    SC_PROVIDER_CAP_PRE_EXECUTED_TOOL_EVENTS = 1u << 19u
} sc_provider_capability;

typedef enum sc_provider_retry_class {
    SC_PROVIDER_RETRY_NONE = 0,
    SC_PROVIDER_RETRY_TRANSIENT,
    SC_PROVIDER_RETRY_RATE_LIMIT,
    SC_PROVIDER_RETRY_SERVER,
    SC_PROVIDER_RETRY_INVALID_CREDENTIALS,
    SC_PROVIDER_RETRY_INVALID_REQUEST,
    SC_PROVIDER_RETRY_SECURITY_DENIED,
    SC_PROVIDER_RETRY_CANCELLED,
} sc_provider_retry_class;

typedef enum sc_provider_thinking_level {
    SC_PROVIDER_THINKING_DEFAULT = 0,
    SC_PROVIDER_THINKING_DISABLED,
    SC_PROVIDER_THINKING_LOW,
    SC_PROVIDER_THINKING_MEDIUM,
    SC_PROVIDER_THINKING_HIGH
} sc_provider_thinking_level;

typedef struct sc_provider_options {
    size_t struct_size;
    sc_str provider_name;
    sc_str base_url;
    sc_str api_key;
    sc_str credential_env;
    sc_str default_model;
    uint32_t timeout_ms;
    double temperature;
    double top_p;
    int64_t top_k;
    int64_t max_output_tokens;
    sc_str generation_config_json;
    sc_str safety_settings_json;
    bool streaming;
    bool validate_model;
    uint32_t max_retries;
    uint32_t retry_backoff_ms;
    bool merge_system_into_user;
    sc_provider_thinking_level thinking_level;
    double input_cost_per_million;
    double output_cost_per_million;
    sc_str openrouter_referer;
    sc_str openrouter_title;
    bool allow_loopback;
    sc_str deployment;
    sc_str api_version;
    sc_str generic_credential_env;
    sc_str secret_value;
    sc_str session_token;
    sc_str reasoning_effort;
    sc_str options_json;
    sc_str format_json;
    sc_str region;
    sc_str command;
    bool native_tool_streaming;
    bool think;
    bool think_set;
    sc_str mcp_server;
    sc_str mcp_tool;
    sc_str mcp_prompt_field;
    sc_str mcp_transport;
    sc_str mcp_args;
    sc_str mcp_url;
    sc_str mcp_headers;
} sc_provider_options;

typedef struct sc_provider_bedrock_credentials {
    size_t struct_size;
    sc_string access_key_id;
    sc_string secret_access_key;
    sc_string session_token;
} sc_provider_bedrock_credentials;

typedef sc_status (*sc_provider_bedrock_metadata_fetch_fn)(void *user_data,
                                                           sc_str url,
                                                           sc_str token,
                                                           sc_allocator *alloc,
                                                           sc_string *out);

typedef struct sc_provider_timeout_policy {
    size_t struct_size;
    uint32_t connect_timeout_ms;
    uint32_t read_timeout_ms;
    uint32_t write_timeout_ms;
    uint32_t total_timeout_ms;
    size_t response_body_limit_bytes;
} sc_provider_timeout_policy;

typedef struct sc_provider_sse_parser {
    size_t struct_size;
    sc_allocator *alloc;
    sc_string partial;
    size_t max_partial_bytes;
    bool failed;
    bool done;
} sc_provider_sse_parser;

typedef struct sc_provider_tool_call {
    size_t struct_size;
    sc_string call_id;
    sc_string name;
    sc_string arguments_json;
    sc_json_value *arguments;
} sc_provider_tool_call;

typedef struct sc_provider_response {
    size_t struct_size;
    sc_string text;
    sc_vec tool_calls;
    bool malformed_tool_call;
    bool streaming_complete;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t total_tokens;
    double cost_usd;
    sc_string finish_reason;
    sc_string model;
    sc_string error_message;
    sc_string reasoning_text;
    sc_provider_tool_call pre_executed_tool_call;
    sc_string pre_executed_tool_result;
} sc_provider_response;

typedef struct sc_provider_route {
    size_t struct_size;
    sc_str hint;
    sc_provider *provider;
} sc_provider_route;

typedef struct sc_provider_stream_event {
    size_t struct_size;
    sc_provider_stream_event_type type;
    sc_string text;
    sc_provider_tool_call tool_call;
    sc_string error_message;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t total_tokens;
    double cost_usd;
} sc_provider_stream_event;

typedef sc_status (*sc_provider_stream_callback)(void *user_data,
                                                 const sc_provider_stream_event *event);

typedef sc_status (*sc_provider_http_send_fn)(void *user_data,
                                              const sc_provider_options *options,
                                              sc_str request_json,
                                              sc_allocator *alloc,
                                              sc_string *response_json,
                                              int *http_status);
typedef void (*sc_provider_generate_complete_fn)(void *user_data,
                                                 const sc_provider_response *response,
                                                 sc_status status);
typedef void (*sc_provider_stream_complete_fn)(void *user_data, sc_status status);

typedef struct sc_provider_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*generate)(void *impl,
                          const sc_provider_request *request,
                          sc_allocator *alloc,
                          sc_provider_response *out);
    sc_status (*stream)(void *impl,
                        const sc_provider_request *request,
                        sc_allocator *alloc,
                        sc_provider_stream_callback callback,
                        void *callback_user_data);
    void (*destroy)(void *impl);
    const char *description_key;
    const char *config_schema_ref;
    const char *const *required_secret_keys;
    size_t required_secret_key_count;
    sc_provider_timeout_policy default_timeout;
    uint64_t provider_modes;
    sc_status (*generate_async)(void *impl,
                                sc_async_context *context,
                                const sc_provider_request *request,
                                sc_allocator *alloc,
                                sc_provider_generate_complete_fn complete,
                                void *complete_user_data,
                                sc_async_op **out);
    sc_status (*stream_async)(void *impl,
                              sc_async_context *context,
                              const sc_provider_request *request,
                              sc_allocator *alloc,
                              sc_provider_stream_callback callback,
                              void *callback_user_data,
                              sc_provider_stream_complete_fn complete,
                              void *complete_user_data,
                              sc_async_op **out);
} sc_provider_vtab;

static inline bool sc_provider_handle_is_null(const sc_provider *provider)
{
    return provider == nullptr;
}

bool sc_provider_vtab_valid(const sc_provider_vtab *vtab);
sc_status sc_provider_new(sc_allocator *alloc,
                          const sc_provider_vtab *vtab,
                          void *impl,
                          sc_provider **out);
sc_status sc_provider_generate(sc_provider *provider,
                               const sc_provider_request *request,
                               sc_allocator *alloc,
                               sc_provider_response *out);
sc_status sc_provider_stream(sc_provider *provider,
                             const sc_provider_request *request,
                             sc_allocator *alloc,
                             sc_provider_stream_callback callback,
                             void *callback_user_data);
sc_status sc_provider_generate_async(sc_provider *provider,
                                     sc_async_context *context,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_generate_complete_fn complete,
                                     void *complete_user_data,
                                     sc_async_op **out);
sc_status sc_provider_stream_async(sc_provider *provider,
                                   sc_async_context *context,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_stream_callback callback,
                                   void *callback_user_data,
                                   sc_provider_stream_complete_fn complete,
                                   void *complete_user_data,
                                   sc_async_op **out);
const sc_provider_vtab *sc_provider_vtab_of(const sc_provider *provider);
void sc_provider_destroy(sc_provider *provider);

void sc_provider_tool_call_clear(sc_provider_tool_call *call);
sc_status sc_provider_tool_call_as_tool_call(const sc_provider_tool_call *source, sc_tool_call *out);
void sc_provider_response_init(sc_provider_response *response, sc_allocator *alloc);
sc_status sc_provider_response_add_tool_call(sc_provider_response *response, const sc_provider_tool_call *call);
void sc_provider_response_clear(sc_provider_response *response);
void sc_provider_stream_event_clear(sc_provider_stream_event *event);
sc_status sc_provider_parse_thinking_level(sc_str value, sc_provider_thinking_level *out);
const char *sc_provider_thinking_level_name(sc_provider_thinking_level level);

sc_status sc_provider_mock_new(sc_allocator *alloc,
                               sc_provider_mock_mode mode,
                               sc_str text,
                               sc_provider **out);
sc_status sc_provider_openai_compatible_new(sc_allocator *alloc,
                                            const sc_provider_options *options,
                                            sc_provider_http_send_fn send,
                                            void *user_data,
                                            sc_provider **out);
sc_status sc_provider_openai_compatible_http_new(sc_allocator *alloc,
                                                 const sc_provider_options *options,
                                                 sc_provider **out);
sc_status sc_provider_openai_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_openrouter_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_azure_openai_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_anthropic_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_gemini_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_ollama_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_llamacpp_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_sglang_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_vllm_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_bedrock_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_gemini_cli_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_copilot_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_claude_code_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_telnyx_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_kilocli_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out);
sc_status sc_provider_reliable_new(sc_allocator *alloc,
                                   sc_provider **providers,
                                   size_t provider_count,
                                   sc_provider **out);
sc_status sc_provider_reliable_new_with_options(sc_allocator *alloc,
                                                sc_provider **providers,
                                                size_t provider_count,
                                                uint32_t max_retries,
                                                uint32_t retry_backoff_ms,
                                                sc_provider **out);
sc_status sc_provider_router_new(sc_allocator *alloc,
                                 sc_provider *default_provider,
                                 sc_provider *tool_provider,
                                 sc_observer *observer,
                                 sc_provider **out);
sc_status sc_provider_router_routes_new(sc_allocator *alloc,
                                        sc_provider *default_provider,
                                        const sc_provider_route *routes,
                                        size_t route_count,
                                        sc_observer *observer,
                                        sc_provider **out);
sc_status sc_provider_resolve_credential(sc_allocator *alloc,
                                         const sc_provider_options *options,
                                         sc_string *out);
sc_status sc_provider_redact_credential(sc_allocator *alloc, sc_str value, sc_string *out);
sc_status sc_provider_bedrock_resolve_credentials(sc_allocator *alloc,
                                                  const sc_provider_options *options,
                                                  sc_provider_bedrock_credentials *out);
void sc_provider_bedrock_credentials_clear(sc_provider_bedrock_credentials *credentials);
void sc_provider_bedrock_set_metadata_fetcher(sc_provider_bedrock_metadata_fetch_fn fetcher,
                                              void *user_data);
sc_status sc_provider_validate_request(const sc_provider_vtab *vtab,
                                       const sc_provider_options *options,
                                       const sc_provider_request *request);
sc_provider_retry_class sc_provider_classify_retry(sc_status status, int http_status);
bool sc_provider_should_retry(sc_status status, int http_status);
bool sc_provider_url_allowed(sc_str url, bool allow_loopback);
void sc_provider_sse_parser_init(sc_provider_sse_parser *parser, sc_allocator *alloc, size_t max_partial_bytes);
sc_status sc_provider_sse_parser_push(sc_provider_sse_parser *parser,
                                      sc_str chunk,
                                      sc_allocator *event_alloc,
                                      sc_provider_stream_callback callback,
                                      void *callback_user_data);
sc_status sc_provider_sse_parser_finish(sc_provider_sse_parser *parser,
                                        sc_provider_stream_callback callback,
                                        void *callback_user_data);
void sc_provider_sse_parser_clear(sc_provider_sse_parser *parser);
sc_status sc_provider_openai_build_request(sc_allocator *alloc,
                                           const sc_provider_options *options,
                                           const sc_provider_request *request,
                                           sc_string *out);
sc_status sc_provider_openai_parse_response(sc_allocator *alloc,
                                            sc_str response_json,
                                            sc_provider_response *out);
sc_status sc_provider_openai_parse_sse_event(sc_allocator *alloc,
                                             sc_str sse_event,
                                             sc_provider_stream_event *out);
sc_status sc_provider_openai_compatible_preset_new(sc_allocator *alloc,
                                                   sc_str kind,
                                                   const sc_provider_options *options,
                                                   sc_provider **out);
sc_status sc_provider_anthropic_build_request(sc_allocator *alloc,
                                              const sc_provider_options *options,
                                              const sc_provider_request *request,
                                              sc_string *out);
sc_status sc_provider_anthropic_parse_response(sc_allocator *alloc,
                                               sc_str response_json,
                                               sc_provider_response *out);
sc_status sc_provider_anthropic_parse_sse_event(sc_allocator *alloc,
                                                sc_str sse_event,
                                                sc_provider_stream_event *out);
sc_status sc_provider_gemini_build_request(sc_allocator *alloc,
                                           const sc_provider_options *options,
                                           const sc_provider_request *request,
                                           sc_string *out);
sc_status sc_provider_gemini_parse_response(sc_allocator *alloc,
                                            sc_str response_json,
                                            sc_provider_response *out);
sc_status sc_provider_gemini_parse_sse_event(sc_allocator *alloc,
                                             sc_str sse_event,
                                             sc_provider_stream_event *out);
sc_status sc_provider_ollama_build_request(sc_allocator *alloc,
                                           const sc_provider_options *options,
                                           const sc_provider_request *request,
                                           sc_string *out);
sc_status sc_provider_ollama_parse_response(sc_allocator *alloc,
                                            sc_str response_json,
                                            sc_provider_response *out);
sc_status sc_provider_ollama_parse_stream(sc_allocator *alloc,
                                          sc_str stream_json,
                                          sc_provider_stream_callback callback,
                                          void *callback_user_data);
sc_status sc_provider_bedrock_build_request(sc_allocator *alloc,
                                            const sc_provider_options *options,
                                            const sc_provider_request *request,
                                            sc_string *out);
sc_status sc_provider_bedrock_parse_response(sc_allocator *alloc,
                                             sc_str response_json,
                                             sc_provider_response *out);

SC_END_DECLS
