#include "compat/c23_keyword_compat.h"

#include "sc/url.h"

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    constexpr size_t max_input_size = 64U * 1024U;
    sc_url url = {0};

    if (data == nullptr || size == 0 || size > max_input_size) {
        return 0;
    }
    sc_status status = sc_url_parse(sc_allocator_heap(),
                                    sc_str_from_parts((const char *)data, size),
                                    &url);
    if (sc_status_is_ok(status)) {
        (void)sc_url_has_credentials(&url);
        (void)sc_url_host_is_private_address(&url);
        sc_url_clear(&url);
    } else {
        sc_status_clear(&status);
    }
    return 0;
}
