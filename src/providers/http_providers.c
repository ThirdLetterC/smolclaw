// cppcheck-suppress-file redundantInitialization
#include "sc/provider.h"

#include "sc/api.h"
#include "sc/log.h"

#include "net/http_client.h"
#include "net/curl_global.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

typedef enum provider_kind {
    PROVIDER_ANTHROPIC = 0,
    PROVIDER_GEMINI,
    PROVIDER_OLLAMA,
    PROVIDER_BEDROCK
} provider_kind;

typedef enum auth_kind {
    AUTH_NONE = 0,
    AUTH_OPENAI_BEARER,
    AUTH_ANTHROPIC_KEY,
    AUTH_GEMINI_KEY,
    AUTH_AWS_SIGV4
} auth_kind;

typedef struct http_provider {
    sc_allocator *alloc;
    provider_kind kind;
    sc_provider_options options;
    sc_string provider_name;
    sc_string base_url;
    sc_string api_key;
    sc_string credential_env;
    sc_string generic_credential_env;
    sc_string secret_value;
    sc_string session_token;
    sc_string default_model;
    sc_string generation_config_json;
    sc_string safety_settings_json;
    sc_string reasoning_effort;
    sc_string options_json;
    sc_string format_json;
    sc_string region;
    sc_string command;
} http_provider;

typedef struct bedrock_ini_keys {
    sc_str access_key_id;
    sc_str secret_access_key;
    sc_str session_token;
} bedrock_ini_keys;

typedef struct bedrock_metadata_fetcher {
    sc_provider_bedrock_metadata_fetch_fn fetcher;
    void *user_data;
} bedrock_metadata_fetcher;

#ifdef SC_HAVE_LIBCURL
typedef struct http_response_buffer {
    sc_bytes bytes;
    size_t max_bytes;
    bool too_large;
} http_response_buffer;

typedef sc_status (*provider_stream_chunk_fn)(void *user_data, sc_str chunk);
typedef sc_status (*provider_stream_finish_fn)(void *user_data);

typedef struct provider_stream_curl_context {
    provider_stream_chunk_fn on_chunk;
    void *user_data;
    sc_status status;
    size_t received_bytes;
} provider_stream_curl_context;

typedef sc_status (*provider_sse_event_parse_fn)(sc_allocator *alloc,
                                                 sc_str sse_event,
                                                 sc_provider_stream_event *out);

typedef struct provider_sse_stream_state {
    sc_allocator *alloc;
    sc_string partial;
    size_t max_partial_bytes;
    provider_sse_event_parse_fn parse;
    sc_provider_stream_callback callback;
    void *callback_user_data;
    bool failed;
    bool done;
} provider_sse_stream_state;

typedef struct ollama_stream_state {
    sc_allocator *alloc;
    sc_string partial;
    size_t max_partial_bytes;
    sc_provider_stream_callback callback;
    void *callback_user_data;
    bool failed;
    bool done;
} ollama_stream_state;
#endif

typedef struct http_provider_async_state {
    sc_allocator *alloc;
    http_provider *provider;
    sc_provider_options options;
    sc_provider_generate_complete_fn generate_complete;
    sc_provider_stream_callback stream_callback;
    void *stream_callback_user_data;
    sc_provider_stream_complete_fn stream_complete;
    void *complete_user_data;
    sc_string request_json;
    sc_string url;
    sc_string credential;
    sc_string aws_secret;
    sc_string aws_session_token;
    sc_string aws_sigv4;
    sc_provider_bedrock_credentials aws_credentials;
    sc_vec headers;
    sc_http_op *op;
#ifdef SC_HAVE_LIBCURL
    provider_sse_stream_state sse_state;
    ollama_stream_state ollama_state;
#endif
    auth_kind auth;
    bool streaming;
} http_provider_async_state;

static sc_status http_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out);
static sc_status http_provider_stream(void *impl,
                                      const sc_provider_request *request,
                                      sc_allocator *alloc,
                                      sc_provider_stream_callback callback,
                                      void *callback_user_data);
static sc_status http_provider_generate_async(void *impl,
                                              sc_async_context *context,
                                              const sc_provider_request *request,
                                              sc_allocator *alloc,
                                              sc_provider_generate_complete_fn complete,
                                              void *complete_user_data,
                                              sc_async_op **out);
static sc_status http_provider_stream_async(void *impl,
                                            sc_async_context *context,
                                            const sc_provider_request *request,
                                            sc_allocator *alloc,
                                            sc_provider_stream_callback callback,
                                            void *callback_user_data,
                                            sc_provider_stream_complete_fn complete,
                                            void *complete_user_data,
                                            sc_async_op **out);
static void http_provider_destroy(void *impl);
static sc_status http_provider_async_prepare(http_provider_async_state *state, const sc_provider_request *request);
static sc_status http_provider_async_prepare_stream(http_provider_async_state *state, const sc_provider_request *request);
static sc_status http_provider_async_build_headers(http_provider_async_state *state);
static sc_status http_provider_async_add_header(http_provider_async_state *state, sc_str name, sc_str value);
static sc_status http_provider_async_schedule(http_provider_async_state *state,
                                              sc_async_context *context,
                                              sc_http_complete_fn complete);
static sc_status http_provider_async_stream_chunk(void *user_data, sc_buf chunk);
static void http_provider_async_generate_complete(void *user_data, const sc_http_response *response, sc_status status);
static void http_provider_async_stream_complete(void *user_data, const sc_http_response *response, sc_status status);
static sc_status http_provider_async_parse_response(http_provider_async_state *state,
                                                    sc_str response_json,
                                                    long http_status,
                                                    sc_provider_response *out);
static void http_provider_async_state_destroy(http_provider_async_state *state);
static void http_provider_async_headers_clear(http_provider_async_state *state);
static sc_status http_provider_new(sc_allocator *alloc,
                                   provider_kind kind,
                                   const sc_provider_options *options,
                                   const sc_provider_vtab *vtab,
                                   sc_provider **out);
static sc_status copy_options(http_provider *provider, const sc_provider_options *options);
static sc_str string_as_option(const sc_string *string);
static sc_str model_or_default(const sc_provider_options *options, const sc_provider_request *request);
static sc_status set_string(sc_json_value *object, sc_str key, sc_str value);
static sc_status set_number(sc_json_value *object, sc_str key, double value);
static sc_status set_raw_json(sc_allocator *alloc, sc_json_value *object, sc_str key, sc_str raw_json);
static sc_status add_anthropic_tools(sc_allocator *alloc, sc_json_value *root, sc_str tool_specs_json);
static sc_status append_text_parts(sc_allocator *alloc, sc_json_value *parts, sc_string *out);
static sc_status append_gemini_part(sc_allocator *alloc, sc_json_value *parts, sc_json_value *part);
static sc_status append_gemini_text_part(sc_allocator *alloc, sc_json_value *parts, sc_str text);
static sc_status append_gemini_media_part(sc_allocator *alloc, sc_json_value *parts, const sc_media_attachment *attachment);
static sc_status base64_encode(sc_allocator *alloc, sc_buf input, sc_string *out);
static sc_status add_gemini_system_instruction(sc_allocator *alloc,
                                               sc_json_value *root,
                                               const sc_provider_options *options,
                                               const sc_provider_request *request);
static sc_status add_gemini_generation_config(sc_allocator *alloc,
                                              sc_json_value *root,
                                              const sc_provider_options *options);
static sc_status add_gemini_tools(sc_allocator *alloc, sc_json_value *root, sc_str tool_specs_json);
static sc_status add_ollama_options(sc_allocator *alloc, sc_json_value *root, const sc_provider_options *options);
static sc_status add_ollama_reasoning_controls(sc_allocator *alloc, sc_json_value *root, const sc_provider_options *options);
static sc_status add_ollama_format(sc_allocator *alloc, sc_json_value *root, const sc_provider_options *options);
static sc_status add_ollama_message(sc_allocator *alloc, sc_json_value *messages, sc_str role, sc_str content);
static sc_status add_ollama_tools(sc_allocator *alloc, sc_json_value *root, sc_str tool_specs_json);
static sc_status parse_gemini_json_response(sc_allocator *alloc, sc_str response_json, sc_provider_response *out);
static sc_status emit_response_as_stream(sc_allocator *alloc,
                                         const sc_provider_response *response,
                                         sc_provider_stream_callback callback,
                                         void *callback_user_data);
static sc_status sse_event_payload(sc_allocator *alloc, sc_str sse_event, sc_string *out);
static sc_status parse_gemini_part(sc_allocator *alloc,
                                   sc_json_value *part,
                                   sc_provider_response *out,
                                   size_t index);
static sc_status parse_gemini_grounding(sc_allocator *alloc, sc_json_value *root, sc_provider_response *out);
static sc_status parse_ollama_tool_calls(sc_allocator *alloc, sc_json_value *message, sc_provider_response *out);
static sc_status parse_gemini_error_status(sc_allocator *alloc,
                                           sc_str response_json,
                                           const char *fallback_key,
                                           sc_status_code code);
static bool contains_data_prefix(sc_str value);
static bool gemini_model_name_valid(sc_str model);
static void apply_usage_cost(const sc_provider_options *options, sc_provider_response *response);
static sc_str reasoning_effort_option(const sc_provider_options *options);
static sc_status build_anthropic_url(const sc_provider_options *options, sc_allocator *alloc, sc_string *out);
static sc_status build_gemini_url(const sc_provider_options *options,
                                  const sc_provider_request *request,
                                  sc_allocator *alloc,
                                  sc_string *out);
static sc_status build_ollama_url(const sc_provider_options *options, sc_allocator *alloc, sc_string *out);
static sc_status build_azure_openai_url(const sc_provider_options *options, sc_allocator *alloc, sc_string *out);
static sc_status build_bedrock_url(const sc_provider_options *options,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_string *out);
static bool str_ends_with(sc_str value, const char *suffix);
static bool provider_http_status_retryable(long response_code);
static bool str_has_prefix(sc_str value, const char *prefix);
static bool str_equal_cstr(sc_str value, const char *expected);
static sc_str compatible_preset_base_url(sc_str kind);
static sc_str compatible_preset_env(sc_str kind);
static void sleep_ms(uint32_t delay_ms);
static sc_status copy_secret(sc_allocator *alloc, sc_str value, sc_string *out);
static sc_status bedrock_credentials_set(sc_allocator *alloc,
                                         sc_str access_key_id,
                                         sc_str secret_access_key,
                                         sc_str session_token,
                                         sc_provider_bedrock_credentials *out);
static sc_status bedrock_resolve_from_environment(sc_allocator *alloc, sc_provider_bedrock_credentials *out);
static sc_status bedrock_resolve_from_shared_files(sc_allocator *alloc, sc_provider_bedrock_credentials *out);
static sc_status bedrock_resolve_from_metadata(sc_allocator *alloc, sc_provider_bedrock_credentials *out);
static sc_status bedrock_read_file(sc_allocator *alloc, sc_str path, size_t max_bytes, sc_string *out);
static sc_status bedrock_default_file_path(sc_allocator *alloc, const char *suffix, sc_string *out);
static bool bedrock_find_profile_keys(sc_str text, sc_str profile, bool config_style, bedrock_ini_keys *out);
static bool bedrock_ini_section_matches(sc_str section, sc_str profile, bool config_style);
static sc_str bedrock_trim(sc_str value);
static sc_status bedrock_copy_profile_value(sc_allocator *alloc,
                                            const sc_string *credentials_text,
                                            const sc_string *config_text,
                                            sc_str profile,
                                            sc_provider_bedrock_credentials *out);
static sc_status bedrock_fetch_metadata_json(sc_allocator *alloc, sc_str url, sc_str token, sc_string *out);
static sc_status bedrock_parse_metadata_credentials(sc_allocator *alloc,
                                                    sc_str payload,
                                                    sc_provider_bedrock_credentials *out);
static bool bedrock_metadata_url_allowed(sc_str url, bool full_uri);
static bool env_truthy(const char *value);

#ifdef SC_HAVE_LIBCURL
enum {
    provider_low_memory_upload_buffer_size = 16 * 1024,
};

static sc_status provider_curl_global_init(void);
static void provider_curl_global_cleanup(void);
static sc_status provider_curl_shared_handle(CURL **out, bool *shared);
static void provider_curl_release_handle(CURL *curl, bool shared);
static void provider_curl_apply_common_options(CURL *curl);
static bool provider_low_memory_transport_enabled(void);
static const char *provider_ca_bundle_path(void);
static sc_status openai_curl_send(void *user_data,
                                  const sc_provider_options *options,
                                  sc_str request_json,
                                  sc_allocator *alloc,
                                  sc_string *response_json,
                                  int *http_status);
static sc_status compatible_curl_send(void *user_data,
                                      const sc_provider_options *options,
                                      sc_str request_json,
                                      sc_allocator *alloc,
                                      sc_string *response_json,
                                      int *http_status);
static sc_status provider_curl_json_post(const sc_provider_options *options,
                                         sc_str url,
                                         sc_str request_json,
                                         auth_kind auth,
                                         sc_allocator *alloc,
                                         sc_string *response_json,
                                         int *http_status);
static sc_status provider_curl_stream_post(const sc_provider_options *options,
                                           sc_str url,
                                           sc_str request_json,
                                           auth_kind auth,
                                           provider_stream_chunk_fn on_chunk,
                                           provider_stream_finish_fn on_finish,
                                           void *stream_user_data,
                                           int *http_status);
static const char *auth_kind_name(auth_kind auth);
static sc_status provider_redact_url_for_log(sc_allocator *alloc, sc_str url, sc_string *out);
static sc_status add_header(struct curl_slist **headers, const char *header);
static sc_status add_header_with_value(sc_allocator *alloc,
                                       struct curl_slist **headers,
                                       const char *name,
                                       sc_str value);
static bool provider_curl_code_retryable(CURLcode code);
static size_t provider_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
static size_t provider_curl_stream_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata);
static sc_status provider_sse_stream_push(void *user_data, sc_str chunk);
static sc_status provider_sse_stream_finish(void *user_data);
static void provider_sse_stream_clear(provider_sse_stream_state *state);
static sc_status ollama_stream_push(void *user_data, sc_str chunk);
static sc_status ollama_stream_finish(void *user_data);
static void ollama_stream_clear(ollama_stream_state *state);
static sc_status ollama_stream_emit_line(ollama_stream_state *state, sc_str line);
#endif

static bedrock_metadata_fetcher g_bedrock_metadata_fetcher = {0};
#ifdef SC_HAVE_LIBCURL
static CURL *g_provider_curl = nullptr;
static bool g_provider_curl_in_use = false;
#endif

static const sc_provider_vtab anthropic_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "anthropic",
    .display_name = "Anthropic Messages provider",
    .feature_flag = "SC_PROVIDER_ANTHROPIC",
    .capabilities = SC_CONTRACT_CAP_STREAMING | SC_CONTRACT_CAP_TOOLS |
                    SC_PROVIDER_CAP_STREAMING | SC_PROVIDER_CAP_STREAMING_TOOL_EVENTS |
                    SC_PROVIDER_CAP_REASONING_EVENTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = http_provider_generate,
    .stream = http_provider_stream,
    .destroy = http_provider_destroy,
    .description_key = "sc.provider.anthropic.description",
    .config_schema_ref = "sc.schema.provider.anthropic.v1",
    .required_secret_keys = (const char *const[]){"api_key"},
    .required_secret_key_count = 1,
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 8U * 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM | SC_PROVIDER_MODE_TOOL_CALLS | SC_PROVIDER_MODE_VISION,
    .generate_async = http_provider_generate_async,
    .stream_async = http_provider_stream_async,
};

static const sc_provider_vtab gemini_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "gemini",
    .display_name = "Gemini generateContent provider",
    .feature_flag = "SC_PROVIDER_GEMINI",
    .capabilities = SC_CONTRACT_CAP_STREAMING | SC_CONTRACT_CAP_TOOLS |
                    SC_PROVIDER_CAP_STREAMING | SC_PROVIDER_CAP_STREAMING_TOOL_EVENTS |
                    SC_PROVIDER_CAP_PRE_EXECUTED_TOOL_EVENTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = http_provider_generate,
    .stream = http_provider_stream,
    .destroy = http_provider_destroy,
    .description_key = "sc.provider.gemini.description",
    .config_schema_ref = "sc.schema.provider.gemini.v1",
    .required_secret_keys = (const char *const[]){"api_key"},
    .required_secret_key_count = 1,
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 8U * 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM | SC_PROVIDER_MODE_TOOL_CALLS | SC_PROVIDER_MODE_VISION,
    .generate_async = http_provider_generate_async,
    .stream_async = http_provider_stream_async,
};

static const sc_provider_vtab ollama_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "ollama",
    .display_name = "Ollama local provider",
    .feature_flag = "SC_PROVIDER_OLLAMA",
    .capabilities = SC_CONTRACT_CAP_STREAMING | SC_CONTRACT_CAP_TOOLS |
                    SC_PROVIDER_CAP_STREAMING | SC_PROVIDER_CAP_STREAMING_TOOL_EVENTS |
                    SC_PROVIDER_CAP_REASONING_EVENTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = http_provider_generate,
    .stream = http_provider_stream,
    .destroy = http_provider_destroy,
    .description_key = "sc.provider.ollama.description",
    .config_schema_ref = "sc.schema.provider.ollama.v1",
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 8U * 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM | SC_PROVIDER_MODE_TOOL_CALLS,
    .generate_async = http_provider_generate_async,
    .stream_async = http_provider_stream_async,
};

static const sc_provider_vtab bedrock_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "bedrock",
    .display_name = "AWS Bedrock Converse provider",
    .feature_flag = "SC_PROVIDER_BEDROCK",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = http_provider_generate,
    .stream = nullptr,
    .destroy = http_provider_destroy,
    .description_key = "sc.provider.bedrock.description",
    .config_schema_ref = "sc.schema.provider.bedrock.v1",
    .required_secret_keys = (const char *const[]){"aws_access_key_id", "aws_secret_access_key"},
    .required_secret_key_count = 2,
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 8U * 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT,
    .generate_async = http_provider_generate_async,
};

sc_status sc_provider_openai_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
#ifdef SC_HAVE_LIBCURL
    sc_provider_options resolved;

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.provider_openai.invalid_argument");
    }
    resolved = *options;
    if (resolved.provider_name.ptr == nullptr || resolved.provider_name.len == 0) {
        resolved.provider_name = sc_str_from_cstr("openai");
    }
    if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
        resolved.base_url = sc_str_from_cstr("https://api.openai.com/v1/chat/completions");
    }
    return sc_provider_openai_compatible_new(alloc, &resolved, openai_curl_send, nullptr, out);
#else
    (void)alloc;
    (void)options;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.provider_openai.libcurl_unavailable");
#endif
}

sc_status sc_provider_openai_compatible_http_new(sc_allocator *alloc,
                                                 const sc_provider_options *options,
                                                 sc_provider **out)
{
#ifdef SC_HAVE_LIBCURL
    sc_provider_options resolved;

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.invalid_argument");
    }
    resolved = *options;
    if (resolved.provider_name.ptr == nullptr || resolved.provider_name.len == 0) {
        resolved.provider_name = sc_str_from_cstr("openai-compatible");
    }
    return sc_provider_openai_compatible_new(alloc, &resolved, compatible_curl_send, nullptr, out);
#else
    (void)alloc;
    (void)options;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.provider_compatible.libcurl_unavailable");
#endif
}

sc_status sc_provider_openrouter_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
#ifdef SC_HAVE_LIBCURL
    sc_provider_options resolved = {0};

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.provider_openrouter.invalid_argument");
    }
    resolved = *options;
    resolved.provider_name = sc_str_from_cstr("openrouter");
    if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
        resolved.base_url = sc_str_from_cstr("https://openrouter.ai/api/v1/chat/completions");
    }
    return sc_provider_openai_compatible_new(alloc, &resolved, openai_curl_send, nullptr, out);
#else
    (void)alloc;
    (void)options;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.provider_openrouter.libcurl_unavailable");
#endif
}

sc_status sc_provider_azure_openai_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
#ifdef SC_HAVE_LIBCURL
    sc_provider_options resolved;
    sc_string url = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.provider_azure_openai.invalid_argument");
    }
    if (options->deployment.len == 0 || options->api_version.len == 0) {
        return sc_status_invalid_argument("sc.provider_azure_openai.missing_deployment_or_api_version");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = build_azure_openai_url(options, alloc, &url);
    if (sc_status_is_ok(status)) {
        resolved = *options;
        resolved.provider_name = sc_str_from_cstr("azure-openai");
        resolved.base_url = sc_string_as_str(&url);
        status = sc_provider_openai_compatible_new(alloc, &resolved, openai_curl_send, nullptr, out);
    }
    sc_string_clear(&url);
    return status;
#else
    (void)alloc;
    (void)options;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.provider_azure_openai.libcurl_unavailable");
#endif
}

sc_status sc_provider_llamacpp_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    sc_provider_options resolved = {0};
    if (options != nullptr) {
        resolved = *options;
    }
    resolved.provider_name = sc_str_from_cstr("llamacpp");
    resolved.allow_loopback = true;
    if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
        resolved.base_url = sc_str_from_cstr("http://localhost:8080/v1");
    }
    return sc_provider_openai_compatible_http_new(alloc, &resolved, out);
}

sc_status sc_provider_sglang_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    sc_provider_options resolved = {0};
    if (options != nullptr) {
        resolved = *options;
    }
    resolved.provider_name = sc_str_from_cstr("sglang");
    resolved.allow_loopback = true;
    if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
        resolved.base_url = sc_str_from_cstr("http://localhost:30000/v1");
    }
    return sc_provider_openai_compatible_http_new(alloc, &resolved, out);
}

