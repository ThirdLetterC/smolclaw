#include "sc/provider.h"

#include "sc/log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct reliable_provider {
    sc_allocator *alloc;
    sc_vec providers;
    uint32_t max_retries;
    uint32_t retry_backoff_ms;
} reliable_provider;

static sc_status reliable_generate(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_response *out);
static sc_status reliable_stream(void *impl,
                                 const sc_provider_request *request,
                                 sc_allocator *alloc,
                                 sc_provider_stream_callback callback,
                                 void *callback_user_data);
static void reliable_destroy(void *impl);
static sc_status reliable_call_generate(const reliable_provider *provider,
                                        sc_provider *candidate,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out);
static sc_status reliable_call_stream(const reliable_provider *provider,
                                      sc_provider *candidate,
                                      const sc_provider_request *request,
                                      sc_allocator *alloc,
                                      sc_provider_stream_callback callback,
                                      void *callback_user_data);
static bool provider_matches_model(sc_provider *provider, sc_str model);
static bool str_contains_cstr(sc_str haystack, const char *needle);
static void reliable_log_attempt(sc_provider *provider, uint32_t attempt, sc_status status, const char *event_key);
static void sleep_ms(uint32_t delay_ms);

static const sc_provider_vtab reliable_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "reliable",
    .display_name = "Reliable provider",
    .feature_flag = "SC_PROVIDER_RELIABLE",
    .capabilities = SC_CONTRACT_CAP_STREAMING,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = reliable_generate,
    .stream = reliable_stream,
    .destroy = reliable_destroy,
    .description_key = "sc.provider.reliable.description",
    .config_schema_ref = "sc.schema.provider.reliable.v1",
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy)},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM,
};

sc_status sc_provider_reliable_new(sc_allocator *alloc,
                                   sc_provider **providers,
                                   size_t provider_count,
                                   sc_provider **out)
{
    return sc_provider_reliable_new_with_options(alloc, providers, provider_count, 2, 0, out);
}

