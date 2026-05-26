#include "sc/provider.h"

static sc_status unsupported_provider(sc_provider **out, const char *error_key)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_unsupported.invalid_argument");
    }
    *out = nullptr;
    return sc_status_unsupported(error_key);
}

sc_status sc_provider_copilot_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    (void)alloc;
    (void)options;
    return unsupported_provider(out, "sc.provider_copilot.unsupported_oauth");
}

sc_status sc_provider_telnyx_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    (void)alloc;
    (void)options;
    return unsupported_provider(out, "sc.provider_telnyx.unsupported_voice");
}

sc_status sc_provider_kilocli_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    (void)alloc;
    (void)options;
    return unsupported_provider(out, "sc.provider_kilocli.unsupported_process");
}