sc_status sc_provider_vllm_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    sc_provider_options resolved = {0};
    if (options != nullptr) {
        resolved = *options;
    }
    resolved.provider_name = sc_str_from_cstr("vllm");
    resolved.allow_loopback = true;
    if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
        resolved.base_url = sc_str_from_cstr("http://localhost:8000/v1");
    }
    return sc_provider_openai_compatible_http_new(alloc, &resolved, out);
}

sc_status sc_provider_openai_compatible_preset_new(sc_allocator *alloc,
                                                   sc_str kind,
                                                   const sc_provider_options *options,
                                                   sc_provider **out)
{
    sc_provider_options resolved = {0};
    sc_str base_url = compatible_preset_base_url(kind);
    sc_str env = compatible_preset_env(kind);

    if (out == nullptr || kind.len == 0) {
        return sc_status_invalid_argument("sc.provider_compatible_preset.invalid_argument");
    }
    if (options != nullptr) {
        resolved = *options;
    }
    resolved.provider_name = kind;
    if (resolved.base_url.len == 0 && base_url.len > 0) {
        resolved.base_url = base_url;
    }
    if (resolved.credential_env.len == 0 && env.len > 0) {
        resolved.credential_env = env;
    }
    if (resolved.base_url.len == 0) {
        return sc_status_invalid_argument("sc.provider_compatible_preset.missing_base_url");
    }
    return sc_provider_openai_compatible_http_new(alloc, &resolved, out);
}

sc_status sc_provider_anthropic_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    return http_provider_new(alloc, PROVIDER_ANTHROPIC, options, &anthropic_vtab, out);
}

sc_status sc_provider_gemini_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    return http_provider_new(alloc, PROVIDER_GEMINI, options, &gemini_vtab, out);
}

sc_status sc_provider_ollama_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    return http_provider_new(alloc, PROVIDER_OLLAMA, options, &ollama_vtab, out);
}

sc_status sc_provider_bedrock_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    return http_provider_new(alloc, PROVIDER_BEDROCK, options, &bedrock_vtab, out);
}

sc_status sc_provider_bedrock_build_request(sc_allocator *alloc,
                                            const sc_provider_options *options,
                                            const sc_provider_request *request,
                                            sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *messages = nullptr;
    sc_json_value *message = nullptr;
    sc_json_value *content = nullptr;
    sc_json_value *text_part = nullptr;
    sc_json_value *system = nullptr;
    sc_json_value *system_part = nullptr;
    sc_json_value *inference = nullptr;
    sc_status status = sc_status_ok();
    bool any_inference = false;

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_bedrock.request_invalid_argument");
    }
    status = sc_provider_validate_request(&bedrock_vtab, options, request);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &root);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &messages);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &message);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("role"), sc_str_from_cstr("user"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &text_part);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(text_part, sc_str_from_cstr("text"), request->prompt);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(content, text_part);
        text_part = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(message, sc_str_from_cstr("content"), content);
        content = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(messages, message);
        message = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("messages"), messages);
        messages = nullptr;
    }
    if (sc_status_is_ok(status) && request->system_instruction.len > 0) {
        status = sc_json_array_new(alloc, &system);
        if (sc_status_is_ok(status)) {
            status = sc_json_object_new(alloc, &system_part);
        }
        if (sc_status_is_ok(status)) {
            status = set_string(system_part, sc_str_from_cstr("text"), request->system_instruction);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_array_append(system, system_part);
            system_part = nullptr;
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(root, sc_str_from_cstr("system"), system);
            system = nullptr;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &inference);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->max_output_tokens > 0) {
        status = set_number(inference, sc_str_from_cstr("maxTokens"), (double)options->max_output_tokens);
        any_inference = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->temperature > 0.0) {
        status = set_number(inference, sc_str_from_cstr("temperature"), options->temperature);
        any_inference = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->top_p > 0.0) {
        status = set_number(inference, sc_str_from_cstr("topP"), options->top_p);
        any_inference = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && any_inference) {
        status = sc_json_object_set(root, sc_str_from_cstr("inferenceConfig"), inference);
        inference = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }

    sc_json_destroy(root);
    sc_json_destroy(messages);
    sc_json_destroy(message);
    sc_json_destroy(content);
    sc_json_destroy(text_part);
    sc_json_destroy(system);
    sc_json_destroy(system_part);
    sc_json_destroy(inference);
    return status;
}

sc_status sc_provider_bedrock_parse_response(sc_allocator *alloc,
                                             sc_str response_json,
                                             sc_provider_response *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *output = nullptr;
    sc_json_value *message = nullptr;
    sc_json_value *content = nullptr;
    sc_json_value *usage = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();
    double number = 0.0;
    sc_str text = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_bedrock.response_invalid_argument");
    }
    status = sc_json_parse(alloc, response_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_json_object_get(root, sc_str_from_cstr("message")) != nullptr &&
        sc_json_object_get(root, sc_str_from_cstr("__type")) != nullptr) {
        sc_json_destroy(root);
        return sc_status_http("sc.provider_bedrock.error_response");
    }
    sc_provider_response_init(out, alloc);
    output = sc_json_object_get(root, sc_str_from_cstr("output"));
    message = sc_json_object_get(output, sc_str_from_cstr("message"));
    content = sc_json_object_get(message, sc_str_from_cstr("content"));
    status = append_text_parts(alloc, content, &out->text);
    if (sc_status_is_ok(status) &&
        sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("stopReason")), &text) &&
        text.len > 0) {
        status = sc_string_from_str(alloc, text, &out->finish_reason);
    }
    usage = sc_json_object_get(root, sc_str_from_cstr("usage"));
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("inputTokens")), &number)) {
        out->input_tokens = (int64_t)number;
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("outputTokens")), &number)) {
        out->output_tokens = (int64_t)number;
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("totalTokens")), &number)) {
        out->total_tokens = (int64_t)number;
    } else if (sc_status_is_ok(status)) {
        out->total_tokens = out->input_tokens + out->output_tokens;
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    sc_json_destroy(root);
    return status;
}

sc_status sc_provider_bedrock_resolve_credentials(sc_allocator *alloc,
                                                  const sc_provider_options *options,
                                                  sc_provider_bedrock_credentials *out)
{
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_bedrock.credentials_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_provider_bedrock_credentials){.struct_size = sizeof(*out)};

    if (options != nullptr && options->api_key.len > 0 && options->secret_value.len > 0) {
        return bedrock_credentials_set(alloc, options->api_key, options->secret_value, options->session_token, out);
    }
    if (options != nullptr && options->api_key.len > 0 && options->secret_value.len == 0) {
        return sc_status_security_denied("sc.provider_bedrock.missing_aws_secret_access_key");
    }
    if (options != nullptr && options->secret_value.len > 0 && options->api_key.len == 0) {
        return sc_status_security_denied("sc.provider_bedrock.missing_aws_access_key_id");
    }

    status = bedrock_resolve_from_environment(alloc, out);
    if (sc_status_is_ok(status)) {
        return status;
    }
    sc_status_clear(&status);
    sc_provider_bedrock_credentials_clear(out);

    status = bedrock_resolve_from_shared_files(alloc, out);
    if (sc_status_is_ok(status)) {
        return status;
    }
    sc_status_clear(&status);
    sc_provider_bedrock_credentials_clear(out);

    status = bedrock_resolve_from_metadata(alloc, out);
    if (sc_status_is_ok(status)) {
        return status;
    }
    if (status.error_key != nullptr &&
        (strcmp(status.error_key, "sc.provider_bedrock.metadata_url_denied") == 0 ||
         strcmp(status.error_key, "sc.provider_bedrock.imds_disabled") == 0)) {
        sc_provider_bedrock_credentials_clear(out);
        return status;
    }
    sc_status_clear(&status);
    sc_provider_bedrock_credentials_clear(out);
    return sc_status_security_denied("sc.provider_bedrock.credentials_missing");
}

void sc_provider_bedrock_credentials_clear(sc_provider_bedrock_credentials *credentials)
{
    if (credentials == nullptr) {
        return;
    }
    sc_string_secure_clear(&credentials->access_key_id);
    sc_string_secure_clear(&credentials->secret_access_key);
    sc_string_secure_clear(&credentials->session_token);
    *credentials = (sc_provider_bedrock_credentials){0};
}

void sc_provider_bedrock_set_metadata_fetcher(sc_provider_bedrock_metadata_fetch_fn fetcher,
                                              void *user_data)
{
    g_bedrock_metadata_fetcher = (bedrock_metadata_fetcher){
        .fetcher = fetcher,
        .user_data = user_data,
    };
}

sc_status sc_provider_anthropic_build_request(sc_allocator *alloc,
                                              const sc_provider_options *options,
                                              const sc_provider_request *request,
                                              sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *messages = nullptr;
    sc_json_value *message = nullptr;
    sc_json_value *stream = nullptr;
    sc_status status = sc_status_ok();

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_anthropic.request_invalid_argument");
    }

    status = sc_provider_validate_request(&anthropic_vtab, options, request);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &root);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(root, sc_str_from_cstr("model"), model_or_default(options, request));
    }
    if (sc_status_is_ok(status)) {
        status = set_number(root, sc_str_from_cstr("max_tokens"), 1024.0);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->streaming) {
        status = sc_json_bool_new(alloc, true, &stream);
    }
    if (sc_status_is_ok(status) && stream != nullptr) {
        status = sc_json_object_set(root, sc_str_from_cstr("stream"), stream);
        stream = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &messages);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &message);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("role"), sc_str_from_cstr("user"));
    }
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("content"), request->prompt);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(messages, message);
        message = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("messages"), messages);
        messages = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = add_anthropic_tools(alloc, root, request->tool_specs_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }

    sc_json_destroy(root);
    sc_json_destroy(messages);
    sc_json_destroy(message);
    sc_json_destroy(stream);
    return status;
}

sc_status sc_provider_anthropic_parse_response(sc_allocator *alloc,
                                               sc_str response_json,
                                               sc_provider_response *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *content = nullptr;
    sc_json_value *usage = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();
    double number = 0.0;
    sc_str text = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_anthropic.response_invalid_argument");
    }
    status = sc_json_parse(alloc, response_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_json_object_get(root, sc_str_from_cstr("error")) != nullptr) {
        sc_json_destroy(root);
        return sc_status_http("sc.provider_anthropic.error_response");
    }

    sc_provider_response_init(out, alloc);
    if (sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("model")), &text) && text.len > 0) {
        status = sc_string_from_str(alloc, text, &out->model);
    }
    if (sc_status_is_ok(status) &&
        sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("stop_reason")), &text) &&
        text.len > 0) {
        status = sc_string_from_str(alloc, text, &out->finish_reason);
    }
    usage = sc_json_object_get(root, sc_str_from_cstr("usage"));
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("input_tokens")), &number)) {
        out->input_tokens = (int64_t)number;
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("output_tokens")), &number)) {
        out->output_tokens = (int64_t)number;
    }
    if (sc_status_is_ok(status)) {
        out->total_tokens = out->input_tokens + out->output_tokens;
    }
    content = sc_json_object_get(root, sc_str_from_cstr("content"));
    if (sc_status_is_ok(status)) {
        status = append_text_parts(alloc, content, &out->text);
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    sc_json_destroy(root);
    return status;
}

sc_status sc_provider_anthropic_parse_sse_event(sc_allocator *alloc,
                                                sc_str sse_event,
                                                sc_provider_stream_event *out)
{
    sc_string payload = {0};
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_str type = {0};
    sc_str delta_type = {0};
    sc_str text = {0};
    sc_str id = {0};
    sc_str name = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_anthropic.sse_invalid_argument");
    }
    *out = (sc_provider_stream_event){.struct_size = sizeof(*out), .type = SC_PROVIDER_STREAM_DELTA};
    status = sse_event_payload(alloc, sse_event, &payload);
    if (sc_status_is_ok(status) && payload.len == 0) {
        sc_string_clear(&payload);
        return sc_status_ok();
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_parse(alloc, sc_string_as_str(&payload), &root, &error);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&payload);
        return status;
    }
    if (sc_json_object_get(root, sc_str_from_cstr("error")) != nullptr) {
        out->type = SC_PROVIDER_STREAM_ERROR;
        status = sc_string_from_cstr(alloc, "sc.provider_anthropic.error_response", &out->error_message);
        sc_json_destroy(root);
        sc_string_clear(&payload);
        return status;
    }
    if (!sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("type")), &type)) {
        status = sc_status_parse("sc.provider_anthropic.sse_missing_type");
    } else if (sc_str_equal(type, sc_str_from_cstr("message_stop"))) {
        out->type = SC_PROVIDER_STREAM_DONE;
    } else if (sc_str_equal(type, sc_str_from_cstr("message_delta"))) {
        double number = 0.0;
        sc_json_value *usage = sc_json_object_get(root, sc_str_from_cstr("usage"));
        out->type = SC_PROVIDER_STREAM_FINAL_USAGE;
        if (sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("input_tokens")), &number)) {
            out->input_tokens = (int64_t)number;
        }
        if (sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("output_tokens")), &number)) {
            out->output_tokens = (int64_t)number;
        }
        out->total_tokens = out->input_tokens + out->output_tokens;
    } else if (sc_str_equal(type, sc_str_from_cstr("content_block_delta"))) {
        sc_json_value *delta = sc_json_object_get(root, sc_str_from_cstr("delta"));
        if (sc_json_as_str(sc_json_object_get(delta, sc_str_from_cstr("type")), &delta_type) &&
            sc_str_equal(delta_type, sc_str_from_cstr("text_delta")) &&
            sc_json_as_str(sc_json_object_get(delta, sc_str_from_cstr("text")), &text)) {
            out->type = SC_PROVIDER_STREAM_DELTA;
            status = sc_string_from_str(alloc, text, &out->text);
        } else if (sc_json_as_str(sc_json_object_get(delta, sc_str_from_cstr("type")), &delta_type) &&
                   (sc_str_equal(delta_type, sc_str_from_cstr("thinking_delta")) ||
                    sc_str_equal(delta_type, sc_str_from_cstr("reasoning_delta"))) &&
                   (sc_json_as_str(sc_json_object_get(delta, sc_str_from_cstr("thinking")), &text) ||
                    sc_json_as_str(sc_json_object_get(delta, sc_str_from_cstr("text")), &text))) {
            out->type = SC_PROVIDER_STREAM_REASONING_DELTA;
            status = sc_string_from_str(alloc, text, &out->text);
        }
    } else if (sc_str_equal(type, sc_str_from_cstr("content_block_start"))) {
        sc_json_value *content_block = sc_json_object_get(root, sc_str_from_cstr("content_block"));
        if (sc_json_as_str(sc_json_object_get(content_block, sc_str_from_cstr("type")), &delta_type) &&
            sc_str_equal(delta_type, sc_str_from_cstr("tool_use")) &&
            sc_json_as_str(sc_json_object_get(content_block, sc_str_from_cstr("name")), &name)) {
            sc_json_value *input = sc_json_object_get(content_block, sc_str_from_cstr("input"));
            out->type = SC_PROVIDER_STREAM_TOOL_CALL;
            (void)sc_json_as_str(sc_json_object_get(content_block, sc_str_from_cstr("id")), &id);
            status = sc_string_from_str(alloc, id, &out->tool_call.call_id);
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, name, &out->tool_call.name);
            }
            if (sc_status_is_ok(status) && input != nullptr) {
                status = sc_json_serialize(input, alloc, &out->tool_call.arguments_json);
            } else if (sc_status_is_ok(status)) {
                status = sc_string_from_cstr(alloc, "{}", &out->tool_call.arguments_json);
            }
            if (sc_status_is_ok(status)) {
                status = sc_json_parse(alloc, sc_string_as_str(&out->tool_call.arguments_json), &out->tool_call.arguments, nullptr);
            }
        }
    }
    sc_json_destroy(root);
    sc_string_clear(&payload);
    if (!sc_status_is_ok(status)) {
        sc_provider_stream_event_clear(out);
    }
    return status;
}

sc_status sc_provider_gemini_build_request(sc_allocator *alloc,
                                           const sc_provider_options *options,
                                           const sc_provider_request *request,
                                           sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *contents = nullptr;
    sc_json_value *content = nullptr;
    sc_json_value *parts = nullptr;
    sc_status status = sc_status_ok();

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini.request_invalid_argument");
    }

    status = sc_provider_validate_request(&gemini_vtab, options, request);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &root);
    }
    if (sc_status_is_ok(status)) {
        status = add_gemini_system_instruction(alloc, root, options, request);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &contents);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &parts);
    }
    if (sc_status_is_ok(status)) {
        status = append_gemini_text_part(alloc, parts, request->prompt);
    }
    if (sc_status_is_ok(status) && request->media_context.len > 0) {
        status = append_gemini_text_part(alloc, parts, request->media_context);
    }
    for (size_t i = 0; sc_status_is_ok(status) && request->media != nullptr && i < request->media_count; i += 1) {
        status = append_gemini_media_part(alloc, parts, &request->media[i]);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(content, sc_str_from_cstr("parts"), parts);
        parts = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(contents, content);
        content = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("contents"), contents);
        contents = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = add_gemini_generation_config(alloc, root, options);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->safety_settings_json.len > 0) {
        status = set_raw_json(alloc, root, sc_str_from_cstr("safetySettings"), options->safety_settings_json);
    }
    if (sc_status_is_ok(status)) {
        status = add_gemini_tools(alloc, root, request->tool_specs_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }

    sc_json_destroy(root);
    sc_json_destroy(contents);
    sc_json_destroy(content);
    sc_json_destroy(parts);
    return status;
}

sc_status sc_provider_gemini_parse_response(sc_allocator *alloc,
                                            sc_str response_json,
                                            sc_provider_response *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini.response_invalid_argument");
    }

    if (contains_data_prefix(response_json)) {
        sc_status status = sc_status_ok();
        size_t offset = 0;
        sc_provider_response_init(out, alloc);
        while (sc_status_is_ok(status) && offset < response_json.len) {
            const char *line = response_json.ptr + offset;
            size_t line_len = 0;
            while (offset + line_len < response_json.len && line[line_len] != '\n') {
                line_len += 1;
            }
            if (line_len >= 5 && memcmp(line, "data:", 5) == 0) {
                sc_str payload = sc_str_from_parts(line + 5, line_len - 5);
                while (payload.len > 0 && (*payload.ptr == ' ' || *payload.ptr == '\t')) {
                    payload.ptr += 1;
                    payload.len -= 1;
                }
                if (payload.len >= 6 && memcmp(payload.ptr, "[DONE]", 6) == 0) {
                    out->streaming_complete = true;
                } else {
                    status = parse_gemini_json_response(alloc, payload, out);
                }
            }
            offset += line_len + (offset + line_len < response_json.len ? 1 : 0);
        }
        if (!sc_status_is_ok(status)) {
            sc_provider_response_clear(out);
        }
        return status;
    }

    sc_provider_response_init(out, alloc);
    sc_status status = parse_gemini_json_response(alloc, response_json, out);
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    return status;
}

sc_status sc_provider_gemini_parse_sse_event(sc_allocator *alloc,
                                             sc_str sse_event,
                                             sc_provider_stream_event *out)
{
    sc_provider_response response = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini.sse_invalid_argument");
    }
    *out = (sc_provider_stream_event){.struct_size = sizeof(*out), .type = SC_PROVIDER_STREAM_DELTA};
    status = sc_provider_gemini_parse_response(alloc, sse_event, &response);
    if (!sc_status_is_ok(status)) {
        out->type = SC_PROVIDER_STREAM_ERROR;
        return status;
    }
    if (response.pre_executed_tool_call.name.len > 0) {
        out->type = SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_CALL;
        status = sc_string_from_str(alloc,
                                    sc_string_as_str(&response.pre_executed_tool_call.call_id),
                                    &out->tool_call.call_id);
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc,
                                        sc_string_as_str(&response.pre_executed_tool_call.name),
                                        &out->tool_call.name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc,
                                        sc_string_as_str(&response.pre_executed_tool_call.arguments_json),
                                        &out->tool_call.arguments_json);
        }
        if (sc_status_is_ok(status) && response.pre_executed_tool_call.arguments != nullptr) {
            status = sc_json_clone(response.pre_executed_tool_call.arguments, alloc, &out->tool_call.arguments);
        }
    } else if (response.pre_executed_tool_result.len > 0) {
        out->type = SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_RESULT;
        status = sc_string_from_str(alloc, sc_string_as_str(&response.pre_executed_tool_result), &out->text);
    } else if (response.tool_calls.len > 0) {
        const sc_provider_tool_call *call = sc_vec_at_const(&response.tool_calls, 0);
        out->type = SC_PROVIDER_STREAM_TOOL_CALL;
        if (call != nullptr) {
            status = sc_string_from_str(alloc, sc_string_as_str(&call->call_id), &out->tool_call.call_id);
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, sc_string_as_str(&call->name), &out->tool_call.name);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, sc_string_as_str(&call->arguments_json), &out->tool_call.arguments_json);
            }
            if (sc_status_is_ok(status) && call->arguments != nullptr) {
                status = sc_json_clone(call->arguments, alloc, &out->tool_call.arguments);
            }
        }
    } else if (response.streaming_complete) {
        out->type = SC_PROVIDER_STREAM_DONE;
    } else {
        status = sc_string_from_str(alloc, sc_string_as_str(&response.text), &out->text);
    }
    sc_provider_response_clear(&response);
    if (!sc_status_is_ok(status)) {
        sc_provider_stream_event_clear(out);
    }
    return status;
}

