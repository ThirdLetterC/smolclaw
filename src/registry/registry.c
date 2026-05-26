#include "sc/registry.h"

#include <string.h>

#define DEFINE_REGISTRY(prefix, registry_type, entry_type, vtab_type, factory_type, handle_type, valid_fn)             \
    void prefix##_init(registry_type *registry, sc_allocator *alloc)                                                   \
    {                                                                                                                  \
        if (registry == nullptr) {                                                                                        \
            return;                                                                                                    \
        }                                                                                                              \
        sc_vec_init(&registry->entries, alloc, sizeof(entry_type));                                                     \
    }                                                                                                                  \
                                                                                                                       \
    sc_status prefix##_register(registry_type *registry, const vtab_type *vtab, factory_type factory)                  \
    {                                                                                                                  \
        entry_type entry = {0};                                                                                        \
        if (registry == nullptr || factory == nullptr || !valid_fn(vtab)) {                                                   \
            return sc_status_invalid_argument("sc.registry.invalid_entry");                                             \
        }                                                                                                              \
        if (prefix##_find(registry, sc_str_from_cstr(vtab->name)) != nullptr) {                                            \
            return sc_status_invalid_argument("sc.registry.duplicate_name");                                            \
        }                                                                                                              \
        entry.vtab = vtab;                                                                                             \
        entry.factory = factory;                                                                                       \
        return sc_vec_push(&registry->entries, &entry);                                                                 \
    }                                                                                                                  \
                                                                                                                       \
    const entry_type *prefix##_find(const registry_type *registry, sc_str name)                                         \
    {                                                                                                                  \
        if (registry == nullptr || (name.len > 0 && name.ptr == nullptr)) {                                                   \
            return nullptr;                                                                                               \
        }                                                                                                              \
        for (size_t i = 0; i < registry->entries.len; ++i) {                                                           \
            const entry_type *entry = sc_vec_at_const(&registry->entries, i);                                           \
            if (entry != nullptr && strlen(entry->vtab->name) == name.len &&                                               \
                memcmp(entry->vtab->name, name.ptr, name.len) == 0) {                                                   \
                return entry;                                                                                          \
            }                                                                                                          \
        }                                                                                                              \
        return nullptr;                                                                                                   \
    }                                                                                                                  \
                                                                                                                       \
    const entry_type *prefix##_at(const registry_type *registry, size_t index)                                          \
    {                                                                                                                  \
        return registry == nullptr ? nullptr : sc_vec_at_const(&registry->entries, index);                                    \
    }                                                                                                                  \
                                                                                                                       \
    size_t prefix##_len(const registry_type *registry)                                                                 \
    {                                                                                                                  \
        return registry == nullptr ? 0 : registry->entries.len;                                                            \
    }                                                                                                                  \
                                                                                                                       \
    sc_status prefix##_create(const registry_type *registry, sc_str name, sc_allocator *alloc, handle_type **out)       \
    {                                                                                                                  \
        const entry_type *entry = nullptr;                                                                                \
        if (out == nullptr) {                                                                                             \
            return sc_status_invalid_argument("sc.registry.invalid_argument");                                          \
        }                                                                                                              \
        entry = prefix##_find(registry, name);                                                                         \
        if (entry == nullptr) {                                                                                           \
            return sc_status_invalid_argument("sc.registry.not_found");                                                 \
        }                                                                                                              \
        return entry->factory(alloc, out);                                                                             \
    }                                                                                                                  \
                                                                                                                       \
    void prefix##_clear(registry_type *registry)                                                                       \
    {                                                                                                                  \
        if (registry == nullptr) {                                                                                        \
            return;                                                                                                    \
        }                                                                                                              \
        sc_vec_clear(&registry->entries);                                                                              \
    }

DEFINE_REGISTRY(sc_provider_registry,
                sc_provider_registry,
                sc_provider_registry_entry,
                sc_provider_vtab,
                sc_provider_factory,
                sc_provider,
                sc_provider_vtab_valid)
