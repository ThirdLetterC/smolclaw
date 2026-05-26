#include "sc/provider.h"

#include <string.h>

typedef struct example_provider {
    sc_allocator *alloc;
    sc_string fixed_text;
} example_provider;

static sc_status example_generate(void *impl,
                                  const sc_provider_request *request,
                                  sc_allocator *alloc,
                                  sc_provider_response *out);
static void example_destroy(void *impl);

static const sc_provider_vtab example_provider_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "example-provider",
    .display_name = "Example Provider",
    .feature_flag = "SC_PROVIDER_EXAMPLE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = example_generate,
    .stream = nullptr,
    .destroy = example_destroy,
    .description_key = "sc.provider.example.description",
    .config_schema_ref = "providers.models.<name>",
    .required_secret_keys = nullptr,
    .required_secret_key_count = 0,
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy)},
    .provider_modes = SC_PROVIDER_MODE_CHAT,
};

static sc_status example_provider_new(sc_allocator *alloc, sc_provider **out)
{
    example_provider *impl = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider.example.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(example_provider));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (example_provider){.alloc = alloc};

    status = sc_string_from_cstr(alloc, "example response", &impl->fixed_text);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &example_provider_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        example_destroy(impl);
    }
    return status;
}

static sc_status example_generate(void *impl,
                                  const sc_provider_request *request,
                                  sc_allocator *alloc,
                                  sc_provider_response *out)
{
    const example_provider *provider = impl;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider.example.generate_invalid_argument");
    }

    sc_provider_response_init(out, alloc);
    return sc_string_from_str(alloc, sc_string_as_str(&provider->fixed_text), &out->text);
}

static void example_destroy(void *impl)
{
    example_provider *provider = impl;

    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->fixed_text);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(example_provider));
}

int main()
{
    sc_provider *provider = nullptr;
    sc_provider_response response = {0};
    sc_provider_request request = {
        .struct_size = sizeof(request),
        .model = sc_str_from_cstr("example-model"),
        .prompt = sc_str_from_cstr("hello"),
    };
    sc_status status = example_provider_new(sc_allocator_heap(), &provider);
    int exit_code = 0;

    if (sc_status_is_ok(status)) {
        status = sc_provider_generate(provider, &request, sc_allocator_heap(), &response);
    }
    if (!sc_status_is_ok(status) || response.text.ptr == nullptr || strcmp(response.text.ptr, "example response") != 0) {
        exit_code = 1;
    }

    sc_status_clear(&status);
    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);
    return exit_code;
}