sc_status sc_provider_ollama_build_request(sc_allocator *alloc,
                                           const sc_provider_options *options,
                                           const sc_provider_request *request,
                                           sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *messages = nullptr;
    sc_json_value *stream = nullptr;
    sc_string user_content = {0};
    sc_status status = sc_status_ok();

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_ollama.request_invalid_argument");
    }

    status = sc_provider_validate_request(&ollama_vtab, options, request);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &root);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(root, sc_str_from_cstr("model"), model_or_default(options, request));
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_bool_new(alloc, options != nullptr && options->streaming, &stream);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("stream"), stream);
        stream = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &messages);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->merge_system_into_user && request->system_instruction.len > 0) {
        sc_string_builder builder = {0};
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append(&builder, request->system_instruction);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n\n");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, request->prompt);
        }
        if (sc_status_is_ok(status) && request->media_context.len > 0) {
            status = sc_string_builder_append_cstr(&builder, "\n\n");
        }
        if (sc_status_is_ok(status) && request->media_context.len > 0) {
            status = sc_string_builder_append(&builder, request->media_context);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &user_content);
        } else {
            sc_string_builder_clear(&builder);
        }
    } else {
        if (sc_status_is_ok(status) && request->system_instruction.len > 0) {
            status = add_ollama_message(alloc, messages, sc_str_from_cstr("system"), request->system_instruction);
        }
        if (sc_status_is_ok(status) && request->media_context.len > 0) {
            sc_string_builder builder = {0};
            sc_string_builder_init(&builder, alloc);
            status = sc_string_builder_append(&builder, request->prompt);
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\n\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, request->media_context);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &user_content);
            } else {
                sc_string_builder_clear(&builder);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        sc_str content = user_content.len > 0 ? sc_string_as_str(&user_content) : request->prompt;
        status = add_ollama_message(alloc, messages, sc_str_from_cstr("user"), content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("messages"), messages);
        messages = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = add_ollama_options(alloc, root, options);
    }
    if (sc_status_is_ok(status)) {
        status = add_ollama_reasoning_controls(alloc, root, options);
    }
    if (sc_status_is_ok(status)) {
        status = add_ollama_format(alloc, root, options);
    }
    if (sc_status_is_ok(status)) {
        status = add_ollama_tools(alloc, root, request->tool_specs_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }

    sc_string_clear(&user_content);
    sc_json_destroy(root);
    sc_json_destroy(messages);
    sc_json_destroy(stream);
    return status;
}

sc_status sc_provider_ollama_parse_response(sc_allocator *alloc,
                                            sc_str response_json,
                                            sc_provider_response *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *message = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();
    double number = 0.0;
    sc_str text = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_ollama.response_invalid_argument");
    }
    status = sc_json_parse(alloc, response_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_json_object_get(root, sc_str_from_cstr("error")) != nullptr) {
        sc_json_destroy(root);
        return parse_gemini_error_status(alloc,
                                         response_json,
                                         "sc.provider_ollama.error_response",
                                         SC_ERR_HTTP);
    }

    sc_provider_response_init(out, alloc);
    if (sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("model")), &text) && text.len > 0) {
        status = sc_string_from_str(alloc, text, &out->model);
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(root, sc_str_from_cstr("prompt_eval_count")), &number)) {
        out->input_tokens = (int64_t)number;
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(root, sc_str_from_cstr("eval_count")), &number)) {
        out->output_tokens = (int64_t)number;
    }
    if (sc_status_is_ok(status)) {
        out->total_tokens = out->input_tokens + out->output_tokens;
    }
    if (sc_status_is_ok(status) && sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("done_reason")), &text) && text.len > 0) {
        status = sc_string_from_str(alloc, text, &out->finish_reason);
    }

    message = sc_json_object_get(root, sc_str_from_cstr("message"));
    if (sc_status_is_ok(status) && sc_json_as_str(sc_json_object_get(message, sc_str_from_cstr("content")), &text)) {
        status = sc_string_from_str(alloc, text, &out->text);
    } else if (sc_status_is_ok(status) && sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("response")), &text)) {
        status = sc_string_from_str(alloc, text, &out->text);
    }
    if (sc_status_is_ok(status) &&
        (sc_json_as_str(sc_json_object_get(message, sc_str_from_cstr("thinking")), &text) ||
         sc_json_as_str(sc_json_object_get(message, sc_str_from_cstr("reasoning")), &text) ||
         sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("thinking")), &text) ||
         sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("reasoning")), &text))) {
        status = sc_string_from_str(alloc, text, &out->reasoning_text);
    }
    if (sc_status_is_ok(status) && message != nullptr) {
        status = parse_ollama_tool_calls(alloc, message, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    sc_json_destroy(root);
    return status;
}

sc_status sc_provider_ollama_parse_stream(sc_allocator *alloc,
                                          sc_str stream_json,
                                          sc_provider_stream_callback callback,
                                          void *callback_user_data)
{
    size_t offset = 0;
    bool saw_done = false;
    sc_status status = sc_status_ok();

    if (callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_ollama.stream_invalid_argument");
    }
    while (sc_status_is_ok(status) && offset < stream_json.len) {
        size_t line_len = 0;
        sc_provider_response response = {0};
        sc_provider_stream_event event = {0};

        while (offset + line_len < stream_json.len && stream_json.ptr[offset + line_len] != '\n') {
            line_len += 1;
        }
        if (line_len > 0 && stream_json.ptr[offset + line_len - 1] == '\r') {
            line_len -= 1;
        }
        if (line_len == 0) {
            while (offset < stream_json.len && (stream_json.ptr[offset] == '\r' || stream_json.ptr[offset] == '\n')) {
                offset += 1;
            }
            continue;
        }

        status = sc_provider_ollama_parse_response(alloc,
                                                   sc_str_from_parts(stream_json.ptr + offset, line_len),
                                                   &response);
        if (sc_status_is_ok(status)) {
            status = emit_response_as_stream(alloc, &response, callback, callback_user_data);
        }
        if (sc_status_is_ok(status) && response.finish_reason.len > 0) {
            event = (sc_provider_stream_event){
                .struct_size = sizeof(event),
                .type = SC_PROVIDER_STREAM_FINAL_USAGE,
                .input_tokens = response.input_tokens,
                .output_tokens = response.output_tokens,
                .total_tokens = response.total_tokens,
                .cost_usd = response.cost_usd,
            };
            status = callback(callback_user_data, &event);
            if (sc_status_is_ok(status)) {
                sc_provider_stream_event_clear(&event);
                event = (sc_provider_stream_event){
                    .struct_size = sizeof(event),
                    .type = SC_PROVIDER_STREAM_DONE,
                };
                status = callback(callback_user_data, &event);
            }
            saw_done = sc_status_is_ok(status);
        }
        sc_provider_stream_event_clear(&event);
        sc_provider_response_clear(&response);
        offset += line_len;
        if (offset < stream_json.len && stream_json.ptr[offset] == '\r') {
            offset += 1;
        }
        if (offset < stream_json.len && stream_json.ptr[offset] == '\n') {
            offset += 1;
        }
    }
    if (sc_status_is_ok(status) && !saw_done) {
        sc_provider_stream_event event = {
            .struct_size = sizeof(event),
            .type = SC_PROVIDER_STREAM_DONE,
        };
        status = callback(callback_user_data, &event);
    }
    return status;
}

static sc_status http_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out)
{
#ifdef SC_HAVE_LIBCURL
    const http_provider *provider = impl;
    sc_string request_json = {0};
    sc_string response_json = {0};
    sc_string url = {0};
    int http_status = 0;
    sc_status status = sc_status_ok();

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    if (provider->kind == PROVIDER_ANTHROPIC) {
        status = sc_provider_anthropic_build_request(alloc, &provider->options, request, &request_json);
        if (sc_status_is_ok(status)) {
            status = build_anthropic_url(&provider->options, alloc, &url);
        }
        for (uint32_t attempt = 0;
             sc_status_is_ok(status) && attempt <= provider->options.max_retries;
             attempt += 1) {
            sc_string_clear(&response_json);
            status = provider_curl_json_post(&provider->options,
                                             sc_string_as_str(&url),
                                             sc_string_as_str(&request_json),
                                             AUTH_ANTHROPIC_KEY,
                                             alloc,
                                             &response_json,
                                             &http_status);
            if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
                status = parse_gemini_error_status(alloc,
                                                   sc_string_as_str(&response_json),
                                                   "sc.provider_anthropic.http_error",
                                                   SC_ERR_HTTP);
            }
            if (sc_status_is_ok(status) || !sc_provider_should_retry(status, http_status) || attempt == provider->options.max_retries) {
                break;
            }
            sc_status_clear(&status);
            status = sc_status_ok();
        }
        if (sc_status_is_ok(status)) {
            status = sc_provider_anthropic_parse_response(alloc, sc_string_as_str(&response_json), out);
        }
        if (sc_status_is_ok(status)) {
            apply_usage_cost(&provider->options, out);
        }
    } else if (provider->kind == PROVIDER_GEMINI) {
        sc_str model = model_or_default(&provider->options, request);
        if (provider->options.validate_model && !gemini_model_name_valid(model)) {
            status = sc_status_invalid_argument("sc.provider_gemini.model_invalid");
        } else {
            status = sc_provider_gemini_build_request(alloc, &provider->options, request, &request_json);
        }
        if (sc_status_is_ok(status)) {
            status = build_gemini_url(&provider->options, request, alloc, &url);
        }
        for (uint32_t attempt = 0;
             sc_status_is_ok(status) && attempt <= provider->options.max_retries;
             attempt += 1) {
            sc_string_clear(&response_json);
            status = provider_curl_json_post(&provider->options,
                                             sc_string_as_str(&url),
                                             sc_string_as_str(&request_json),
                                             AUTH_GEMINI_KEY,
                                             alloc,
                                             &response_json,
                                             &http_status);
            if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
                status = parse_gemini_error_status(alloc,
                                                   sc_string_as_str(&response_json),
                                                   "sc.provider_gemini.http_error",
                                                   SC_ERR_HTTP);
            }
            if (sc_status_is_ok(status) || !sc_provider_should_retry(status, http_status) || attempt == provider->options.max_retries) {
                break;
            }
            sc_status_clear(&status);
            status = sc_status_ok();
        }
        if (sc_status_is_ok(status)) {
            status = sc_provider_gemini_parse_response(alloc, sc_string_as_str(&response_json), out);
        }
        if (sc_status_is_ok(status)) {
            apply_usage_cost(&provider->options, out);
        }
    } else if (provider->kind == PROVIDER_OLLAMA) {
        status = sc_provider_ollama_build_request(alloc, &provider->options, request, &request_json);
        if (sc_status_is_ok(status)) {
            status = build_ollama_url(&provider->options, alloc, &url);
        }
        for (uint32_t attempt = 0;
             sc_status_is_ok(status) && attempt <= provider->options.max_retries;
             attempt += 1) {
            sc_string_clear(&response_json);
            status = provider_curl_json_post(&provider->options,
                                             sc_string_as_str(&url),
                                             sc_string_as_str(&request_json),
                                             AUTH_NONE,
                                             alloc,
                                             &response_json,
                                             &http_status);
            if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
                status = parse_gemini_error_status(alloc,
                                                   sc_string_as_str(&response_json),
                                                   "sc.provider_ollama.http_error",
                                                   SC_ERR_HTTP);
            }
            if (sc_status_is_ok(status) || !sc_provider_should_retry(status, http_status) || attempt == provider->options.max_retries) {
                break;
            }
            sc_status_clear(&status);
            status = sc_status_ok();
        }
        if (sc_status_is_ok(status)) {
            status = sc_provider_ollama_parse_response(alloc, sc_string_as_str(&response_json), out);
        }
        if (sc_status_is_ok(status)) {
            apply_usage_cost(&provider->options, out);
        }
    } else {
        status = sc_provider_bedrock_build_request(alloc, &provider->options, request, &request_json);
        if (sc_status_is_ok(status)) {
            status = build_bedrock_url(&provider->options, request, alloc, &url);
        }
        for (uint32_t attempt = 0;
             sc_status_is_ok(status) && attempt <= provider->options.max_retries;
             attempt += 1) {
            sc_string_clear(&response_json);
            status = provider_curl_json_post(&provider->options,
                                             sc_string_as_str(&url),
                                             sc_string_as_str(&request_json),
                                             AUTH_AWS_SIGV4,
                                             alloc,
                                             &response_json,
                                             &http_status);
            if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
                status = parse_gemini_error_status(alloc,
                                                   sc_string_as_str(&response_json),
                                                   "sc.provider_bedrock.http_error",
                                                   SC_ERR_HTTP);
            }
            if (sc_status_is_ok(status) || !sc_provider_should_retry(status, http_status) || attempt == provider->options.max_retries) {
                break;
            }
            sc_status_clear(&status);
            status = sc_status_ok();
        }
        if (sc_status_is_ok(status)) {
            status = sc_provider_bedrock_parse_response(alloc, sc_string_as_str(&response_json), out);
        }
        if (sc_status_is_ok(status)) {
            apply_usage_cost(&provider->options, out);
        }
    }

    sc_string_clear(&url);
    sc_string_clear(&response_json);
    sc_string_clear(&request_json);
    return status;
#else
    (void)impl;
    (void)request;
    (void)alloc;
    (void)out;
    return sc_status_unsupported("sc.provider_http.libcurl_unavailable");
#endif
}

static sc_status http_provider_stream(void *impl,
                                      const sc_provider_request *request,
                                      sc_allocator *alloc,
                                      sc_provider_stream_callback callback,
                                      void *callback_user_data)
{
#ifdef SC_HAVE_LIBCURL
    const http_provider *provider = impl;
    sc_provider_options options = {0};
    sc_string request_json = {0};
    sc_string url = {0};
    int http_status = 0;
    sc_status status = sc_status_ok();
    auth_kind auth = AUTH_NONE;
    provider_sse_stream_state sse_state = {0};
    ollama_stream_state ollama_state = {0};
    provider_stream_chunk_fn on_chunk = nullptr;
    provider_stream_finish_fn on_finish = nullptr;
    void *stream_user_data = nullptr;

    if (provider == nullptr || request == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.stream_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    options = provider->options;
    options.streaming = true;

    if (provider->kind == PROVIDER_BEDROCK) {
        return sc_status_unsupported("sc.provider_bedrock.streaming_unsupported");
    }
    if (provider->kind == PROVIDER_ANTHROPIC) {
        status = sc_provider_anthropic_build_request(alloc, &options, request, &request_json);
        if (sc_status_is_ok(status)) {
            status = build_anthropic_url(&options, alloc, &url);
        }
        auth = AUTH_ANTHROPIC_KEY;
    } else if (provider->kind == PROVIDER_GEMINI) {
        status = sc_provider_gemini_build_request(alloc, &options, request, &request_json);
        if (sc_status_is_ok(status)) {
            status = build_gemini_url(&options, request, alloc, &url);
        }
        auth = AUTH_GEMINI_KEY;
    } else {
        status = sc_provider_ollama_build_request(alloc, &options, request, &request_json);
        if (sc_status_is_ok(status)) {
            status = build_ollama_url(&options, alloc, &url);
        }
        auth = AUTH_NONE;
    }

    if (sc_status_is_ok(status) && provider->kind == PROVIDER_ANTHROPIC) {
        sse_state = (provider_sse_stream_state){
            .alloc = alloc,
            .max_partial_bytes = 64U * 1024U,
            .parse = sc_provider_anthropic_parse_sse_event,
            .callback = callback,
            .callback_user_data = callback_user_data,
        };
        on_chunk = provider_sse_stream_push;
        on_finish = provider_sse_stream_finish;
        stream_user_data = &sse_state;
    } else if (sc_status_is_ok(status) && provider->kind == PROVIDER_GEMINI) {
        sse_state = (provider_sse_stream_state){
            .alloc = alloc,
            .max_partial_bytes = 64U * 1024U,
            .parse = sc_provider_gemini_parse_sse_event,
            .callback = callback,
            .callback_user_data = callback_user_data,
        };
        on_chunk = provider_sse_stream_push;
        on_finish = provider_sse_stream_finish;
        stream_user_data = &sse_state;
    } else if (sc_status_is_ok(status)) {
        ollama_state = (ollama_stream_state){
            .alloc = alloc,
            .max_partial_bytes = 64U * 1024U,
            .callback = callback,
            .callback_user_data = callback_user_data,
        };
        on_chunk = ollama_stream_push;
        on_finish = ollama_stream_finish;
        stream_user_data = &ollama_state;
    }

    if (sc_status_is_ok(status)) {
        status = provider_curl_stream_post(&options,
                                           sc_string_as_str(&url),
                                           sc_string_as_str(&request_json),
                                           auth,
                                           on_chunk,
                                           on_finish,
                                           stream_user_data,
                                           &http_status);
    }
    if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
        status = sc_status_http("sc.provider_http.stream_http_error");
    }

    provider_sse_stream_clear(&sse_state);
    ollama_stream_clear(&ollama_state);
    sc_string_clear(&url);
    sc_string_clear(&request_json);
    return status;
#else
    (void)impl;
    (void)request;
    (void)alloc;
    (void)callback;
    (void)callback_user_data;
    return sc_status_unsupported("sc.provider_http.libcurl_unavailable");
#endif
}

static sc_status http_provider_generate_async(void *impl,
                                              sc_async_context *context,
                                              const sc_provider_request *request,
                                              sc_allocator *alloc,
                                              sc_provider_generate_complete_fn complete,
                                              void *complete_user_data,
                                              sc_async_op **out)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    http_provider *provider = impl;
    http_provider_async_state *state = nullptr;
    sc_status status = sc_status_ok();

    if (provider == nullptr || context == nullptr || request == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(http_provider_async_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (http_provider_async_state){
        .alloc = alloc,
        .provider = provider,
        .options = provider->options,
        .generate_complete = complete,
        .complete_user_data = complete_user_data,
    };
    sc_vec_init(&state->headers, alloc, sizeof(sc_http_header));
    status = http_provider_async_prepare(state, request);
    if (sc_status_is_ok(status)) {
        status = http_provider_async_build_headers(state);
    }
    if (sc_status_is_ok(status)) {
        status = http_provider_async_schedule(state, context, http_provider_async_generate_complete);
    }
    if (!sc_status_is_ok(status)) {
        complete(complete_user_data, nullptr, status);
        http_provider_async_state_destroy(state);
    }
    return sc_status_ok();
#else
    (void)impl;
    (void)context;
    (void)request;
    (void)alloc;
    (void)complete;
    (void)complete_user_data;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.provider_http.async_http_unavailable");
#endif
}

static sc_status http_provider_stream_async(void *impl,
                                            sc_async_context *context,
                                            const sc_provider_request *request,
                                            sc_allocator *alloc,
                                            sc_provider_stream_callback callback,
                                            void *callback_user_data,
                                            sc_provider_stream_complete_fn complete,
                                            void *complete_user_data,
                                            sc_async_op **out)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    http_provider *provider = impl;
    http_provider_async_state *state = nullptr;
    sc_status status = sc_status_ok();

    if (provider == nullptr || context == nullptr || request == nullptr || callback == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_stream_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    if (provider->kind == PROVIDER_BEDROCK) {
        complete(complete_user_data, sc_status_unsupported("sc.provider_bedrock.streaming_unsupported"));
        return sc_status_ok();
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(http_provider_async_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (http_provider_async_state){
        .alloc = alloc,
        .provider = provider,
        .options = provider->options,
        .stream_callback = callback,
        .stream_callback_user_data = callback_user_data,
        .stream_complete = complete,
        .complete_user_data = complete_user_data,
        .streaming = true,
    };
    state->options.streaming = true;
    sc_vec_init(&state->headers, alloc, sizeof(sc_http_header));
    status = http_provider_async_prepare_stream(state, request);
    if (sc_status_is_ok(status)) {
        status = http_provider_async_build_headers(state);
    }
    if (sc_status_is_ok(status)) {
        status = http_provider_async_schedule(state, context, http_provider_async_stream_complete);
    }
    if (!sc_status_is_ok(status)) {
        complete(complete_user_data, status);
        http_provider_async_state_destroy(state);
    }
    return sc_status_ok();
#else
    (void)impl;
    (void)context;
    (void)request;
    (void)alloc;
    (void)callback;
    (void)callback_user_data;
    (void)complete;
    (void)complete_user_data;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.provider_http.async_http_unavailable");
#endif
}

static sc_status http_provider_async_prepare(http_provider_async_state *state, const sc_provider_request *request)
{
    if (state == nullptr || state->provider == nullptr || request == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_invalid_argument");
    }
    if (state->provider->kind == PROVIDER_ANTHROPIC) {
        state->auth = AUTH_ANTHROPIC_KEY;
        sc_status status = sc_provider_anthropic_build_request(state->alloc, &state->options, request, &state->request_json);
        if (sc_status_is_ok(status)) {
            status = build_anthropic_url(&state->options, state->alloc, &state->url);
        }
        return status;
    }
    if (state->provider->kind == PROVIDER_GEMINI) {
        sc_str model = model_or_default(&state->options, request);
        if (state->options.validate_model && !gemini_model_name_valid(model)) {
            return sc_status_invalid_argument("sc.provider_gemini.model_invalid");
        }
        state->auth = AUTH_GEMINI_KEY;
        sc_status status = sc_provider_gemini_build_request(state->alloc, &state->options, request, &state->request_json);
        if (sc_status_is_ok(status)) {
            status = build_gemini_url(&state->options, request, state->alloc, &state->url);
        }
        return status;
    }
    if (state->provider->kind == PROVIDER_OLLAMA) {
        state->auth = AUTH_NONE;
        sc_status status = sc_provider_ollama_build_request(state->alloc, &state->options, request, &state->request_json);
        if (sc_status_is_ok(status)) {
            status = build_ollama_url(&state->options, state->alloc, &state->url);
        }
        return status;
    }
    state->auth = AUTH_AWS_SIGV4;
    sc_status status = sc_provider_bedrock_build_request(state->alloc, &state->options, request, &state->request_json);
    if (sc_status_is_ok(status)) {
        status = build_bedrock_url(&state->options, request, state->alloc, &state->url);
    }
    return status;
}

static sc_status http_provider_async_prepare_stream(http_provider_async_state *state, const sc_provider_request *request)
{
    sc_status status = http_provider_async_prepare(state, request);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (state->provider->kind == PROVIDER_ANTHROPIC) {
        state->sse_state = (provider_sse_stream_state){
            .alloc = state->alloc,
            .max_partial_bytes = 64U * 1024U,
            .parse = sc_provider_anthropic_parse_sse_event,
            .callback = state->stream_callback,
            .callback_user_data = state->stream_callback_user_data,
        };
    } else if (state->provider->kind == PROVIDER_GEMINI) {
        state->sse_state = (provider_sse_stream_state){
            .alloc = state->alloc,
            .max_partial_bytes = 64U * 1024U,
            .parse = sc_provider_gemini_parse_sse_event,
            .callback = state->stream_callback,
            .callback_user_data = state->stream_callback_user_data,
        };
    } else {
        state->ollama_state = (ollama_stream_state){
            .alloc = state->alloc,
            .max_partial_bytes = 64U * 1024U,
            .callback = state->stream_callback,
            .callback_user_data = state->stream_callback_user_data,
        };
    }
    return sc_status_ok();
}

static sc_status http_provider_async_build_headers(http_provider_async_state *state)
{
    sc_status status = sc_status_ok();

    if (state == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_header_invalid_argument");
    }
    status = http_provider_async_add_header(state, sc_str_from_cstr("Content-Type"), sc_str_from_cstr("application/json"));
    if (sc_status_is_ok(status) && state->auth != AUTH_NONE && state->auth != AUTH_AWS_SIGV4) {
        status = sc_provider_resolve_credential(state->alloc, &state->options, &state->credential);
    }
    if (sc_status_is_ok(status) && state->auth == AUTH_ANTHROPIC_KEY) {
        if (str_has_prefix(sc_string_as_str(&state->credential), "sk-ant-oat")) {
            sc_string_builder builder = {0};
            sc_string bearer = {0};
            sc_string_builder_init(&builder, state->alloc);
            status = sc_string_builder_append_cstr(&builder, "Bearer ");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&state->credential));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &bearer);
            } else {
                sc_string_builder_clear(&builder);
            }
            if (sc_status_is_ok(status)) {
                status = http_provider_async_add_header(state, sc_str_from_cstr("Authorization"), sc_string_as_str(&bearer));
            }
            sc_string_clear(&bearer);
        } else {
            status = http_provider_async_add_header(state, sc_str_from_cstr("x-api-key"), sc_string_as_str(&state->credential));
        }
        if (sc_status_is_ok(status)) {
            status = http_provider_async_add_header(state, sc_str_from_cstr("anthropic-version"), sc_str_from_cstr("2023-06-01"));
        }
    } else if (sc_status_is_ok(status) && state->auth == AUTH_GEMINI_KEY) {
        status = http_provider_async_add_header(state, sc_str_from_cstr("x-goog-api-key"), sc_string_as_str(&state->credential));
    } else if (sc_status_is_ok(status) && state->auth == AUTH_AWS_SIGV4) {
        sc_str region = state->options.region;
        sc_string_builder builder = {0};
        status = sc_provider_bedrock_resolve_credentials(state->alloc, &state->options, &state->aws_credentials);
        if (sc_status_is_ok(status)) {
            state->credential = state->aws_credentials.access_key_id;
            state->aws_credentials.access_key_id = (sc_string){0};
            state->aws_secret = state->aws_credentials.secret_access_key;
            state->aws_credentials.secret_access_key = (sc_string){0};
            state->aws_session_token = state->aws_credentials.session_token;
            state->aws_credentials.session_token = (sc_string){0};
        }
        if (region.len == 0) {
            const char *region_env = getenv("AWS_REGION");
            if (region_env == nullptr || region_env[0] == '\0') {
                region_env = getenv("AWS_DEFAULT_REGION");
            }
            region = region_env == nullptr || region_env[0] == '\0' ? sc_str_from_cstr("us-east-1") : sc_str_from_cstr(region_env);
        }
        sc_string_builder_init(&builder, state->alloc);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "aws:amz:");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, region);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ":bedrock");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &state->aws_sigv4);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status) && state->aws_session_token.len > 0) {
            status = http_provider_async_add_header(state,
                                                    sc_str_from_cstr("X-Amz-Security-Token"),
                                                    sc_string_as_str(&state->aws_session_token));
        }
    }
    return status;
}