DEFINE_REGISTRY(sc_channel_registry,
                sc_channel_registry,
                sc_channel_registry_entry,
                sc_channel_vtab,
                sc_channel_factory,
                sc_channel,
                sc_channel_vtab_valid)
DEFINE_REGISTRY(sc_tool_registry,
                sc_tool_registry,
                sc_tool_registry_entry,
                sc_tool_vtab,
                sc_tool_factory,
                sc_tool,
                sc_tool_vtab_valid)
DEFINE_REGISTRY(sc_memory_registry,
                sc_memory_registry,
                sc_memory_registry_entry,
                sc_memory_vtab,
                sc_memory_factory,
                sc_memory,
                sc_memory_vtab_valid)
DEFINE_REGISTRY(sc_peripheral_registry,
                sc_peripheral_registry,
                sc_peripheral_registry_entry,
                sc_peripheral_vtab,
                sc_peripheral_factory,
                sc_peripheral,
                sc_peripheral_vtab_valid)

void sc_observer_list_init(sc_observer_list *list, sc_allocator *alloc)
{
    if (list == nullptr) {
        return;
    }
    sc_ptr_vec_init(&list->observers, alloc);
}

sc_status sc_observer_list_add(sc_observer_list *list, sc_observer *observer)
{
    if (list == nullptr || observer == nullptr) {
        return sc_status_invalid_argument("sc.observer_list.invalid_argument");
    }
    return sc_ptr_vec_push(&list->observers, observer);
}

sc_status sc_observer_list_emit(sc_observer_list *list, const sc_observer_event *event)
{
    if (list == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.observer_list.invalid_argument");
    }
    for (size_t i = 0; i < list->observers.inner.len; ++i) {
        sc_observer *observer = sc_ptr_vec_at(&list->observers, i);
        sc_status status = sc_observer_emit(observer, event);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    return sc_status_ok();
}

sc_status sc_observer_list_emit_isolated(sc_observer_list *list,
                                         const sc_observer_event *event,
                                         size_t *failure_count)
{
    size_t failures = 0;

    if (list == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.observer_list.invalid_argument");
    }
    for (size_t i = 0; i < list->observers.inner.len; ++i) {
        sc_observer *observer = sc_ptr_vec_at(&list->observers, i);
        sc_status status = sc_observer_emit(observer, event);
        if (!sc_status_is_ok(status)) {
            failures += 1;
        }
        sc_status_clear(&status);
    }
    if (failure_count != nullptr) {
        *failure_count = failures;
    }
    return sc_status_ok();
}

sc_status sc_observer_list_flush(sc_observer_list *list)
{
    if (list == nullptr) {
        return sc_status_invalid_argument("sc.observer_list.invalid_argument");
    }
    for (size_t i = 0; i < list->observers.inner.len; ++i) {
        sc_observer *observer = sc_ptr_vec_at(&list->observers, i);
        sc_status status = sc_observer_flush(observer);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    return sc_status_ok();
}

sc_status sc_observer_list_flush_isolated(sc_observer_list *list, size_t *failure_count)
{
    size_t failures = 0;

    if (list == nullptr) {
        return sc_status_invalid_argument("sc.observer_list.invalid_argument");
    }
    for (size_t i = 0; i < list->observers.inner.len; ++i) {
        sc_observer *observer = sc_ptr_vec_at(&list->observers, i);
        sc_status status = sc_observer_flush(observer);
        if (!sc_status_is_ok(status)) {
            failures += 1;
        }
        sc_status_clear(&status);
    }
    if (failure_count != nullptr) {
        *failure_count = failures;
    }
    return sc_status_ok();
}

size_t sc_observer_list_len(const sc_observer_list *list)
{
    return list == nullptr ? 0 : list->observers.inner.len;
}

void sc_observer_list_clear(sc_observer_list *list)
{
    if (list == nullptr) {
        return;
    }
    for (size_t i = 0; i < list->observers.inner.len; ++i) {
        sc_observer *observer = sc_ptr_vec_at(&list->observers, i);
        sc_observer_destroy(observer);
    }
    sc_ptr_vec_clear(&list->observers);
}

#undef DEFINE_REGISTRY
