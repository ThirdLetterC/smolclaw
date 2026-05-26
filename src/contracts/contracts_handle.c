#include "contracts/contracts_internal.h"

#include "sc/contract.h"

sc_status sc_contract_handle_new(sc_allocator *alloc, const void *vtab, void *impl, size_t size, void **out)
{
    sc_contract_handle *handle = nullptr;

    if (out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.contract.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    handle = sc_alloc(alloc, size, _Alignof(sc_contract_handle));
    if (handle == nullptr) {
        return sc_status_no_memory();
    }
    *handle = (sc_contract_handle){
        .alloc = alloc,
        .vtab = vtab,
        .impl = impl,
    };
    *out = handle;
    return sc_status_ok();
}

void sc_contract_handle_destroy(sc_contract_handle *handle, size_t size, void (*destroy)(void *impl))
{
    sc_allocator *alloc = nullptr;

    if (handle == nullptr) {
        return;
    }
    alloc = handle->alloc == nullptr ? sc_allocator_heap() : handle->alloc;
    if (destroy != nullptr) {
        destroy(handle->impl);
    }
    sc_free(alloc, handle, size, _Alignof(sc_contract_handle));
}

bool sc_contract_common_vtab_valid(size_t struct_size,
                                   uint32_t abi_major,
                                   const char *name,
                                   bool required_present,
                                   bool destroy_present)
{
    return struct_size > 0 && abi_major == SC_ABI_VERSION_MAJOR &&
           sc_contract_name_is_valid(sc_str_from_cstr(name)) && required_present && destroy_present;
}