static sc_status http_provider_async_add_header(http_provider_async_state *state, sc_str name, sc_str value)
{
    sc_string name_copy = {0};
    sc_string value_copy = {0};
    sc_http_header header;
    sc_status status = sc_status_ok();

    if (state == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_header_invalid_argument");
    }
    status = sc_string_from_str(state->alloc, name, &name_copy);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(state->alloc, value, &value_copy);
    }
    if (sc_status_is_ok(status)) {
        header = (sc_http_header){
            .name = sc_string_as_str(&name_copy),
            .value = sc_string_as_str(&value_copy),
        };
        status = sc_vec_push(&state->headers, &header);
    }
    if (sc_status_is_ok(status)) {
        name_copy = (sc_string){0};
        value_copy = (sc_string){0};
    }
    sc_string_clear(&name_copy);
    sc_string_clear(&value_copy);
    return status;
}

static sc_status http_provider_async_schedule(http_provider_async_state *state,
                                              sc_async_context *context,
                                              sc_http_complete_fn complete)
{
    sc_http_client *client = nullptr;
    sc_status status = sc_status_ok();

    if (state == nullptr || context == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_invalid_argument");
    }
    status = sc_async_context_http_client(context, &client);
    if (sc_status_is_ok(status)) {
        sc_http_request http_request = {
            .struct_size = sizeof(http_request),
            .method = sc_str_from_cstr("POST"),
            .url = sc_string_as_str(&state->url),
            .headers = state->headers.len == 0 ? nullptr : state->headers.ptr,
            .header_count = state->headers.len,
            .body = sc_string_as_str(&state->request_json),
            .user_agent = sc_str_from_cstr("smolclaw-c/0.1 provider-http"),
            .max_response_bytes = 8U * 1024U * 1024U,
            .timeout_ms = state->options.timeout_ms == 0 ? 30000 : state->options.timeout_ms,
            .connect_timeout_ms = state->options.timeout_ms == 0 || state->options.timeout_ms > 10000 ?
                10000 :
                state->options.timeout_ms,
            .follow_location = false,
            .allow_private_network = state->options.allow_loopback,
            .aws_sigv4 = sc_string_as_str(&state->aws_sigv4),
            .username = sc_string_as_str(&state->credential),
            .password = sc_string_as_str(&state->aws_secret),
            .on_chunk = state->streaming ? http_provider_async_stream_chunk : nullptr,
            .chunk_user_data = state,
        };
        status = sc_http_client_perform(client, &http_request, state->alloc, complete, state, &state->op);
    }
    return status;
}

static sc_status http_provider_async_stream_chunk(void *user_data, sc_buf chunk)
{
    http_provider_async_state *state = user_data;

    if (state == nullptr || chunk.ptr == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_stream_invalid_argument");
    }
    if (state->provider->kind == PROVIDER_OLLAMA) {
        return ollama_stream_push(&state->ollama_state, sc_str_from_parts((const char *)chunk.ptr, chunk.len));
    }
    return provider_sse_stream_push(&state->sse_state, sc_str_from_parts((const char *)chunk.ptr, chunk.len));
}

static void http_provider_async_generate_complete(void *user_data, const sc_http_response *response, sc_status status)
{
    http_provider_async_state *state = user_data;
    sc_provider_response provider_response = {0};
    sc_status final_status = status;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (response != nullptr && response->http_status >= 400) {
        sc_status_clear(&final_status);
        final_status = http_provider_async_parse_response(state,
                                                          sc_str_from_parts((const char *)response->body.ptr, response->body.len),
                                                          response->http_status,
                                                          &provider_response);
    } else if (sc_status_is_ok(final_status) && response != nullptr) {
        final_status = http_provider_async_parse_response(state,
                                                          sc_str_from_parts((const char *)response->body.ptr, response->body.len),
                                                          response->http_status,
                                                          &provider_response);
    }
    state->generate_complete(state->complete_user_data, sc_status_is_ok(final_status) ? &provider_response : nullptr, final_status);
    sc_provider_response_clear(&provider_response);
    http_provider_async_state_destroy(state);
}

static void http_provider_async_stream_complete(void *user_data, const sc_http_response *response, sc_status status)
{
    http_provider_async_state *state = user_data;
    sc_status final_status = status;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (sc_status_is_ok(final_status) && response != nullptr && (response->http_status < 200 || response->http_status >= 300)) {
        final_status = sc_status_http("sc.provider_http.stream_http_error");
    }
    if (sc_status_is_ok(final_status)) {
        final_status = state->provider->kind == PROVIDER_OLLAMA ? ollama_stream_finish(&state->ollama_state) :
                                                                  provider_sse_stream_finish(&state->sse_state);
    }
    state->stream_complete(state->complete_user_data, final_status);
    http_provider_async_state_destroy(state);
}

static sc_status http_provider_async_parse_response(http_provider_async_state *state,
                                                    sc_str response_json,
                                                    long http_status,
                                                    sc_provider_response *out)
{
    sc_status status = sc_status_ok();

    if (state == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.async_parse_invalid_argument");
    }
    if (http_status < 200 || http_status >= 300) {
        const char *fallback = state->provider->kind == PROVIDER_ANTHROPIC ? "sc.provider_anthropic.http_error" :
            (state->provider->kind == PROVIDER_GEMINI ? "sc.provider_gemini.http_error" :
                 (state->provider->kind == PROVIDER_OLLAMA ? "sc.provider_ollama.http_error" :
                                                              "sc.provider_bedrock.http_error"));
        return parse_gemini_error_status(state->alloc, response_json, fallback, SC_ERR_HTTP);
    }
    if (state->provider->kind == PROVIDER_ANTHROPIC) {
        status = sc_provider_anthropic_parse_response(state->alloc, response_json, out);
    } else if (state->provider->kind == PROVIDER_GEMINI) {
        status = sc_provider_gemini_parse_response(state->alloc, response_json, out);
    } else if (state->provider->kind == PROVIDER_OLLAMA) {
        status = sc_provider_ollama_parse_response(state->alloc, response_json, out);
    } else {
        status = sc_provider_bedrock_parse_response(state->alloc, response_json, out);
    }
    if (sc_status_is_ok(status)) {
        apply_usage_cost(&state->options, out);
    }
    return status;
}

static void http_provider_async_state_destroy(http_provider_async_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_http_op_destroy(state->op);
    http_provider_async_headers_clear(state);
    provider_sse_stream_clear(&state->sse_state);
    ollama_stream_clear(&state->ollama_state);
    sc_string_clear(&state->request_json);
    sc_string_clear(&state->url);
    sc_string_secure_clear(&state->credential);
    sc_string_secure_clear(&state->aws_secret);
    sc_string_secure_clear(&state->aws_session_token);
    sc_string_clear(&state->aws_sigv4);
    sc_provider_bedrock_credentials_clear(&state->aws_credentials);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(http_provider_async_state));
}

static void http_provider_async_headers_clear(http_provider_async_state *state)
{
    if (state == nullptr) {
        return;
    }
    for (size_t i = 0; i < state->headers.len; i += 1) {
        const sc_http_header *header = sc_vec_at(&state->headers, i);
        sc_string name = {.ptr = header == nullptr ? nullptr : (char *)header->name.ptr,
                          .len = header == nullptr ? 0 : header->name.len,
                          .alloc = state->alloc};
        sc_string value = {.ptr = header == nullptr ? nullptr : (char *)header->value.ptr,
                           .len = header == nullptr ? 0 : header->value.len,
                           .alloc = state->alloc};
        sc_string_clear(&name);
        sc_string_clear(&value);
    }
    sc_vec_clear(&state->headers);
}

static void http_provider_destroy(void *impl)
{
    http_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->provider_name);
    sc_string_clear(&provider->base_url);
    sc_string_secure_clear(&provider->api_key);
    sc_string_clear(&provider->credential_env);
    sc_string_clear(&provider->generic_credential_env);
    sc_string_secure_clear(&provider->secret_value);
    sc_string_secure_clear(&provider->session_token);
    sc_string_clear(&provider->default_model);
    sc_string_clear(&provider->generation_config_json);
    sc_string_clear(&provider->safety_settings_json);
    sc_string_clear(&provider->reasoning_effort);
    sc_string_clear(&provider->options_json);
    sc_string_clear(&provider->format_json);
    sc_string_clear(&provider->region);
    sc_string_clear(&provider->command);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(http_provider));
}

static sc_status http_provider_new(sc_allocator *alloc,
                                   provider_kind kind,
                                   const sc_provider_options *options,
                                   const sc_provider_vtab *vtab,
                                   sc_provider **out)
{
    http_provider *impl = nullptr;
    sc_provider_options resolved = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    resolved = *options;
    if (kind == PROVIDER_ANTHROPIC) {
        if (resolved.provider_name.ptr == nullptr || resolved.provider_name.len == 0) {
            resolved.provider_name = sc_str_from_cstr("anthropic");
        }
        if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
            resolved.base_url = sc_str_from_cstr("https://api.anthropic.com/v1/messages");
        }
    } else if (kind == PROVIDER_GEMINI) {
        if (resolved.provider_name.ptr == nullptr || resolved.provider_name.len == 0) {
            resolved.provider_name = sc_str_from_cstr("gemini");
        }
        if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
            resolved.base_url = sc_str_from_cstr("https://generativelanguage.googleapis.com/v1beta/models");
        }
    } else if (kind == PROVIDER_OLLAMA) {
        if (resolved.provider_name.ptr == nullptr || resolved.provider_name.len == 0) {
            resolved.provider_name = sc_str_from_cstr("ollama");
        }
        if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
            resolved.base_url = sc_str_from_cstr("http://127.0.0.1:11434/api/chat");
        }
    } else {
        if (resolved.provider_name.ptr == nullptr || resolved.provider_name.len == 0) {
            resolved.provider_name = sc_str_from_cstr("bedrock");
        }
        if (resolved.base_url.ptr == nullptr || resolved.base_url.len == 0) {
            resolved.base_url = sc_str_from_cstr("https://bedrock-runtime.us-east-1.amazonaws.com");
        }
    }
    if (!sc_provider_url_allowed(resolved.base_url, kind == PROVIDER_OLLAMA)) {
        return sc_status_security_denied("sc.provider_http.url_denied");
    }

    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(http_provider));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (http_provider){.alloc = alloc, .kind = kind};
    status = copy_options(impl, &resolved);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        http_provider_destroy(impl);
    }
    return status;
}

static sc_status copy_options(http_provider *provider, const sc_provider_options *options)
{
    sc_status status = sc_string_from_str(provider->alloc, options->provider_name, &provider->provider_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->base_url, &provider->base_url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->api_key, &provider->api_key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->credential_env, &provider->credential_env);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->generic_credential_env, &provider->generic_credential_env);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->secret_value, &provider->secret_value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->session_token, &provider->session_token);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->default_model, &provider->default_model);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->generation_config_json, &provider->generation_config_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->safety_settings_json, &provider->safety_settings_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->reasoning_effort, &provider->reasoning_effort);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->options_json, &provider->options_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->format_json, &provider->format_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->region, &provider->region);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->command, &provider->command);
    }
    if (sc_status_is_ok(status)) {
        provider->options = *options;
        provider->options.provider_name = string_as_option(&provider->provider_name);
        provider->options.base_url = string_as_option(&provider->base_url);
        provider->options.api_key = string_as_option(&provider->api_key);
        provider->options.credential_env = string_as_option(&provider->credential_env);
        provider->options.generic_credential_env = string_as_option(&provider->generic_credential_env);
        provider->options.secret_value = string_as_option(&provider->secret_value);
        provider->options.session_token = string_as_option(&provider->session_token);
        provider->options.default_model = string_as_option(&provider->default_model);
        provider->options.generation_config_json = string_as_option(&provider->generation_config_json);
        provider->options.safety_settings_json = string_as_option(&provider->safety_settings_json);
        provider->options.reasoning_effort = string_as_option(&provider->reasoning_effort);
        provider->options.options_json = string_as_option(&provider->options_json);
        provider->options.format_json = string_as_option(&provider->format_json);
        provider->options.region = string_as_option(&provider->region);
        provider->options.command = string_as_option(&provider->command);
    }
    return status;
}

static sc_str string_as_option(const sc_string *string)
{
    if (string == nullptr || string->ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    return sc_string_as_str(string);
}

static sc_str model_or_default(const sc_provider_options *options, const sc_provider_request *request)
{
    if (request != nullptr && request->model.ptr != nullptr && request->model.len > 0) {
        return request->model;
    }
    if (options != nullptr && options->default_model.ptr != nullptr && options->default_model.len > 0) {
        return options->default_model;
    }
    return sc_str_from_cstr("");
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

static sc_status set_raw_json(sc_allocator *alloc, sc_json_value *object, sc_str key, sc_str raw_json)
{
    sc_json_value *value = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (raw_json.ptr == nullptr || raw_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, raw_json, &value, &error);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(object, key, value);
        value = nullptr;
    }
    sc_json_destroy(value);
    return status;
}

static sc_status add_anthropic_tools(sc_allocator *alloc, sc_json_value *root, sc_str tool_specs_json)
{
    sc_json_value *openai_specs = nullptr;
    sc_json_value *tools = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (tool_specs_json.ptr == nullptr || tool_specs_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, tool_specs_json, &openai_specs, &error);
    if (sc_status_is_ok(status) && sc_json_array_len(openai_specs) == 0) {
        sc_json_destroy(openai_specs);
        return sc_status_ok();
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &tools);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(openai_specs); i += 1) {
        sc_json_value *spec = sc_json_array_get(openai_specs, i);
        sc_json_value *function = sc_json_object_get(spec, sc_str_from_cstr("function"));
        sc_json_value *tool = nullptr;
        sc_json_value *name = nullptr;
        sc_json_value *description = nullptr;
        sc_json_value *input_schema = nullptr;
        if (function == nullptr) {
            continue;
        }
        status = sc_json_object_new(alloc, &tool);
        if (sc_status_is_ok(status)) {
            status = sc_json_clone(sc_json_object_get(function, sc_str_from_cstr("name")), alloc, &name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(tool, sc_str_from_cstr("name"), name);
            name = nullptr;
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_clone(sc_json_object_get(function, sc_str_from_cstr("description")), alloc, &description);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(tool, sc_str_from_cstr("description"), description);
            description = nullptr;
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_clone(sc_json_object_get(function, sc_str_from_cstr("parameters")), alloc, &input_schema);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(tool, sc_str_from_cstr("input_schema"), input_schema);
            input_schema = nullptr;
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_array_append(tools, tool);
            tool = nullptr;
        }
        sc_json_destroy(tool);
        sc_json_destroy(name);
        sc_json_destroy(description);
        sc_json_destroy(input_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("tools"), tools);
        tools = nullptr;
    }
    sc_json_destroy(openai_specs);
    sc_json_destroy(tools);
    return status;
}

static sc_status append_gemini_part(sc_allocator *alloc, sc_json_value *parts, sc_json_value *part)
{
    (void)alloc;
    if (parts == nullptr || part == nullptr) {
        sc_json_destroy(part);
        return sc_status_invalid_argument("sc.provider_gemini.part_invalid_argument");
    }
    sc_status status = sc_json_array_append(parts, part);
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(part);
    }
    return status;
}

static sc_status append_gemini_text_part(sc_allocator *alloc, sc_json_value *parts, sc_str text)
{
    sc_json_value *part = nullptr;
    sc_status status = sc_status_ok();

    if (text.ptr == nullptr || text.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_object_new(alloc, &part);
    if (sc_status_is_ok(status)) {
        status = set_string(part, sc_str_from_cstr("text"), text);
    }
    if (sc_status_is_ok(status)) {
        status = append_gemini_part(alloc, parts, part);
        part = nullptr;
    }
    sc_json_destroy(part);
    return status;
}

static sc_status append_gemini_media_part(sc_allocator *alloc, sc_json_value *parts, const sc_media_attachment *attachment)
{
    sc_json_value *part = nullptr;
    sc_json_value *inline_data = nullptr;
    sc_string encoded = {0};
    sc_status status = sc_status_ok();

    if (attachment == nullptr || attachment->redacted) {
        return sc_status_ok();
    }
    if (attachment->storage_kind == SC_MEDIA_STORAGE_BYTES && attachment->bytes.ptr != nullptr && attachment->bytes.len > 0) {
        status = base64_encode(alloc, attachment->bytes, &encoded);
        if (sc_status_is_ok(status)) {
            status = sc_json_object_new(alloc, &part);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_new(alloc, &inline_data);
        }
        if (sc_status_is_ok(status)) {
            status = set_string(inline_data,
                                sc_str_from_cstr("mimeType"),
                                attachment->content_type.len > 0 ? attachment->content_type : sc_str_from_cstr("application/octet-stream"));
        }
        if (sc_status_is_ok(status)) {
            status = set_string(inline_data, sc_str_from_cstr("data"), sc_string_as_str(&encoded));
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(part, sc_str_from_cstr("inlineData"), inline_data);
            inline_data = nullptr;
        }
        if (sc_status_is_ok(status)) {
            status = append_gemini_part(alloc, parts, part);
            part = nullptr;
        }
    } else {
        sc_string_builder builder = {0};
        sc_string text = {0};
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "Attached media: ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder,
                                              attachment->filename.len > 0 ? attachment->filename : attachment->attachment_id);
        }
        if (sc_status_is_ok(status) && attachment->content_type.len > 0) {
            status = sc_string_builder_append_cstr(&builder, " (");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, attachment->content_type);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, ")");
            }
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &text);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status)) {
            status = append_gemini_text_part(alloc, parts, sc_string_as_str(&text));
        }
        sc_string_clear(&text);
    }

    sc_string_clear(&encoded);
    sc_json_destroy(part);
    sc_json_destroy(inline_data);
    return status;
}