sc_status sc_provider_reliable_new_with_options(sc_allocator *alloc,
                                                sc_provider **providers,
                                                size_t provider_count,
                                                uint32_t max_retries,
                                                uint32_t retry_backoff_ms,
                                                sc_provider **out)
{
    reliable_provider *impl = nullptr;
    sc_status status;

    if (out == nullptr || providers == nullptr || provider_count == 0) {
        return sc_status_invalid_argument("sc.provider_reliable.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(reliable_provider));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (reliable_provider){
        .alloc = alloc,
        .max_retries = max_retries,
        .retry_backoff_ms = retry_backoff_ms,
    };
    sc_vec_init(&impl->providers, alloc, sizeof(sc_provider *));
    status = sc_status_ok();
    for (size_t i = 0; sc_status_is_ok(status) && i < provider_count; i += 1) {
        if (providers[i] == nullptr) {
            status = sc_status_invalid_argument("sc.provider_reliable.null_provider");
        } else {
            status = sc_vec_push(&impl->providers, &providers[i]);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &reliable_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        reliable_destroy(impl);
    }
    return status;
}

static sc_status reliable_generate(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_response *out)
{
    reliable_provider *provider = impl;
    sc_status last = sc_status_invalid_argument("sc.provider_reliable.empty");
    bool any_model_match = false;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_reliable.invalid_argument");
    }

    for (size_t i = 0; i < provider->providers.len; i += 1) {
        sc_provider **candidate_slot = sc_vec_at(&provider->providers, i);
        if (candidate_slot != nullptr && provider_matches_model(*candidate_slot, request->model)) {
            any_model_match = true;
            break;
        }
    }
    for (size_t i = 0; i < provider->providers.len; i += 1) {
        sc_provider **candidate_slot = sc_vec_at(&provider->providers, i);
        sc_status status;
        if (any_model_match && (candidate_slot == nullptr || !provider_matches_model(*candidate_slot, request->model))) {
            continue;
        }
        sc_status_clear(&last);
        status = reliable_call_generate(provider, *candidate_slot, request, alloc, out);
        if (sc_status_is_ok(status)) {
            return status;
        }
        if (status.code == SC_ERR_SECURITY_DENIED ||
            status.code == SC_ERR_CANCELLED ||
            status.code == SC_ERR_INVALID_ARGUMENT ||
            status.code == SC_ERR_PARSE) {
            last = status;
            break;
        }
        reliable_log_attempt(*candidate_slot, provider->max_retries + 1U, status, "provider.reliable.exhausted");
        last = status;
    }
    return last;
}

static sc_status reliable_stream(void *impl,
                                 const sc_provider_request *request,
                                 sc_allocator *alloc,
                                 sc_provider_stream_callback callback,
                                 void *callback_user_data)
{
    reliable_provider *provider = impl;
    sc_status last = sc_status_invalid_argument("sc.provider_reliable.empty");
    bool any_model_match = false;

    if (provider == nullptr || request == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_reliable.stream_invalid_argument");
    }
    for (size_t i = 0; i < provider->providers.len; i += 1) {
        sc_provider **candidate_slot = sc_vec_at(&provider->providers, i);
        if (candidate_slot != nullptr && provider_matches_model(*candidate_slot, request->model)) {
            any_model_match = true;
            break;
        }
    }
    for (size_t i = 0; i < provider->providers.len; i += 1) {
        sc_provider **candidate_slot = sc_vec_at(&provider->providers, i);
        sc_status status;
        if (candidate_slot == nullptr) {
            continue;
        }
        if (any_model_match && !provider_matches_model(*candidate_slot, request->model)) {
            continue;
        }
        sc_status_clear(&last);
        status = reliable_call_stream(provider, *candidate_slot, request, alloc, callback, callback_user_data);
        if (sc_status_is_ok(status)) {
            return status;
        }
        if (status.code == SC_ERR_SECURITY_DENIED ||
            status.code == SC_ERR_CANCELLED ||
            status.code == SC_ERR_INVALID_ARGUMENT ||
            status.code == SC_ERR_PARSE) {
            last = status;
            break;
        }
        reliable_log_attempt(*candidate_slot, provider->max_retries + 1U, status, "provider.reliable.exhausted");
        last = status;
    }
    return last;
}

static sc_status reliable_call_generate(const reliable_provider *provider,
                                        sc_provider *candidate,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out)
{
    sc_status status = sc_status_invalid_argument("sc.provider_reliable.null_provider");

    if (provider == nullptr || candidate == nullptr) {
        return status;
    }
    for (uint32_t attempt = 1; attempt <= provider->max_retries + 1U; attempt += 1) {
        status = sc_provider_generate(candidate, request, alloc, out);
        reliable_log_attempt(candidate, attempt, status, sc_status_is_ok(status) ? "provider.reliable.selected" : "provider.reliable.attempt");
        if (sc_status_is_ok(status) || !sc_provider_should_retry(status, 0) || attempt > provider->max_retries) {
            break;
        }
        sc_status_clear(&status);
        sleep_ms(provider->retry_backoff_ms * attempt);
    }
    return status;
}

static sc_status reliable_call_stream(const reliable_provider *provider,
                                      sc_provider *candidate,
                                      const sc_provider_request *request,
                                      sc_allocator *alloc,
                                      sc_provider_stream_callback callback,
                                      void *callback_user_data)
{
    sc_status status = sc_status_invalid_argument("sc.provider_reliable.null_provider");

    if (provider == nullptr || candidate == nullptr) {
        return status;
    }
    for (uint32_t attempt = 1; attempt <= provider->max_retries + 1U; attempt += 1) {
        status = sc_provider_stream(candidate, request, alloc, callback, callback_user_data);
        reliable_log_attempt(candidate, attempt, status, sc_status_is_ok(status) ? "provider.reliable.selected" : "provider.reliable.attempt");
        if (sc_status_is_ok(status) || !sc_provider_should_retry(status, 0) || attempt > provider->max_retries) {
            break;
        }
        sc_status_clear(&status);
        sleep_ms(provider->retry_backoff_ms * attempt);
    }
    return status;
}

static bool provider_matches_model(sc_provider *provider, sc_str model)
{
    const sc_provider_vtab *vtab = sc_provider_vtab_of(provider);
    return vtab != nullptr && vtab->name != nullptr && str_contains_cstr(model, vtab->name);
}

static bool str_contains_cstr(sc_str haystack, const char *needle)
{
    size_t needle_len = needle == nullptr ? 0 : strlen(needle);
    if (haystack.ptr == nullptr || haystack.len == 0 || needle_len == 0 || needle_len > haystack.len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack.len; i += 1) {
        if (memcmp(haystack.ptr + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void reliable_log_attempt(sc_provider *provider, uint32_t attempt, sc_status status, const char *event_key)
{
    const sc_provider_vtab *vtab = sc_provider_vtab_of(provider);
    char attempt_text[32] = {0};
    sc_log_field fields[4] = {0};

    (void)snprintf(attempt_text, sizeof(attempt_text), "%u", attempt);
    fields[0] = (sc_log_field){.key = "provider", .value = sc_str_from_cstr(vtab == nullptr || vtab->name == nullptr ? "" : vtab->name), .secret = false};
    fields[1] = (sc_log_field){.key = "attempt", .value = sc_str_from_cstr(attempt_text), .secret = false};
    fields[2] = (sc_log_field){.key = "status", .value = sc_str_from_cstr(sc_status_is_ok(status) ? "ok" : "error"), .secret = false};
    fields[3] = (sc_log_field){.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key), .secret = false};
    sc_log_write(sc_status_is_ok(status) ? SC_LOG_INFO : SC_LOG_WARN,
                 "sc.provider",
                 event_key,
                 fields,
                 SC_ARRAY_LEN(fields));
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

static void reliable_destroy(void *impl)
{
    reliable_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_vec_clear(&provider->providers);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(reliable_provider));
}