static sc_status base64_encode(sc_allocator *alloc, sc_buf input, sc_string *out)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sc_string result = {0};
    size_t groups = 0;
    size_t encoded_len = 0;
    size_t alloc_len = 0;
    size_t cursor = 0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini.base64_invalid_argument");
    }
    if (input.len > 0 && input.ptr == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini.base64_invalid_argument");
    }
    if (alloc == nullptr) {
        alloc = sc_allocator_heap();
    }
    if (sc_size_add_overflow(input.len, 2, &groups) ||
        sc_size_mul_overflow(groups / 3, 4, &encoded_len) ||
        sc_size_add_overflow(encoded_len, 1, &alloc_len)) {
        return sc_status_no_memory();
    }

    result.ptr = sc_alloc(alloc, alloc_len, _Alignof(char));
    if (result.ptr == nullptr) {
        return sc_status_no_memory();
    }
    result.len = encoded_len;
    result.alloc = alloc;

    for (size_t i = 0; i < input.len; i += 3) {
        unsigned int a = input.ptr[i];
        unsigned int b = i + 1 < input.len ? input.ptr[i + 1] : 0;
        unsigned int c = i + 2 < input.len ? input.ptr[i + 2] : 0;
        result.ptr[cursor] = alphabet[(a >> 2u) & 0x3Fu];
        result.ptr[cursor + 1] = alphabet[((a & 0x03u) << 4u) | ((b >> 4u) & 0x0Fu)];
        result.ptr[cursor + 2] = i + 1 < input.len ? alphabet[((b & 0x0Fu) << 2u) | ((c >> 6u) & 0x03u)] : '=';
        result.ptr[cursor + 3] = i + 2 < input.len ? alphabet[c & 0x3Fu] : '=';
        cursor += 4;
    }
    result.ptr[result.len] = '\0';
    *out = result;
    return sc_status_ok();
}

static sc_status add_gemini_system_instruction(sc_allocator *alloc,
                                               sc_json_value *root,
                                               const sc_provider_options *options,
                                               const sc_provider_request *request)
{
    sc_json_value *instruction = nullptr;
    sc_json_value *parts = nullptr;
    sc_status status = sc_status_ok();
    sc_str system = request == nullptr ? sc_str_from_cstr("") : request->system_instruction;

    if (options != nullptr && options->merge_system_into_user) {
        return sc_status_ok();
    }
    if (system.ptr == nullptr || system.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_object_new(alloc, &instruction);
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &parts);
    }
    if (sc_status_is_ok(status)) {
        status = append_gemini_text_part(alloc, parts, system);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(instruction, sc_str_from_cstr("parts"), parts);
        parts = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("systemInstruction"), instruction);
        instruction = nullptr;
    }
    sc_json_destroy(instruction);
    sc_json_destroy(parts);
    return status;
}

static sc_status add_gemini_generation_config(sc_allocator *alloc,
                                              sc_json_value *root,
                                              const sc_provider_options *options)
{
    sc_json_value *config = nullptr;
    sc_status status = sc_status_ok();
    bool any = false;

    if (options == nullptr) {
        return sc_status_ok();
    }
    if (options->generation_config_json.len > 0) {
        return set_raw_json(alloc, root, sc_str_from_cstr("generationConfig"), options->generation_config_json);
    }
    status = sc_json_object_new(alloc, &config);
    if (sc_status_is_ok(status) && options->temperature > 0.0) {
        status = set_number(config, sc_str_from_cstr("temperature"), options->temperature);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options->top_p > 0.0) {
        status = set_number(config, sc_str_from_cstr("topP"), options->top_p);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options->top_k > 0) {
        status = set_number(config, sc_str_from_cstr("topK"), (double)options->top_k);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options->max_output_tokens > 0) {
        status = set_number(config, sc_str_from_cstr("maxOutputTokens"), (double)options->max_output_tokens);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && any) {
        status = sc_json_object_set(root, sc_str_from_cstr("generationConfig"), config);
        config = nullptr;
    }
    sc_json_destroy(config);
    return status;
}

static sc_status add_gemini_tools(sc_allocator *alloc, sc_json_value *root, sc_str tool_specs_json)
{
    sc_json_value *openai_specs = nullptr;
    sc_json_value *tools = nullptr;
    sc_json_value *tool = nullptr;
    sc_json_value *declarations = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (tool_specs_json.ptr == nullptr || tool_specs_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, tool_specs_json, &openai_specs, &error);
    if (sc_status_is_ok(status) && sc_json_array_len(openai_specs) == 0) {
        sc_json_destroy(openai_specs);
        return sc_status_ok();
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &tools);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &declarations);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(openai_specs); i += 1) {
        sc_json_value *spec = sc_json_array_get(openai_specs, i);
        sc_json_value *function = sc_json_object_get(spec, sc_str_from_cstr("function"));
        sc_json_value *copy = nullptr;
        if (function != nullptr) {
            status = sc_json_clone(function, alloc, &copy);
            if (sc_status_is_ok(status)) {
                status = sc_json_array_append(declarations, copy);
                copy = nullptr;
            }
        }
        sc_json_destroy(copy);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(tool, sc_str_from_cstr("functionDeclarations"), declarations);
        declarations = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(tools, tool);
        tool = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("tools"), tools);
        tools = nullptr;
    }
    sc_json_destroy(openai_specs);
    sc_json_destroy(tools);
    sc_json_destroy(tool);
    sc_json_destroy(declarations);
    return status;
}

static sc_status add_ollama_options(sc_allocator *alloc, sc_json_value *root, const sc_provider_options *options)
{
    sc_json_value *object = nullptr;
    sc_status status = sc_status_ok();
    bool any = false;

    if (options == nullptr) {
        return sc_status_ok();
    }
    if (options->options_json.len > 0) {
        return set_raw_json(alloc, root, sc_str_from_cstr("options"), options->options_json);
    }
    if (options->generation_config_json.len > 0) {
        return set_raw_json(alloc, root, sc_str_from_cstr("options"), options->generation_config_json);
    }

    status = sc_json_object_new(alloc, &object);
    if (sc_status_is_ok(status) && options->temperature > 0.0) {
        status = set_number(object, sc_str_from_cstr("temperature"), options->temperature);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options->top_p > 0.0) {
        status = set_number(object, sc_str_from_cstr("top_p"), options->top_p);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options->top_k > 0) {
        status = set_number(object, sc_str_from_cstr("top_k"), (double)options->top_k);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && options->max_output_tokens > 0) {
        status = set_number(object, sc_str_from_cstr("num_predict"), (double)options->max_output_tokens);
        any = sc_status_is_ok(status);
    }
    if (sc_status_is_ok(status) && any) {
        status = sc_json_object_set(root, sc_str_from_cstr("options"), object);
        object = nullptr;
    }
    sc_json_destroy(object);
    return status;
}

static sc_status add_ollama_reasoning_controls(sc_allocator *alloc, sc_json_value *root, const sc_provider_options *options)
{
    sc_json_value *think = nullptr;
    sc_status status = sc_status_ok();
    sc_str effort = reasoning_effort_option(options);
    bool disable_think = options != nullptr && options->thinking_level == SC_PROVIDER_THINKING_DISABLED;

    if (options == nullptr) {
        return sc_status_ok();
    }
    if (options->think_set || disable_think) {
        status = sc_json_bool_new(alloc, options->think_set ? options->think : false, &think);
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(root, sc_str_from_cstr("think"), think);
            think = nullptr;
        }
    }
    if (sc_status_is_ok(status) && effort.len > 0) {
        status = set_string(root, sc_str_from_cstr("reasoning_effort"), effort);
    }
    sc_json_destroy(think);
    return status;
}

static sc_status add_ollama_format(sc_allocator *alloc, sc_json_value *root, const sc_provider_options *options)
{
    sc_json_value *format = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (options == nullptr || options->format_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, options->format_json, &format, &error);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return set_string(root, sc_str_from_cstr("format"), options->format_json);
    }
    status = sc_json_object_set(root, sc_str_from_cstr("format"), format);
    if (sc_status_is_ok(status)) {
        format = nullptr;
    }
    sc_json_destroy(format);
    return status;
}

static sc_status add_ollama_message(sc_allocator *alloc, sc_json_value *messages, sc_str role, sc_str content)
{
    sc_json_value *message = nullptr;
    sc_status status = sc_status_ok();

    if (messages == nullptr || content.ptr == nullptr || content.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_object_new(alloc, &message);
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("role"), role);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("content"), content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(messages, message);
        message = nullptr;
    }
    sc_json_destroy(message);
    return status;
}

static sc_status add_ollama_tools(sc_allocator *alloc, sc_json_value *root, sc_str tool_specs_json)
{
    sc_json_value *tools = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();

    if (tool_specs_json.ptr == nullptr || tool_specs_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, tool_specs_json, &tools, &error);
    if (sc_status_is_ok(status) && sc_json_array_len(tools) == 0) {
        sc_json_destroy(tools);
        return sc_status_ok();
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("tools"), tools);
        tools = nullptr;
    }
    sc_json_destroy(tools);
    return status;
}

static sc_status append_text_parts(sc_allocator *alloc, sc_json_value *parts, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    size_t total_len = 0;
    size_t reserve_len = 0;

    if (out == nullptr || parts == nullptr) {
        return sc_status_parse("sc.provider_http.missing_text");
    }
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(parts); i += 1) {
        sc_json_value *part = sc_json_array_get(parts, i);
        sc_str text = {0};
        if (sc_json_as_str(sc_json_object_get(part, sc_str_from_cstr("text")), &text)) {
            if (sc_size_add_overflow(total_len, text.len, &total_len)) {
                status = sc_status_no_memory();
            }
        }
    }
    if (sc_status_is_ok(status) && total_len > 0) {
        status = sc_size_add_overflow(total_len, 1, &reserve_len) ? sc_status_no_memory() :
                                                                    sc_bytes_reserve(&builder.bytes, reserve_len);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(parts); i += 1) {
        sc_json_value *part = sc_json_array_get(parts, i);
        sc_str text = {0};
        if (sc_json_as_str(sc_json_object_get(part, sc_str_from_cstr("text")), &text)) {
            status = sc_string_builder_append(&builder, text);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status emit_response_as_stream(sc_allocator *alloc,
                                         const sc_provider_response *response,
                                         sc_provider_stream_callback callback,
                                         void *callback_user_data)
{
    sc_provider_stream_event event = {0};
    sc_status status = sc_status_ok();

    if (response == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_stream.response_invalid_argument");
    }
    if (response->reasoning_text.len > 0) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_REASONING_DELTA};
        status = sc_string_from_str(alloc, sc_string_as_str(&response->reasoning_text), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    if (response->text.len > 0) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DELTA};
        status = sc_string_from_str(alloc, sc_string_as_str(&response->text), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < response->tool_calls.len; i += 1) {
        const sc_provider_tool_call *call = sc_vec_at_const(&response->tool_calls, i);
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_TOOL_CALL};
        if (call != nullptr) {
            status = sc_string_from_str(alloc, sc_string_as_str(&call->call_id), &event.tool_call.call_id);
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, sc_string_as_str(&call->name), &event.tool_call.name);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, sc_string_as_str(&call->arguments_json), &event.tool_call.arguments_json);
            }
            if (sc_status_is_ok(status) && call->arguments != nullptr) {
                status = sc_json_clone(call->arguments, alloc, &event.tool_call.arguments);
            }
        }
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    if (sc_status_is_ok(status) && response->pre_executed_tool_call.name.len > 0) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_CALL};
        status = sc_string_from_str(alloc,
                                    sc_string_as_str(&response->pre_executed_tool_call.call_id),
                                    &event.tool_call.call_id);
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc,
                                        sc_string_as_str(&response->pre_executed_tool_call.name),
                                        &event.tool_call.name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc,
                                        sc_string_as_str(&response->pre_executed_tool_call.arguments_json),
                                        &event.tool_call.arguments_json);
        }
        if (sc_status_is_ok(status) && response->pre_executed_tool_call.arguments != nullptr) {
            status = sc_json_clone(response->pre_executed_tool_call.arguments, alloc, &event.tool_call.arguments);
        }
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    if (sc_status_is_ok(status) && response->pre_executed_tool_result.len > 0) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_RESULT};
        status = sc_string_from_str(alloc, sc_string_as_str(&response->pre_executed_tool_result), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    return status;
}

static sc_status sse_event_payload(sc_allocator *alloc, sc_str sse_event, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    size_t pos = 0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_sse.payload_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    while (sc_status_is_ok(status) && pos < sse_event.len) {
        size_t start = pos;
        size_t len = 0;
        while (pos < sse_event.len && sse_event.ptr[pos] != '\n') {
            pos += 1;
        }
        len = pos - start;
        if (len > 0 && sse_event.ptr[start + len - 1] == '\r') {
            len -= 1;
        }
        if (len >= 5 && memcmp(sse_event.ptr + start, "data:", 5) == 0) {
            size_t data_start = start + 5;
            if (data_start < start + len && sse_event.ptr[data_start] == ' ') {
                data_start += 1;
            }
            if (builder.bytes.len > 0) {
                status = sc_string_builder_append_cstr(&builder, "\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder,
                                                  sc_str_from_parts(sse_event.ptr + data_start, start + len - data_start));
            }
        }
        if (pos < sse_event.len && sse_event.ptr[pos] == '\n') {
            pos += 1;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status parse_gemini_json_response(sc_allocator *alloc, sc_str response_json, sc_provider_response *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *candidates = nullptr;
    sc_json_value *candidate = nullptr;
    sc_json_value *content = nullptr;
    sc_json_value *parts = nullptr;
    sc_json_value *usage = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_status_ok();
    double number = 0.0;
    sc_str text = {0};

    status = sc_json_parse(alloc, response_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_json_object_get(root, sc_str_from_cstr("error")) != nullptr) {
        sc_json_destroy(root);
        return parse_gemini_error_status(alloc,
                                         response_json,
                                         "sc.provider_gemini.error_response",
                                         SC_ERR_HTTP);
    }

    if (sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("modelVersion")), &text) && text.len > 0) {
        sc_string_clear(&out->model);
        status = sc_string_from_str(alloc, text, &out->model);
    }
    usage = sc_json_object_get(root, sc_str_from_cstr("usageMetadata"));
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("promptTokenCount")), &number)) {
        out->input_tokens += (int64_t)number;
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("candidatesTokenCount")), &number)) {
        out->output_tokens += (int64_t)number;
    }
    if (sc_status_is_ok(status) && sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("totalTokenCount")), &number)) {
        out->total_tokens += (int64_t)number;
    } else if (sc_status_is_ok(status)) {
        out->total_tokens = out->input_tokens + out->output_tokens;
    }
    if (sc_status_is_ok(status)) {
        status = parse_gemini_grounding(alloc, root, out);
    }

    candidates = sc_json_object_get(root, sc_str_from_cstr("candidates"));
    candidate = sc_json_array_get(candidates, 0);
    if (sc_status_is_ok(status) && sc_json_as_str(sc_json_object_get(candidate, sc_str_from_cstr("finishReason")), &text) && text.len > 0) {
        sc_string_clear(&out->finish_reason);
        status = sc_string_from_str(alloc, text, &out->finish_reason);
    }
    content = sc_json_object_get(candidate, sc_str_from_cstr("content"));
    parts = sc_json_object_get(content, sc_str_from_cstr("parts"));
    if (sc_status_is_ok(status) && parts == nullptr) {
        status = sc_status_parse("sc.provider_gemini.missing_parts");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(parts); i += 1) {
        status = parse_gemini_part(alloc, sc_json_array_get(parts, i), out, i);
    }
    sc_json_destroy(root);
    return status;
}

static sc_status parse_gemini_part(sc_allocator *alloc,
                                   sc_json_value *part,
                                   sc_provider_response *out,
                                   size_t index)
{
    sc_json_value *function_call = nullptr;
    sc_json_value *args = nullptr;
    sc_provider_tool_call call = {0};
    sc_string args_json = {0};
    sc_json_value *owned_empty_args = nullptr;
    sc_status status = sc_status_ok();
    sc_str text = {0};
    sc_str name = {0};

    if (part == nullptr) {
        return sc_status_ok();
    }
    if (sc_json_as_str(sc_json_object_get(part, sc_str_from_cstr("text")), &text)) {
        sc_string_builder builder = {0};
        sc_string combined = {0};
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append(&builder, sc_string_as_str(&out->text));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, text);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &combined);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status)) {
            sc_string_clear(&out->text);
            out->text = combined;
        }
        return status;
    }

    function_call = sc_json_object_get(part, sc_str_from_cstr("functionCall"));
    if (function_call == nullptr) {
        return sc_status_ok();
    }
    if (!sc_json_as_str(sc_json_object_get(function_call, sc_str_from_cstr("name")), &name) || name.len == 0) {
        return sc_status_parse("sc.provider_gemini.tool_call_missing_name");
    }
    args = sc_json_object_get(function_call, sc_str_from_cstr("args"));
    if (args == nullptr) {
        status = sc_json_object_new(alloc, &owned_empty_args);
        args = owned_empty_args;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(args, alloc, &args_json);
    }
    if (sc_status_is_ok(status)) {
        char call_id[64] = {0};
        (void)snprintf(call_id, sizeof(call_id), "gemini-call-%zu", index + 1);
        call.struct_size = sizeof(call);
        status = sc_string_from_cstr(alloc, call_id, &call.call_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, name, &call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&args_json), &call.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_parse(alloc, sc_string_as_str(&args_json), &call.arguments, nullptr);
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_response_add_tool_call(out, &call);
    }
    sc_provider_tool_call_clear(&call);
    sc_string_clear(&args_json);
    sc_json_destroy(owned_empty_args);
    return status;
}

static sc_status parse_gemini_grounding(sc_allocator *alloc, sc_json_value *root, sc_provider_response *out)
{
    sc_json_value *candidates = sc_json_object_get(root, sc_str_from_cstr("candidates"));
    sc_json_value *candidate = sc_json_array_get(candidates, 0);
    sc_json_value *grounding = sc_json_object_get(candidate, sc_str_from_cstr("groundingMetadata"));
    sc_json_value *queries = sc_json_object_get(grounding, sc_str_from_cstr("webSearchQueries"));
    sc_json_value *chunks = sc_json_object_get(grounding, sc_str_from_cstr("groundingChunks"));
    sc_json_value *arguments = nullptr;
    sc_json_value *query_value = nullptr;
    sc_string arguments_json = {0};
    sc_string result_json = {0};
    sc_str query = {0};
    sc_status status = sc_status_ok();

    if (grounding == nullptr || out == nullptr || sc_json_array_len(queries) == 0) {
        return sc_status_ok();
    }
    if (!sc_json_as_str(sc_json_array_get(queries, 0), &query)) {
        query = sc_str_from_cstr("");
    }
    status = sc_json_object_new(alloc, &arguments);
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(alloc, query, &query_value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(arguments, sc_str_from_cstr("query"), query_value);
        query_value = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(arguments, alloc, &arguments_json);
    }
    if (sc_status_is_ok(status)) {
        sc_provider_tool_call_clear(&out->pre_executed_tool_call);
        out->pre_executed_tool_call.struct_size = sizeof(out->pre_executed_tool_call);
        status = sc_string_from_cstr(alloc, "gemini-grounded-search", &out->pre_executed_tool_call.call_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, "google_search", &out->pre_executed_tool_call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&arguments_json), &out->pre_executed_tool_call.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_clone(arguments, alloc, &out->pre_executed_tool_call.arguments);
    }
    if (sc_status_is_ok(status) && chunks != nullptr) {
        status = sc_json_serialize(chunks, alloc, &result_json);
    }
    if (sc_status_is_ok(status) && result_json.len > 0) {
        sc_string_clear(&out->pre_executed_tool_result);
        status = sc_string_from_str(alloc, sc_string_as_str(&result_json), &out->pre_executed_tool_result);
    }
    sc_string_clear(&result_json);
    sc_string_clear(&arguments_json);
    sc_json_destroy(query_value);
    sc_json_destroy(arguments);
    return status;
}

static sc_status parse_ollama_tool_calls(sc_allocator *alloc, sc_json_value *message, sc_provider_response *out)
{
    sc_json_value *calls = sc_json_object_get(message, sc_str_from_cstr("tool_calls"));
    sc_status status = sc_status_ok();

    for (size_t i = 0; calls != nullptr && sc_status_is_ok(status) && i < sc_json_array_len(calls); i += 1) {
        sc_json_value *value = sc_json_array_get(calls, i);
        sc_json_value *function = sc_json_object_get(value, sc_str_from_cstr("function"));
        sc_json_value *args = sc_json_object_get(function, sc_str_from_cstr("arguments"));
        sc_json_value *parsed_args = nullptr;
        sc_provider_tool_call call = {0};
        sc_string args_json = {0};
        sc_str id = {0};
        sc_str name = {0};
        sc_str args_text = {0};

        if (!sc_json_as_str(sc_json_object_get(function, sc_str_from_cstr("name")), &name) || name.len == 0) {
            return sc_status_parse("sc.provider_ollama.tool_call_missing_name");
        }
        if (args == nullptr) {
            status = sc_json_object_new(alloc, &parsed_args);
            args = parsed_args;
        } else if (sc_json_as_str(args, &args_text)) {
            status = sc_json_parse(alloc, args_text, &parsed_args, nullptr);
            if (sc_status_is_ok(status)) {
                args = parsed_args;
            }
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_serialize(args, alloc, &args_json);
        }
        if (sc_status_is_ok(status) &&
            !sc_json_as_str(sc_json_object_get(value, sc_str_from_cstr("id")), &id)) {
            char generated_id[64] = {0};
            (void)snprintf(generated_id, sizeof(generated_id), "ollama-call-%zu", i + 1);
            status = sc_string_from_cstr(alloc, generated_id, &call.call_id);
        } else if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc, id, &call.call_id);
        }
        if (sc_status_is_ok(status)) {
            call.struct_size = sizeof(call);
            status = sc_string_from_str(alloc, name, &call.name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(alloc, sc_string_as_str(&args_json), &call.arguments_json);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_parse(alloc, sc_string_as_str(&args_json), &call.arguments, nullptr);
        }
        if (sc_status_is_ok(status)) {
            status = sc_provider_response_add_tool_call(out, &call);
        }
        sc_provider_tool_call_clear(&call);
        sc_string_clear(&args_json);
        sc_json_destroy(parsed_args);
    }
    return status;
}

static sc_status parse_gemini_error_status(sc_allocator *alloc,
                                           sc_str response_json,
                                           const char *fallback_key,
                                           sc_status_code code)
{
    sc_json_value *root = nullptr;
    sc_json_value *error = nullptr;
    sc_json_parse_error parse_error = {0};
    sc_str message = {0};
    sc_str status_text = {0};
    sc_status status = sc_json_parse(alloc, response_json, &root, &parse_error);

    if (!sc_status_is_ok(status)) {
        return sc_status_make(code, fallback_key, nullptr);
    }
    error = sc_json_object_get(root, sc_str_from_cstr("error"));
    (void)sc_json_as_str(sc_json_object_get(error, sc_str_from_cstr("message")), &message);
    (void)sc_json_as_str(sc_json_object_get(error, sc_str_from_cstr("status")), &status_text);
    if (message.len > 0) {
        status = sc_status_make_owned(alloc, code, fallback_key, message.ptr);
        sc_json_destroy(root);
        return status;
    }
    if (status_text.len > 0) {
        status = sc_status_make_owned(alloc, code, fallback_key, status_text.ptr);
        sc_json_destroy(root);
        return status;
    }
    sc_json_destroy(root);
    return sc_status_make(code, fallback_key, nullptr);
}

static bool contains_data_prefix(sc_str value)
{
    if (value.ptr == nullptr || value.len < 5) {
        return false;
    }
    for (size_t i = 0; i + 5 <= value.len; i += 1) {
        if (memcmp(value.ptr + i, "data:", 5) == 0) {
            return true;
        }
    }
    return false;
}

static bool gemini_model_name_valid(sc_str model)
{
    if (model.ptr == nullptr || model.len == 0 || model.len > 256) {
        return false;
    }
    for (size_t i = 0; i < model.len; i += 1) {
        unsigned char ch = (unsigned char)model.ptr[i];
        bool ok = (ch >= 'a' && ch <= 'z') ||
                  (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') ||
                  ch == '-' || ch == '_' || ch == '.' || ch == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void apply_usage_cost(const sc_provider_options *options, sc_provider_response *response)
{
    if (options == nullptr || response == nullptr) {
        return;
    }
    if (options->input_cost_per_million <= 0.0 && options->output_cost_per_million <= 0.0) {
        return;
    }
    response->cost_usd =
        ((double)response->input_tokens / 1000000.0) * options->input_cost_per_million +
        ((double)response->output_tokens / 1000000.0) * options->output_cost_per_million;
}

static sc_str reasoning_effort_option(const sc_provider_options *options)
{
    if (options == nullptr) {
        return sc_str_from_cstr("");
    }
    if (options->reasoning_effort.len > 0) {
        return options->reasoning_effort;
    }
    switch (options->thinking_level) {
    case SC_PROVIDER_THINKING_DISABLED:
        return sc_str_from_cstr("none");
    case SC_PROVIDER_THINKING_LOW:
        return sc_str_from_cstr("low");
    case SC_PROVIDER_THINKING_MEDIUM:
        return sc_str_from_cstr("medium");
    case SC_PROVIDER_THINKING_HIGH:
        return sc_str_from_cstr("high");
    case SC_PROVIDER_THINKING_DEFAULT:
        break;
    }
    return sc_str_from_cstr("");
}

static sc_status build_anthropic_url(const sc_provider_options *options, sc_allocator *alloc, sc_string *out)
{
    sc_str url = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    if (url.ptr == nullptr || url.len == 0) {
        url = sc_str_from_cstr("https://api.anthropic.com/v1/messages");
    }
    return sc_string_from_str(alloc, url, out);
}

static sc_status build_gemini_url(const sc_provider_options *options,
                                  const sc_provider_request *request,
                                  sc_allocator *alloc,
                                  sc_string *out)
{
    sc_string_builder builder = {0};
    sc_str base = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    sc_str model = model_or_default(options, request);
    sc_status status = sc_status_ok();

    if (base.ptr == nullptr || base.len == 0) {
        base = sc_str_from_cstr("https://generativelanguage.googleapis.com/v1beta/models");
    }
    while (base.len > 0 && base.ptr[base.len - 1] == '/') {
        base.len -= 1;
    }
    if ((base.len >= strlen(":generateContent") &&
         memcmp(base.ptr + base.len - strlen(":generateContent"), ":generateContent", strlen(":generateContent")) == 0) ||
        (base.len >= strlen(":streamGenerateContent") &&
         memcmp(base.ptr + base.len - strlen(":streamGenerateContent"), ":streamGenerateContent", strlen(":streamGenerateContent")) == 0)) {
        return sc_string_from_str(alloc, base, out);
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, base);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, model);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->streaming) {
        status = sc_string_builder_append_cstr(&builder, ":streamGenerateContent?alt=sse");
    } else if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ":generateContent");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_ollama_url(const sc_provider_options *options, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_str base = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    sc_status status = sc_status_ok();

    if (base.ptr == nullptr || base.len == 0) {
        base = sc_str_from_cstr("http://127.0.0.1:11434/api/chat");
    }
    while (base.len > 0 && base.ptr[base.len - 1] == '/') {
        base.len -= 1;
    }
    if (str_ends_with(base, "/api/chat") || str_ends_with(base, "/api/generate")) {
        return sc_string_from_str(alloc, base, out);
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, base);
    if (sc_status_is_ok(status) && str_ends_with(base, "/api")) {
        status = sc_string_builder_append_cstr(&builder, "/chat");
    } else if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/api/chat");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_azure_openai_url(const sc_provider_options *options, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_str base = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr || base.len == 0 ||
        options->deployment.len == 0 || options->api_version.len == 0) {
        return sc_status_invalid_argument("sc.provider_azure_openai.invalid_argument");
    }
    while (base.len > 0 && base.ptr[base.len - 1] == '/') {
        base.len -= 1;
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, base);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/openai/deployments/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, options->deployment);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/chat/completions?api-version=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, options->api_version);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_bedrock_url(const sc_provider_options *options,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_string *out)
{
    sc_string_builder builder = {0};
    sc_str base = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    sc_str region = options == nullptr ? sc_str_from_cstr("") : options->region;
    sc_str model = model_or_default(options, request);
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_bedrock.url_invalid_argument");
    }
    if (model.len == 0) {
        return sc_status_invalid_argument("sc.provider_bedrock.model_missing");
    }
    if (region.len == 0) {
        const char *env_region = getenv("AWS_REGION");
        if (env_region == nullptr || env_region[0] == '\0') {
            env_region = getenv("AWS_DEFAULT_REGION");
        }
        region = env_region == nullptr || env_region[0] == '\0' ? sc_str_from_cstr("us-east-1") : sc_str_from_cstr(env_region);
    }
    if (base.ptr == nullptr || base.len == 0 ||
        sc_str_equal(base, sc_str_from_cstr("https://bedrock-runtime.us-east-1.amazonaws.com"))) {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "https://bedrock-runtime.");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, region);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ".amazonaws.com");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, out);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (!sc_status_is_ok(status)) {
            return status;
        }
        base = sc_string_as_str(out);
    }
    while (base.len > 0 && base.ptr[base.len - 1] == '/') {
        base.len -= 1;
    }
    if (str_ends_with(base, "/converse")) {
        if (out->len > 0) {
            return sc_status_ok();
        }
        return sc_string_from_str(alloc, base, out);
    }
    if (out->len > 0) {
        sc_string existing = *out;
        *out = (sc_string){0};
        base = sc_string_as_str(&existing);
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append(&builder, base);
        sc_string_clear(&existing);
    } else {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append(&builder, base);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/model/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, model);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/converse");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool str_ends_with(sc_str value, const char *suffix)
{
    size_t suffix_len = suffix == nullptr ? 0U : strlen(suffix);
    if (value.ptr == nullptr || suffix == nullptr || value.len < suffix_len) {
        return false;
    }
    return memcmp(value.ptr + value.len - suffix_len, suffix, suffix_len) == 0;
}

static bool str_has_prefix(sc_str value, const char *prefix)
{
    size_t prefix_len = prefix == nullptr ? 0U : strlen(prefix);
    if (value.ptr == nullptr || prefix == nullptr || value.len < prefix_len) {
        return false;
    }
    return memcmp(value.ptr, prefix, prefix_len) == 0;
}

static bool str_equal_cstr(sc_str value, const char *expected)
{
    return sc_str_equal(value, sc_str_from_cstr(expected));
}

static bool provider_http_status_retryable(long response_code)
{
    return response_code == 408 || response_code == 429 || response_code >= 500;
}

static sc_str compatible_preset_base_url(sc_str kind)
{
    if (sc_str_equal(kind, sc_str_from_cstr("groq"))) {
        return sc_str_from_cstr("https://api.groq.com/openai/v1");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("mistral"))) {
        return sc_str_from_cstr("https://api.mistral.ai");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("xai")) || sc_str_equal(kind, sc_str_from_cstr("grok"))) {
        return sc_str_from_cstr("https://api.x.ai");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("deepseek"))) {
        return sc_str_from_cstr("https://api.deepseek.com");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("moonshot"))) {
        return sc_str_from_cstr("https://api.moonshot.cn/v1");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("zai")) ||
        sc_str_equal(kind, sc_str_from_cstr("z.ai")) ||
        sc_str_equal(kind, sc_str_from_cstr("glm"))) {
        return sc_str_from_cstr("https://open.bigmodel.cn/api/paas/v4");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("minimax"))) {
        return sc_str_from_cstr("https://api.minimax.chat");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("qianfan"))) {
        return sc_str_from_cstr("https://qianfan.baidubce.com/v2");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("venice"))) {
        return sc_str_from_cstr("https://api.venice.ai/api/v1");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("vercel-ai-gateway")) ||
        sc_str_equal(kind, sc_str_from_cstr("vercel"))) {
        return sc_str_from_cstr("https://ai-gateway.vercel.sh/v1");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("cloudflare-gateway")) ||
        sc_str_equal(kind, sc_str_from_cstr("cloudflare"))) {
        return sc_str_from_cstr("https://gateway.ai.cloudflare.com/v1");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("opencode"))) {
        return sc_str_from_cstr("https://api.opencode.ai");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("synthetic"))) {
        return sc_str_from_cstr("https://api.synthetic.ai");
    }
    return sc_str_from_cstr("");
}

static sc_str compatible_preset_env(sc_str kind)
{
    if (sc_str_equal(kind, sc_str_from_cstr("groq"))) {
        return sc_str_from_cstr("GROQ_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("mistral"))) {
        return sc_str_from_cstr("MISTRAL_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("xai")) || sc_str_equal(kind, sc_str_from_cstr("grok"))) {
        return sc_str_from_cstr("XAI_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("deepseek"))) {
        return sc_str_from_cstr("DEEPSEEK_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("moonshot"))) {
        return sc_str_from_cstr("MOONSHOT_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("zai")) ||
        sc_str_equal(kind, sc_str_from_cstr("z.ai")) ||
        sc_str_equal(kind, sc_str_from_cstr("glm"))) {
        return sc_str_from_cstr("ZAI_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("minimax"))) {
        return sc_str_from_cstr("MINIMAX_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("qianfan"))) {
        return sc_str_from_cstr("QIANFAN_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("venice"))) {
        return sc_str_from_cstr("VENICE_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("vercel-ai-gateway")) ||
        sc_str_equal(kind, sc_str_from_cstr("vercel"))) {
        return sc_str_from_cstr("VERCEL_AI_GATEWAY_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("cloudflare-gateway")) ||
        sc_str_equal(kind, sc_str_from_cstr("cloudflare"))) {
        return sc_str_from_cstr("CLOUDFLARE_API_TOKEN");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("opencode"))) {
        return sc_str_from_cstr("OPENCODE_API_KEY");
    }
    if (sc_str_equal(kind, sc_str_from_cstr("synthetic"))) {
        return sc_str_from_cstr("SYNTHETIC_API_KEY");
    }
    return sc_str_from_cstr("");
}

static void sleep_ms(uint32_t delay_ms)
{
    struct timespec requested = {
        .tv_sec = (time_t)(delay_ms / 1000U),
        .tv_nsec = (long)(delay_ms % 1000U) * 1000000L,
    };
    if (delay_ms == 0) {
        return;
    }
    (void)nanosleep(&requested, nullptr);
}

static sc_status copy_secret(sc_allocator *alloc, sc_str value, sc_string *out)
{
    if (value.len == 0) {
        return sc_string_from_cstr(alloc, "", out);
    }
    return sc_string_from_str(alloc, value, out);
}

static sc_status bedrock_credentials_set(sc_allocator *alloc,
                                         sc_str access_key_id,
                                         sc_str secret_access_key,
                                         sc_str session_token,
                                         sc_provider_bedrock_credentials *out)
{
    sc_status status = sc_status_ok();

    if (access_key_id.len == 0) {
        return sc_status_security_denied("sc.provider_bedrock.missing_aws_access_key_id");
    }
    if (secret_access_key.len == 0) {
        return sc_status_security_denied("sc.provider_bedrock.missing_aws_secret_access_key");
    }
    sc_provider_bedrock_credentials_clear(out);
    *out = (sc_provider_bedrock_credentials){.struct_size = sizeof(*out)};
    status = copy_secret(alloc, access_key_id, &out->access_key_id);
    if (sc_status_is_ok(status)) {
        status = copy_secret(alloc, secret_access_key, &out->secret_access_key);
    }
    if (sc_status_is_ok(status) && session_token.len > 0) {
        status = copy_secret(alloc, session_token, &out->session_token);
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_bedrock_credentials_clear(out);
    }
    return status;
}

static sc_status bedrock_resolve_from_environment(sc_allocator *alloc, sc_provider_bedrock_credentials *out)
{
    const char *access_key = getenv("AWS_ACCESS_KEY_ID");
    const char *secret_key = getenv("AWS_SECRET_ACCESS_KEY");
    const char *token = getenv("AWS_SESSION_TOKEN");

    if (access_key == nullptr || access_key[0] == '\0' || secret_key == nullptr || secret_key[0] == '\0') {
        return sc_status_security_denied("sc.provider_bedrock.env_credentials_missing");
    }
    return bedrock_credentials_set(alloc,
                                   sc_str_from_cstr(access_key),
                                   sc_str_from_cstr(secret_key),
                                   token == nullptr ? sc_str_from_cstr("") : sc_str_from_cstr(token),
                                   out);
}

static sc_status bedrock_resolve_from_shared_files(sc_allocator *alloc, sc_provider_bedrock_credentials *out)
{
    const char *profile_env = getenv("AWS_PROFILE");
    const char *credentials_env = getenv("AWS_SHARED_CREDENTIALS_FILE");
    const char *config_env = getenv("AWS_CONFIG_FILE");
    sc_str profile = profile_env == nullptr || profile_env[0] == '\0' ? sc_str_from_cstr("default") :
                                                                         sc_str_from_cstr(profile_env);
    sc_string credentials_path = {0};
    sc_string config_path = {0};
    sc_string credentials_text = {0};
    sc_string config_text = {0};
    sc_status status = sc_status_ok();

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (credentials_env != nullptr && credentials_env[0] != '\0') {
        status = sc_string_from_cstr(alloc, credentials_env, &credentials_path);
    } else {
        status = bedrock_default_file_path(alloc, ".aws/credentials", &credentials_path);
    }
    if (sc_status_is_ok(status)) {
        if (config_env != nullptr && config_env[0] != '\0') {
            status = sc_string_from_cstr(alloc, config_env, &config_path);
        } else {
            status = bedrock_default_file_path(alloc, ".aws/config", &config_path);
        }
    }
    if (sc_status_is_ok(status)) {
        status = bedrock_read_file(alloc, sc_string_as_str(&credentials_path), 128U * 1024U, &credentials_text);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &credentials_text);
        }
    }
    if (sc_status_is_ok(status)) {
        status = bedrock_read_file(alloc, sc_string_as_str(&config_path), 128U * 1024U, &config_text);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_string_from_cstr(alloc, "", &config_text);
        }
    }
    if (sc_status_is_ok(status)) {
        status = bedrock_copy_profile_value(alloc, &credentials_text, &config_text, profile, out);
    }
    sc_string_clear(&credentials_path);
    sc_string_clear(&config_path);
    sc_string_secure_clear(&credentials_text);
    sc_string_secure_clear(&config_text);
    return status;
}

static sc_status bedrock_resolve_from_metadata(sc_allocator *alloc, sc_provider_bedrock_credentials *out)
{
    const char *relative = getenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    const char *full = getenv("AWS_CONTAINER_CREDENTIALS_FULL_URI");
    const char *imds_disabled = getenv("AWS_EC2_METADATA_DISABLED");
    sc_string url = {0};
    sc_string payload = {0};
    sc_string token = {0};
    sc_status status = sc_status_security_denied("sc.provider_bedrock.metadata_credentials_missing");
    bool explicit_metadata_url = false;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (relative != nullptr && relative[0] != '\0') {
        sc_string_builder builder = {0};
        explicit_metadata_url = true;
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "http://169.254.170.2");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, relative);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &url);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status) && !bedrock_metadata_url_allowed(sc_string_as_str(&url), false)) {
            status = sc_status_security_denied("sc.provider_bedrock.metadata_url_denied");
        }
    } else if (full != nullptr && full[0] != '\0') {
        explicit_metadata_url = true;
        status = sc_string_from_cstr(alloc, full, &url);
        if (sc_status_is_ok(status) && !bedrock_metadata_url_allowed(sc_string_as_str(&url), true)) {
            status = sc_status_security_denied("sc.provider_bedrock.metadata_url_denied");
        }
    }
    if (sc_status_is_ok(status) && url.len > 0) {
        const char *auth_token = getenv("AWS_CONTAINER_AUTHORIZATION_TOKEN");
        status = bedrock_fetch_metadata_json(alloc,
                                             sc_string_as_str(&url),
                                             auth_token == nullptr ? sc_str_from_cstr("") : sc_str_from_cstr(auth_token),
                                             &payload);
        if (sc_status_is_ok(status)) {
            status = bedrock_parse_metadata_credentials(alloc, sc_string_as_str(&payload), out);
        }
        sc_string_clear(&url);
        sc_string_secure_clear(&payload);
        return status;
    }
    sc_string_clear(&url);
    if (explicit_metadata_url && !sc_status_is_ok(status)) {
        return status;
    }

    if (env_truthy(imds_disabled)) {
        return sc_status_security_denied("sc.provider_bedrock.imds_disabled");
    }
    status = bedrock_fetch_metadata_json(alloc,
                                         sc_str_from_cstr("http://169.254.169.254/latest/api/token"),
                                         sc_str_from_cstr(""),
                                         &token);
    if (sc_status_is_ok(status)) {
        status = bedrock_fetch_metadata_json(alloc,
                                             sc_str_from_cstr("http://169.254.169.254/latest/meta-data/iam/security-credentials/"),
                                             sc_string_as_str(&token),
                                             &payload);
    }
    if (sc_status_is_ok(status) && payload.len > 0) {
        sc_str role = sc_string_as_str(&payload);
        while (role.len > 0 && (role.ptr[role.len - 1] == '\n' || role.ptr[role.len - 1] == '\r')) {
            role.len -= 1;
        }
        sc_string_clear(&url);
        sc_string_builder builder = {0};
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder,
                                               "http://169.254.169.254/latest/meta-data/iam/security-credentials/");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, role);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &url);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    sc_string_secure_clear(&payload);
    if (sc_status_is_ok(status)) {
        status = bedrock_fetch_metadata_json(alloc, sc_string_as_str(&url), sc_string_as_str(&token), &payload);
    }
    if (sc_status_is_ok(status)) {
        status = bedrock_parse_metadata_credentials(alloc, sc_string_as_str(&payload), out);
    }
    sc_string_clear(&url);
    sc_string_secure_clear(&payload);
    sc_string_secure_clear(&token);
    return status;
}

static sc_status bedrock_read_file(sc_allocator *alloc, sc_str path, size_t max_bytes, sc_string *out)
{
    FILE *file = nullptr;
    sc_string tmp = {0};
    size_t read_count = 0;
    sc_status status = sc_status_ok();

    if (out == nullptr || path.len == 0 || path.ptr == nullptr || path.len >= 4096U) {
        return sc_status_invalid_argument("sc.provider_bedrock.file_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tmp.ptr = sc_alloc(alloc, max_bytes + 1U, _Alignof(char));
    if (tmp.ptr == nullptr) {
        return sc_status_no_memory();
    }
    tmp.alloc = alloc;
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.provider_bedrock.file_open_failed");
        goto cleanup;
    }
    read_count = fread(tmp.ptr, 1, max_bytes, file);
    if (ferror(file) != 0) {
        status = sc_status_io("sc.provider_bedrock.file_read_failed");
        goto cleanup;
    }
    if (sc_status_is_ok(status)) {
        tmp.ptr[read_count] = '\0';
        tmp.len = read_count;
        *out = tmp;
        tmp = (sc_string){0};
    }

cleanup:
    if (file != nullptr && fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.provider_bedrock.file_close_failed");
    }
    sc_string_clear(&tmp);
    return status;
}

static sc_status bedrock_default_file_path(sc_allocator *alloc, const char *suffix, sc_string *out)
{
    const char *home = getenv("HOME");
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (home == nullptr || home[0] == '\0') {
        return sc_status_io("sc.provider_bedrock.home_missing");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, home);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, suffix);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool bedrock_find_profile_keys(sc_str text, sc_str profile, bool config_style, bedrock_ini_keys *out)
{
    bool in_section = false;
    bool found = false;
    size_t offset = 0;

    if (out == nullptr) {
        return false;
    }
    *out = (bedrock_ini_keys){0};
    while (offset <= text.len) {
        size_t start = offset;
        size_t line_len = 0;
        sc_str line = {0};
        const char *equals = nullptr;

        while (offset + line_len < text.len && text.ptr[offset + line_len] != '\n') {
            line_len += 1;
        }
        line = bedrock_trim(sc_str_from_parts(text.ptr + start, line_len));
        if (line.len >= 2U && line.ptr[0] == '[' && line.ptr[line.len - 1U] == ']') {
            in_section = bedrock_ini_section_matches(sc_str_from_parts(line.ptr + 1, line.len - 2U),
                                                     profile,
                                                     config_style);
        } else if (in_section && line.len > 0 && line.ptr[0] != '#' && line.ptr[0] != ';') {
            for (size_t i = 0; i < line.len; i += 1) {
                if (line.ptr[i] == '=') {
                    equals = line.ptr + i;
                    break;
                }
            }
            if (equals != nullptr) {
                sc_str key = bedrock_trim(sc_str_from_parts(line.ptr, (size_t)(equals - line.ptr)));
                sc_str value = bedrock_trim(sc_str_from_parts(equals + 1, line.len - (size_t)(equals - line.ptr) - 1U));
                if (str_equal_cstr(key, "aws_access_key_id")) {
                    out->access_key_id = value;
                    found = true;
                } else if (str_equal_cstr(key, "aws_secret_access_key")) {
                    out->secret_access_key = value;
                    found = true;
                } else if (str_equal_cstr(key, "aws_session_token")) {
                    out->session_token = value;
                    found = true;
                }
            }
        }
        if (offset + line_len >= text.len) {
            break;
        }
        offset += line_len + 1U;
    }
    return found;
}

static bool bedrock_ini_section_matches(sc_str section, sc_str profile, bool config_style)
{
    section = bedrock_trim(section);
    if (!config_style || str_equal_cstr(profile, "default")) {
        return sc_str_equal(section, profile);
    }
    if (!str_has_prefix(section, "profile ")) {
        return false;
    }
    section.ptr += strlen("profile ");
    section.len -= strlen("profile ");
    return sc_str_equal(bedrock_trim(section), profile);
}

static sc_str bedrock_trim(sc_str value)
{
    while (value.len > 0 && isspace((unsigned char)value.ptr[0])) {
        value.ptr += 1;
        value.len -= 1;
    }
    while (value.len > 0 && isspace((unsigned char)value.ptr[value.len - 1U])) {
        value.len -= 1;
    }
    return value;
}

static sc_status bedrock_copy_profile_value(sc_allocator *alloc,
                                            const sc_string *credentials_text,
                                            const sc_string *config_text,
                                            sc_str profile,
                                            sc_provider_bedrock_credentials *out)
{
    bedrock_ini_keys credentials_keys = {0};
    bedrock_ini_keys config_keys = {0};
    sc_str access_key = {0};
    sc_str secret_key = {0};
    sc_str token = {0};

    (void)bedrock_find_profile_keys(sc_string_as_str(credentials_text), profile, false, &credentials_keys);
    (void)bedrock_find_profile_keys(sc_string_as_str(config_text), profile, true, &config_keys);
    access_key = credentials_keys.access_key_id.len > 0 ? credentials_keys.access_key_id : config_keys.access_key_id;
    secret_key = credentials_keys.secret_access_key.len > 0 ? credentials_keys.secret_access_key : config_keys.secret_access_key;
    token = credentials_keys.session_token.len > 0 ? credentials_keys.session_token : config_keys.session_token;
    if (access_key.len == 0 || secret_key.len == 0) {
        return sc_status_security_denied("sc.provider_bedrock.profile_credentials_missing");
    }
    return bedrock_credentials_set(alloc, access_key, secret_key, token, out);
}

static sc_status bedrock_fetch_metadata_json(sc_allocator *alloc, sc_str url, sc_str token, sc_string *out)
{
    if (!bedrock_metadata_url_allowed(url, true)) {
        return sc_status_security_denied("sc.provider_bedrock.metadata_url_denied");
    }
    if (g_bedrock_metadata_fetcher.fetcher != nullptr) {
        return g_bedrock_metadata_fetcher.fetcher(g_bedrock_metadata_fetcher.user_data, url, token, alloc, out);
    }
#ifdef SC_HAVE_LIBCURL
    CURL *curl = nullptr;
    CURLcode code;
    struct curl_slist *headers = nullptr;
    http_response_buffer response = {0};
    sc_status status = provider_curl_global_init();

    if (!sc_status_is_ok(status)) {
        return status;
    }
    curl = curl_easy_init();
    if (curl == nullptr) {
        return sc_status_no_memory();
    }
    sc_bytes_init(&response.bytes, alloc);
    response.max_bytes = 64U * 1024U;
    if (str_equal_cstr(url, "http://169.254.169.254/latest/api/token")) {
        status = add_header(&headers, "X-aws-ec2-metadata-token-ttl-seconds: 21600");
        (void)curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    } else {
        (void)curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
        if (token.len > 0) {
            status = add_header_with_value(alloc, &headers, "X-aws-ec2-metadata-token: ", token);
        }
    }
    if (sc_status_is_ok(status)) {
        (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, provider_curl_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 bedrock-credentials");
        (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 1000L);
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 2000L);
        (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        code = curl_easy_perform(curl);
        if (code != CURLE_OK || response.too_large) {
            status = sc_status_io(response.too_large ? "sc.provider_bedrock.metadata_too_large" :
                                                       "sc.provider_bedrock.metadata_request_failed");
        } else {
            status = sc_string_from_str(alloc,
                                        sc_str_from_parts((const char *)response.bytes.ptr, response.bytes.len),
                                        out);
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    sc_bytes_clear(&response.bytes);
    return status;
#else
    (void)alloc;
    (void)token;
    (void)out;
    return sc_status_unsupported("sc.provider_bedrock.metadata_libcurl_unavailable");
#endif
}

static sc_status bedrock_parse_metadata_credentials(sc_allocator *alloc,
                                                    sc_str payload,
                                                    sc_provider_bedrock_credentials *out)
{
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_str access_key = {0};
    sc_str secret_key = {0};
    sc_str token = {0};
    sc_status status = sc_json_parse(alloc, payload, &root, &error);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    (void)sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("AccessKeyId")), &access_key);
    (void)sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("SecretAccessKey")), &secret_key);
    (void)sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("Token")), &token);
    status = bedrock_credentials_set(alloc, access_key, secret_key, token, out);
    sc_json_destroy(root);
    return status;
}

static bool bedrock_metadata_url_allowed(sc_str url, bool full_uri)
{
    if (url.ptr == nullptr || url.len == 0) {
        return false;
    }
    if (str_has_prefix(url, "http://169.254.170.2/") ||
        str_has_prefix(url, "http://169.254.169.254/")) {
        return true;
    }
    if (!full_uri) {
        return false;
    }
    return str_has_prefix(url, "http://127.0.0.1:") ||
           str_has_prefix(url, "http://localhost:") ||
           str_has_prefix(url, "http://[::1]:");
}

static bool env_truthy(const char *value)
{
    return value != nullptr &&
           (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "TRUE") == 0);
}

#ifdef SC_HAVE_LIBCURL
static sc_status provider_curl_global_init(void)
{
    static bool initialized = false;
    sc_status status = sc_status_ok();

    if (initialized) {
        return sc_status_ok();
    }
    status = sc_curl_global_init("sc.provider_http.curl_init_failed");
    if (!sc_status_is_ok(status)) {
        return status;
    }
    (void)atexit(provider_curl_global_cleanup);
    initialized = true;
    return sc_status_ok();
}

static void provider_curl_global_cleanup(void)
{
    if (g_provider_curl != nullptr) {
        curl_easy_cleanup(g_provider_curl);
        g_provider_curl = nullptr;
    }
    g_provider_curl_in_use = false;
}

static sc_status provider_curl_shared_handle(CURL **out, bool *shared)
{
    if (out == nullptr || shared == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.curl_handle_invalid_argument");
    }
    *out = nullptr;
    *shared = false;
    if (provider_low_memory_transport_enabled() || g_provider_curl_in_use) {
        *out = curl_easy_init();
        if (*out == nullptr) {
            return sc_status_no_memory();
        }
        return sc_status_ok();
    }
    if (g_provider_curl == nullptr) {
        g_provider_curl = curl_easy_init();
        if (g_provider_curl == nullptr) {
            return sc_status_no_memory();
        }
    }
    g_provider_curl_in_use = true;
    *shared = true;
    *out = g_provider_curl;
    return sc_status_ok();
}

static void provider_curl_release_handle(CURL *curl, bool shared)
{
    if (curl == nullptr) {
        return;
    }
    if (shared) {
        g_provider_curl_in_use = false;
        if (provider_low_memory_transport_enabled()) {
            curl_easy_cleanup(g_provider_curl);
            g_provider_curl = nullptr;
        }
        return;
    }
    curl_easy_cleanup(curl);
}

static void provider_curl_apply_common_options(CURL *curl)
{
    const char *ca_bundle = provider_ca_bundle_path();

    if (curl == nullptr) {
        return;
    }
    if (ca_bundle != nullptr && ca_bundle[0] != '\0') {
        (void)curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    }
    (void)curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
    if (provider_low_memory_transport_enabled()) {
        (void)curl_easy_setopt(curl, CURLOPT_UPLOAD_BUFFERSIZE, (long)provider_low_memory_upload_buffer_size);
        (void)curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 1L);
    }
}

static bool provider_low_memory_transport_enabled(void)
{
    const char *value = getenv("SC_LOW_MEMORY_TRANSPORT");
    if (value == nullptr || value[0] == '\0') {
        value = getenv("SC_LOW_MEMORY");
    }
    return env_truthy(value);
}

static const char *provider_ca_bundle_path(void)
{
    const char *value = getenv("SC_PROVIDER_CA_BUNDLE");
    if (value == nullptr || value[0] == '\0') {
        value = getenv("SC_CA_BUNDLE");
    }
    if (value == nullptr || value[0] == '\0') {
        value = getenv("CURL_CA_BUNDLE");
    }
    return value;
}

static sc_status openai_curl_send(void *user_data,
                                  const sc_provider_options *options,
                                  sc_str request_json,
                                  sc_allocator *alloc,
                                  sc_string *response_json,
                                  int *http_status)
{
    sc_str url = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    (void)user_data;
    if (url.ptr == nullptr || url.len == 0) {
        url = sc_str_from_cstr("https://api.openai.com/v1/chat/completions");
    }
    return provider_curl_json_post(options, url, request_json, AUTH_OPENAI_BEARER, alloc, response_json, http_status);
}

static sc_status compatible_curl_send(void *user_data,
                                      const sc_provider_options *options,
                                      sc_str request_json,
                                      sc_allocator *alloc,
                                      sc_string *response_json,
                                      int *http_status)
{
    sc_str url = options == nullptr ? sc_str_from_cstr("") : options->base_url;
    auth_kind auth = AUTH_NONE;
    (void)user_data;

    if (options != nullptr &&
        ((options->api_key.ptr != nullptr && options->api_key.len > 0) ||
         (options->credential_env.ptr != nullptr && options->credential_env.len > 0))) {
        auth = AUTH_OPENAI_BEARER;
    }
    return provider_curl_json_post(options, url, request_json, auth, alloc, response_json, http_status);
}

static sc_status provider_curl_json_post(const sc_provider_options *options,
                                         sc_str url,
                                         sc_str request_json,
                                         auth_kind auth,
                                         sc_allocator *alloc,
                                         sc_string *response_json,
                                         int *http_status)
{
    CURL *curl = nullptr;
    CURLcode code = CURLE_OK;
    struct curl_slist *headers = nullptr;
    bool shared_curl = false;
    sc_string credential = {0};
    sc_string aws_secret = {0};
    sc_string aws_session_token = {0};
    sc_string aws_sigv4 = {0};
    sc_provider_bedrock_credentials aws_credentials = {0};
    sc_string redacted_url = {0};
    http_response_buffer response = {0};
    long response_code = 0;
    long timeout_ms = options == nullptr || options->timeout_ms == 0 ? 30000L : (long)options->timeout_ms;
    sc_status status = sc_status_ok();
    char request_bytes[32] = {0};
    char response_bytes[32] = {0};
    char http_status_text[32] = {0};
    char curl_code_text[32] = {0};
    char timeout_text[32] = {0};
    char attempts_text[32] = {0};
    char max_attempts_text[32] = {0};
    char backoff_text[32] = {0};
    sc_log_field start_fields[6] = {0};
    sc_log_field retry_fields[10] = {0};
    sc_log_field done_fields[12] = {0};
    uint32_t max_attempts = options == nullptr ? 1U : options->max_retries + 1U;
    uint32_t attempt = 0;

    if (options == nullptr || response_json == nullptr || (url.len > 0 && url.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.provider_http.invalid_argument");
    }
    (void)provider_redact_url_for_log(alloc == nullptr ? sc_allocator_heap() : alloc, url, &redacted_url);
    (void)snprintf(request_bytes, sizeof(request_bytes), "%zu", request_json.len);
    (void)snprintf(timeout_text, sizeof(timeout_text), "%ld", timeout_ms);
    start_fields[0] = (sc_log_field){.key = "provider", .value = options->provider_name, .secret = false};
    start_fields[1] = (sc_log_field){.key = "method", .value = sc_str_from_cstr("POST"), .secret = false};
    start_fields[2] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
    start_fields[3] = (sc_log_field){.key = "auth", .value = sc_str_from_cstr(auth_kind_name(auth)), .secret = false};
    start_fields[4] = (sc_log_field){.key = "request_bytes", .value = sc_str_from_cstr(request_bytes), .secret = false};
    start_fields[5] = (sc_log_field){.key = "timeout_ms", .value = sc_str_from_cstr(timeout_text), .secret = false};
    sc_log_write(SC_LOG_TRACE, "sc.net", "net.provider.request.start", start_fields, SC_ARRAY_LEN(start_fields));

    status = sc_status_ok();
    if (auth != AUTH_NONE && auth != AUTH_AWS_SIGV4) {
        status = sc_provider_resolve_credential(alloc, options, &credential);
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&redacted_url);
            return status;
        }
    }
    if (sc_status_is_ok(status) && auth == AUTH_AWS_SIGV4) {
        const char *region_env = nullptr;
        sc_str region = options->region;
        sc_string_builder builder = {0};
        status = sc_provider_bedrock_resolve_credentials(alloc, options, &aws_credentials);
        if (sc_status_is_ok(status)) {
            credential = aws_credentials.access_key_id;
            aws_credentials.access_key_id = (sc_string){0};
            aws_secret = aws_credentials.secret_access_key;
            aws_credentials.secret_access_key = (sc_string){0};
            aws_session_token = aws_credentials.session_token;
            aws_credentials.session_token = (sc_string){0};
        }
        if (region.len == 0) {
            region_env = getenv("AWS_REGION");
            if (region_env == nullptr || region_env[0] == '\0') {
                region_env = getenv("AWS_DEFAULT_REGION");
            }
            region = region_env == nullptr || region_env[0] == '\0' ? sc_str_from_cstr("us-east-1") : sc_str_from_cstr(region_env);
        }
        sc_string_builder_init(&builder, alloc);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "aws:amz:");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, region);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ":bedrock");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &aws_sigv4);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    if (sc_status_is_ok(status)) {
        status = provider_curl_global_init();
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_bedrock_credentials_clear(&aws_credentials);
        sc_string_secure_clear(&aws_session_token);
        sc_string_secure_clear(&aws_secret);
        sc_string_clear(&aws_sigv4);
        sc_string_secure_clear(&credential);
        sc_string_clear(&redacted_url);
        return status;
    }
    status = provider_curl_shared_handle(&curl, &shared_curl);
    if (!sc_status_is_ok(status)) {
        sc_provider_bedrock_credentials_clear(&aws_credentials);
        sc_string_secure_clear(&aws_session_token);
        sc_string_secure_clear(&aws_secret);
        sc_string_clear(&aws_sigv4);
        sc_string_secure_clear(&credential);
        sc_string_clear(&redacted_url);
        return status;
    }

    sc_bytes_init(&response.bytes, alloc);
    response.max_bytes = 8U * 1024U * 1024U;

    status = add_header(&headers, "Content-Type: application/json");
    if (sc_status_is_ok(status) && auth == AUTH_OPENAI_BEARER) {
        status = add_header_with_value(alloc, &headers, "Authorization: Bearer ", sc_string_as_str(&credential));
        if (sc_status_is_ok(status) && sc_str_equal(options->provider_name, sc_str_from_cstr("openrouter"))) {
            status = add_header_with_value(alloc, &headers, "HTTP-Referer: ", options->openrouter_referer.len == 0 ? sc_str_from_cstr("https://smolclaw.local") : options->openrouter_referer);
        }
        if (sc_status_is_ok(status) && sc_str_equal(options->provider_name, sc_str_from_cstr("openrouter"))) {
            status = add_header_with_value(alloc, &headers, "X-Title: ", options->openrouter_title.len == 0 ? sc_str_from_cstr("SmolClaw") : options->openrouter_title);
        }
    } else if (sc_status_is_ok(status) && auth == AUTH_ANTHROPIC_KEY) {
        if (str_has_prefix(sc_string_as_str(&credential), "sk-ant-oat")) {
            status = add_header_with_value(alloc, &headers, "Authorization: Bearer ", sc_string_as_str(&credential));
        } else {
            status = add_header_with_value(alloc, &headers, "x-api-key: ", sc_string_as_str(&credential));
        }
        if (sc_status_is_ok(status)) {
            status = add_header(&headers, "anthropic-version: 2023-06-01");
        }
    } else if (sc_status_is_ok(status) && auth == AUTH_GEMINI_KEY) {
        status = add_header_with_value(alloc, &headers, "x-goog-api-key: ", sc_string_as_str(&credential));
    } else if (sc_status_is_ok(status) && auth == AUTH_AWS_SIGV4) {
        if (aws_session_token.len > 0) {
            status = add_header_with_value(alloc, &headers, "X-Amz-Security-Token: ", sc_string_as_str(&aws_session_token));
        }
    }

    if (sc_status_is_ok(status)) {
        curl_easy_reset(curl);
        (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.ptr == nullptr ? "" : request_json.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_json.len);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, provider_curl_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 provider-http");
        (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 10000L ? timeout_ms : 10000L);
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        provider_curl_apply_common_options(curl);
        if (auth == AUTH_AWS_SIGV4) {
#if defined(SC_HAVE_CURL_AWS_SIGV4)
            (void)curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_AWS_SIGV4);
            (void)curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, aws_sigv4.ptr);
            (void)curl_easy_setopt(curl, CURLOPT_USERNAME, credential.ptr);
            (void)curl_easy_setopt(curl, CURLOPT_PASSWORD, aws_secret.ptr);
#else
            status = sc_status_unsupported("sc.provider_bedrock.curl_sigv4_unavailable");
#endif
        }

        for (attempt = 1; sc_status_is_ok(status) && attempt <= max_attempts; attempt += 1) {
            response.bytes.len = 0;
            response.too_large = false;
            response_code = 0;
            code = curl_easy_perform(curl);
            (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
            if (response.too_large ||
                (!provider_curl_code_retryable(code) && !provider_http_status_retryable(response_code)) ||
                attempt >= max_attempts) {
                break;
            }
            (void)snprintf(http_status_text, sizeof(http_status_text), "%ld", response_code);
            (void)snprintf(curl_code_text, sizeof(curl_code_text), "%d", (int)code);
            (void)snprintf(response_bytes, sizeof(response_bytes), "%zu", response.bytes.len);
            (void)snprintf(attempts_text, sizeof(attempts_text), "%u", attempt);
            (void)snprintf(max_attempts_text, sizeof(max_attempts_text), "%u", max_attempts);
            (void)snprintf(backoff_text, sizeof(backoff_text), "%u", options->retry_backoff_ms * attempt);
            retry_fields[0] = (sc_log_field){.key = "provider", .value = options->provider_name, .secret = false};
            retry_fields[1] = (sc_log_field){.key = "method", .value = sc_str_from_cstr("POST"), .secret = false};
            retry_fields[2] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
            retry_fields[3] = (sc_log_field){.key = "http_status", .value = sc_str_from_cstr(http_status_text), .secret = false};
            retry_fields[4] = (sc_log_field){.key = "curl_code", .value = sc_str_from_cstr(curl_code_text), .secret = false};
            retry_fields[5] = (sc_log_field){.key = "curl_error", .value = sc_str_from_cstr(curl_easy_strerror(code)), .secret = false};
            retry_fields[6] = (sc_log_field){.key = "response_bytes", .value = sc_str_from_cstr(response_bytes), .secret = false};
            retry_fields[7] = (sc_log_field){.key = "attempt", .value = sc_str_from_cstr(attempts_text), .secret = false};
            retry_fields[8] = (sc_log_field){.key = "max_attempts", .value = sc_str_from_cstr(max_attempts_text), .secret = false};
            retry_fields[9] = (sc_log_field){.key = "backoff_ms", .value = sc_str_from_cstr(backoff_text), .secret = false};
            sc_log_write(SC_LOG_TRACE, "sc.net", "net.provider.request.retry", retry_fields, SC_ARRAY_LEN(retry_fields));
            sleep_ms(options->retry_backoff_ms * attempt);
        }
        if (http_status != nullptr) {
            *http_status = (int)response_code;
        }
        if (code != CURLE_OK || response.too_large) {
            status = sc_status_http(response.too_large ? "sc.provider_http.response_too_large" : "sc.provider_http.request_failed");
        } else {
            status = sc_string_from_str(alloc, sc_str_from_parts((const char *)response.bytes.ptr, response.bytes.len), response_json);
        }
    }

    (void)snprintf(response_bytes, sizeof(response_bytes), "%zu", response.bytes.len);
    (void)snprintf(http_status_text, sizeof(http_status_text), "%ld", response_code);
    (void)snprintf(curl_code_text, sizeof(curl_code_text), "%d", (int)code);
    (void)snprintf(attempts_text, sizeof(attempts_text), "%u", attempt == 0 ? 1U : attempt);
    (void)snprintf(max_attempts_text, sizeof(max_attempts_text), "%u", max_attempts);
    done_fields[0] = (sc_log_field){.key = "provider", .value = options->provider_name, .secret = false};
    done_fields[1] = (sc_log_field){.key = "method", .value = sc_str_from_cstr("POST"), .secret = false};
    done_fields[2] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
    done_fields[3] = (sc_log_field){.key = "http_status", .value = sc_str_from_cstr(http_status_text), .secret = false};
    done_fields[4] = (sc_log_field){.key = "curl_code", .value = sc_str_from_cstr(curl_code_text), .secret = false};
    done_fields[5] = (sc_log_field){.key = "curl_error", .value = sc_str_from_cstr(curl_easy_strerror(code)), .secret = false};
    done_fields[6] = (sc_log_field){.key = "response_bytes", .value = sc_str_from_cstr(response_bytes), .secret = false};
    done_fields[7] = (sc_log_field){.key = "too_large", .value = sc_str_from_cstr(response.too_large ? "true" : "false"), .secret = false};
    done_fields[8] = (sc_log_field){.key = "status", .value = sc_str_from_cstr(sc_status_is_ok(status) ? "ok" : "error"), .secret = false};
    done_fields[9] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
    done_fields[10] = (sc_log_field){.key = "attempts", .value = sc_str_from_cstr(attempts_text), .secret = false};
    done_fields[11] = (sc_log_field){.key = "max_attempts", .value = sc_str_from_cstr(max_attempts_text), .secret = false};
    sc_log_write(SC_LOG_TRACE, "sc.net", "net.provider.request.done", done_fields, SC_ARRAY_LEN(done_fields));

    curl_slist_free_all(headers);
    provider_curl_release_handle(curl, shared_curl);
    sc_bytes_clear(&response.bytes);
    sc_string_clear(&redacted_url);
    sc_provider_bedrock_credentials_clear(&aws_credentials);
    sc_string_secure_clear(&aws_session_token);
    sc_string_secure_clear(&aws_secret);
    sc_string_clear(&aws_sigv4);
    sc_string_secure_clear(&credential);
    return status;
}

static sc_status provider_curl_stream_post(const sc_provider_options *options,
                                           sc_str url,
                                           sc_str request_json,
                                           auth_kind auth,
                                           provider_stream_chunk_fn on_chunk,
                                           provider_stream_finish_fn on_finish,
                                           void *stream_user_data,
                                           int *http_status)
{
    CURL *curl = nullptr;
    CURLcode code = CURLE_OK;
    struct curl_slist *headers = nullptr;
    bool shared_curl = false;
    sc_string credential = {0};
    sc_string redacted_url = {0};
    provider_stream_curl_context stream = {
        .on_chunk = on_chunk,
        .user_data = stream_user_data,
        .status = sc_status_ok(),
    };
    long response_code = 0;
    long timeout_ms = options == nullptr || options->timeout_ms == 0 ? 30000L : (long)options->timeout_ms;
    sc_status status = sc_status_ok();
    char request_bytes[32] = {0};
    char response_bytes[32] = {0};
    char http_status_text[32] = {0};
    char curl_code_text[32] = {0};
    char timeout_text[32] = {0};
    sc_log_field start_fields[6] = {0};
    sc_log_field done_fields[10] = {0};

    if (options == nullptr || on_chunk == nullptr || on_finish == nullptr || (url.len > 0 && url.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.provider_http.stream_invalid_argument");
    }
    (void)provider_redact_url_for_log(sc_allocator_heap(), url, &redacted_url);
    (void)snprintf(request_bytes, sizeof(request_bytes), "%zu", request_json.len);
    (void)snprintf(timeout_text, sizeof(timeout_text), "%ld", timeout_ms);
    start_fields[0] = (sc_log_field){.key = "provider", .value = options->provider_name, .secret = false};
    start_fields[1] = (sc_log_field){.key = "method", .value = sc_str_from_cstr("POST"), .secret = false};
    start_fields[2] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
    start_fields[3] = (sc_log_field){.key = "auth", .value = sc_str_from_cstr(auth_kind_name(auth)), .secret = false};
    start_fields[4] = (sc_log_field){.key = "request_bytes", .value = sc_str_from_cstr(request_bytes), .secret = false};
    start_fields[5] = (sc_log_field){.key = "timeout_ms", .value = sc_str_from_cstr(timeout_text), .secret = false};
    sc_log_write(SC_LOG_TRACE, "sc.net", "net.provider.stream.start", start_fields, SC_ARRAY_LEN(start_fields));

    status = sc_status_ok();
    if (auth != AUTH_NONE) {
        status = sc_provider_resolve_credential(sc_allocator_heap(), options, &credential);
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&redacted_url);
            return status;
        }
    }
    if (sc_status_is_ok(status)) {
        status = provider_curl_global_init();
    }
    if (sc_status_is_ok(status)) {
        status = provider_curl_shared_handle(&curl, &shared_curl);
    }

    if (sc_status_is_ok(status)) {
        status = add_header(&headers, "Content-Type: application/json");
    }
    if (sc_status_is_ok(status) && auth == AUTH_OPENAI_BEARER) {
        status = add_header_with_value(sc_allocator_heap(), &headers, "Authorization: Bearer ", sc_string_as_str(&credential));
        if (sc_status_is_ok(status) && sc_str_equal(options->provider_name, sc_str_from_cstr("openrouter"))) {
            status = add_header_with_value(sc_allocator_heap(), &headers, "HTTP-Referer: ", options->openrouter_referer.len == 0 ? sc_str_from_cstr("https://smolclaw.local") : options->openrouter_referer);
        }
        if (sc_status_is_ok(status) && sc_str_equal(options->provider_name, sc_str_from_cstr("openrouter"))) {
            status = add_header_with_value(sc_allocator_heap(), &headers, "X-Title: ", options->openrouter_title.len == 0 ? sc_str_from_cstr("SmolClaw") : options->openrouter_title);
        }
    } else if (sc_status_is_ok(status) && auth == AUTH_ANTHROPIC_KEY) {
        if (str_has_prefix(sc_string_as_str(&credential), "sk-ant-oat")) {
            status = add_header_with_value(sc_allocator_heap(), &headers, "Authorization: Bearer ", sc_string_as_str(&credential));
        } else {
            status = add_header_with_value(sc_allocator_heap(), &headers, "x-api-key: ", sc_string_as_str(&credential));
        }
        if (sc_status_is_ok(status)) {
            status = add_header(&headers, "anthropic-version: 2023-06-01");
        }
    } else if (sc_status_is_ok(status) && auth == AUTH_GEMINI_KEY) {
        status = add_header_with_value(sc_allocator_heap(), &headers, "x-goog-api-key: ", sc_string_as_str(&credential));
    }

    if (sc_status_is_ok(status)) {
        curl_easy_reset(curl);
        (void)curl_easy_setopt(curl, CURLOPT_URL, url.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_json.ptr == nullptr ? "" : request_json.ptr);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request_json.len);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, provider_curl_stream_write_callback);
        (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &stream);
        (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "smolclaw-c/0.1 provider-http");
        (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms < 10000L ? timeout_ms : 10000L);
        (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
        (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        provider_curl_apply_common_options(curl);

        code = curl_easy_perform(curl);
        (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (http_status != nullptr) {
            *http_status = (int)response_code;
        }
        if (!sc_status_is_ok(stream.status)) {
            status = stream.status;
            stream.status = sc_status_ok();
        } else if (code != CURLE_OK) {
            status = sc_status_http("sc.provider_http.stream_request_failed");
        } else if (response_code >= 200 && response_code < 300) {
            status = on_finish(stream_user_data);
        }
    }

    (void)snprintf(response_bytes, sizeof(response_bytes), "%zu", stream.received_bytes);
    (void)snprintf(http_status_text, sizeof(http_status_text), "%ld", response_code);
    (void)snprintf(curl_code_text, sizeof(curl_code_text), "%d", (int)code);
    done_fields[0] = (sc_log_field){.key = "provider", .value = options->provider_name, .secret = false};
    done_fields[1] = (sc_log_field){.key = "method", .value = sc_str_from_cstr("POST"), .secret = false};
    done_fields[2] = (sc_log_field){.key = "url", .value = sc_string_as_str(&redacted_url), .secret = false};
    done_fields[3] = (sc_log_field){.key = "http_status", .value = sc_str_from_cstr(http_status_text), .secret = false};
    done_fields[4] = (sc_log_field){.key = "curl_code", .value = sc_str_from_cstr(curl_code_text), .secret = false};
    done_fields[5] = (sc_log_field){.key = "curl_error", .value = sc_str_from_cstr(curl_easy_strerror(code)), .secret = false};
    done_fields[6] = (sc_log_field){.key = "response_bytes", .value = sc_str_from_cstr(response_bytes), .secret = false};
    done_fields[7] = (sc_log_field){.key = "status", .value = sc_str_from_cstr(sc_status_is_ok(status) ? "ok" : "error"), .secret = false};
    done_fields[8] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
    done_fields[9] = (sc_log_field){.key = "streaming", .value = sc_str_from_cstr("true"), .secret = false};
    sc_log_write(SC_LOG_TRACE, "sc.net", "net.provider.stream.done", done_fields, SC_ARRAY_LEN(done_fields));

    curl_slist_free_all(headers);
    provider_curl_release_handle(curl, shared_curl);
    sc_string_secure_clear(&credential);
    sc_string_clear(&redacted_url);
    return status;
}

static const char *auth_kind_name(auth_kind auth)
{
    switch (auth) {
    case AUTH_NONE:
        return "none";
    case AUTH_OPENAI_BEARER:
        return "bearer";
    case AUTH_ANTHROPIC_KEY:
        return "anthropic_key";
    case AUTH_GEMINI_KEY:
        return "gemini_key";
    case AUTH_AWS_SIGV4:
        return "aws_sigv4";
    }
    return "unknown";
}

static bool provider_curl_code_retryable(CURLcode code)
{
    return code == CURLE_COULDNT_RESOLVE_HOST ||
           code == CURLE_COULDNT_CONNECT ||
           code == CURLE_OPERATION_TIMEDOUT ||
           code == CURLE_RECV_ERROR ||
           code == CURLE_SEND_ERROR ||
           code == CURLE_GOT_NOTHING ||
           code == CURLE_PARTIAL_FILE;
}

static sc_status provider_redact_url_for_log(sc_allocator *alloc, sc_str url, sc_string *out)
{
    static const char marker[] = "[REDACTED]";
    sc_string_builder builder = {0};
    size_t authority_start = 0;
    size_t authority_end = 0;
    size_t at = SIZE_MAX;
    size_t query = SIZE_MAX;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.log_url_invalid_argument");
    }
    if (url.ptr == nullptr) {
        return sc_string_from_cstr(alloc, "", out);
    }
    for (size_t i = 0; i + 2 < url.len; i += 1) {
        if (url.ptr[i] == ':' && url.ptr[i + 1] == '/' && url.ptr[i + 2] == '/') {
            authority_start = i + 3;
            break;
        }
    }
    authority_end = authority_start;
    while (authority_end < url.len && url.ptr[authority_end] != '/' && url.ptr[authority_end] != '?' && url.ptr[authority_end] != '#') {
        if (url.ptr[authority_end] == '@') {
            at = authority_end;
        }
        authority_end += 1;
    }
    for (size_t i = 0; i < url.len; i += 1) {
        if (url.ptr[i] == '?') {
            query = i;
            break;
        }
    }

    sc_string_builder_init(&builder, alloc);
    if (at != SIZE_MAX) {
        status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr, authority_start));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, marker);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr + at, (query == SIZE_MAX ? url.len : query) - at));
        }
    } else {
        status = sc_string_builder_append(&builder, sc_str_from_parts(url.ptr, query == SIZE_MAX ? url.len : query));
    }
    if (sc_status_is_ok(status) && query != SIZE_MAX) {
        status = sc_string_builder_append_cstr(&builder, "?");
    }
    if (sc_status_is_ok(status) && query != SIZE_MAX) {
        status = sc_string_builder_append_cstr(&builder, marker);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status add_header(struct curl_slist **headers, const char *header)
{
    struct curl_slist *next = nullptr;
    if (headers == nullptr || header == nullptr) {
        return sc_status_invalid_argument("sc.provider_http.header_invalid_argument");
    }
    next = curl_slist_append(*headers, header);
    if (next == nullptr) {
        return sc_status_no_memory();
    }
    *headers = next;
    return sc_status_ok();
}

static sc_status add_header_with_value(sc_allocator *alloc,
                                       struct curl_slist **headers,
                                       const char *name,
                                       sc_str value)
{
    sc_string_builder builder = {0};
    sc_string header = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, name);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &header);
    }
    if (sc_status_is_ok(status)) {
        status = add_header(headers, header.ptr);
    }
    sc_string_secure_clear(&header);
    sc_string_builder_clear(&builder);
    return status;
}

static size_t provider_curl_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    http_response_buffer *response = userdata;
    size_t bytes = size * nmemb;
    size_t next_len = 0;
    sc_status status = sc_status_ok();

    if (response == nullptr || ptr == nullptr) {
        return 0;
    }
    if (sc_size_add_overflow(response->bytes.len, bytes, &next_len) || next_len > response->max_bytes) {
        response->too_large = true;
        return 0;
    }
    status = sc_bytes_append(&response->bytes, sc_buf_from_parts(ptr, bytes));
    return sc_status_is_ok(status) ? bytes : 0;
}

static size_t provider_curl_stream_write_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    provider_stream_curl_context *stream = userdata;
    size_t bytes = size * nmemb;

    if (stream == nullptr || ptr == nullptr) {
        return 0;
    }
    stream->received_bytes += bytes;
    stream->status = stream->on_chunk(stream->user_data, sc_str_from_parts(ptr, bytes));
    return sc_status_is_ok(stream->status) ? bytes : 0;
}

static sc_status provider_sse_stream_push(void *user_data, sc_str chunk)
{
    provider_sse_stream_state *state = user_data;
    sc_string_builder builder = {0};
    sc_string combined = {0};
    size_t offset = 0;
    sc_status status = sc_status_ok();

    if (state == nullptr || state->callback == nullptr || state->parse == nullptr || state->failed) {
        return sc_status_invalid_argument("sc.provider_sse.stream_invalid_argument");
    }
    if (chunk.len == 0) {
        return sc_status_ok();
    }
    if (state->partial.len + chunk.len > state->max_partial_bytes) {
        state->failed = true;
        return sc_status_invalid_argument("sc.provider_sse.partial_too_large");
    }

    sc_string_builder_init(&builder, state->alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(&state->partial));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, chunk);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &combined);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (!sc_status_is_ok(status)) {
        state->failed = true;
        return status;
    }
    sc_string_clear(&state->partial);

    while (sc_status_is_ok(status) && offset < combined.len) {
        size_t event_len = 0;
        bool found = false;
        while (offset + event_len + 1 < combined.len) {
            if (combined.ptr[offset + event_len] == '\n' &&
                combined.ptr[offset + event_len + 1] == '\n') {
                found = true;
                break;
            }
            event_len += 1;
        }
        if (!found) {
            status = sc_string_from_str(state->alloc,
                                        sc_str_from_parts(combined.ptr + offset, combined.len - offset),
                                        &state->partial);
            break;
        }
        if (event_len > state->max_partial_bytes) {
            status = sc_status_invalid_argument("sc.provider_sse.event_too_large");
            break;
        }
        if (event_len > 0) {
            sc_provider_stream_event event = {0};
            status = state->parse(state->alloc, sc_str_from_parts(combined.ptr + offset, event_len), &event);
            if (sc_status_is_ok(status)) {
                state->done = event.type == SC_PROVIDER_STREAM_DONE || state->done;
                if (event.type != SC_PROVIDER_STREAM_DELTA || event.text.len > 0) {
                    status = state->callback(state->callback_user_data, &event);
                }
            }
            sc_provider_stream_event_clear(&event);
        }
        offset += event_len + 2;
        while (offset < combined.len && (combined.ptr[offset] == '\n' || combined.ptr[offset] == '\r')) {
            offset += 1;
        }
    }
    sc_string_clear(&combined);
    if (!sc_status_is_ok(status)) {
        state->failed = true;
    }
    return status;
}

static sc_status provider_sse_stream_finish(void *user_data)
{
    provider_sse_stream_state *state = user_data;
    sc_status status = sc_status_ok();

    if (state == nullptr || state->callback == nullptr || state->failed) {
        return sc_status_invalid_argument("sc.provider_sse.finish_invalid_argument");
    }
    if (state->partial.len > 0) {
        state->failed = true;
        return sc_status_parse("sc.provider_sse.truncated_event");
    }
    if (!state->done) {
        sc_provider_stream_event event = {
            .struct_size = sizeof(event),
            .type = SC_PROVIDER_STREAM_DONE,
        };
        status = state->callback(state->callback_user_data, &event);
        if (sc_status_is_ok(status)) {
            state->done = true;
        }
    }
    return status;
}

static void provider_sse_stream_clear(provider_sse_stream_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_string_clear(&state->partial);
    *state = (provider_sse_stream_state){0};
}

static sc_status ollama_stream_push(void *user_data, sc_str chunk)
{
    ollama_stream_state *state = user_data;
    sc_string_builder builder = {0};
    sc_string combined = {0};
    size_t offset = 0;
    sc_status status = sc_status_ok();

    if (state == nullptr || state->callback == nullptr || state->failed) {
        return sc_status_invalid_argument("sc.provider_ollama.stream_invalid_argument");
    }
    if (chunk.len == 0) {
        return sc_status_ok();
    }
    if (state->partial.len + chunk.len > state->max_partial_bytes) {
        state->failed = true;
        return sc_status_invalid_argument("sc.provider_ollama.partial_too_large");
    }

    sc_string_builder_init(&builder, state->alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(&state->partial));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, chunk);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &combined);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (!sc_status_is_ok(status)) {
        state->failed = true;
        return status;
    }
    sc_string_clear(&state->partial);

    while (sc_status_is_ok(status) && offset < combined.len) {
        size_t line_len = 0;
        while (offset + line_len < combined.len && combined.ptr[offset + line_len] != '\n') {
            line_len += 1;
        }
        if (offset + line_len >= combined.len) {
            status = sc_string_from_str(state->alloc,
                                        sc_str_from_parts(combined.ptr + offset, combined.len - offset),
                                        &state->partial);
            break;
        }
        if (line_len > 0 && combined.ptr[offset + line_len - 1] == '\r') {
            line_len -= 1;
        }
        if (line_len > 0) {
            status = ollama_stream_emit_line(state, sc_str_from_parts(combined.ptr + offset, line_len));
        }
        offset += line_len;
        while (offset < combined.len && (combined.ptr[offset] == '\r' || combined.ptr[offset] == '\n')) {
            offset += 1;
        }
    }
    sc_string_clear(&combined);
    if (!sc_status_is_ok(status)) {
        state->failed = true;
    }
    return status;
}

static sc_status ollama_stream_finish(void *user_data)
{
    ollama_stream_state *state = user_data;
    sc_status status = sc_status_ok();

    if (state == nullptr || state->callback == nullptr || state->failed) {
        return sc_status_invalid_argument("sc.provider_ollama.finish_invalid_argument");
    }
    if (state->partial.len > 0) {
        status = ollama_stream_emit_line(state, sc_string_as_str(&state->partial));
        sc_string_clear(&state->partial);
    }
    if (sc_status_is_ok(status) && !state->done) {
        sc_provider_stream_event event = {
            .struct_size = sizeof(event),
            .type = SC_PROVIDER_STREAM_DONE,
        };
        status = state->callback(state->callback_user_data, &event);
        if (sc_status_is_ok(status)) {
            state->done = true;
        }
    }
    return status;
}

static void ollama_stream_clear(ollama_stream_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_string_clear(&state->partial);
    *state = (ollama_stream_state){0};
}

static sc_status ollama_stream_emit_line(ollama_stream_state *state, sc_str line)
{
    sc_provider_response response = {0};
    sc_provider_stream_event event = {0};
    sc_status status = sc_provider_ollama_parse_response(state->alloc, line, &response);

    if (sc_status_is_ok(status)) {
        status = emit_response_as_stream(state->alloc, &response, state->callback, state->callback_user_data);
    }
    if (sc_status_is_ok(status) && response.finish_reason.len > 0 && !state->done) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DONE};
        status = state->callback(state->callback_user_data, &event);
        if (sc_status_is_ok(status)) {
            state->done = true;
        }
    }
    sc_provider_stream_event_clear(&event);
    sc_provider_response_clear(&response);
    return status;
}
#endif
