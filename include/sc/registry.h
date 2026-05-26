#pragma once

#include "sc/channel.h"
#include "sc/memory.h"
#include "sc/observer.h"
#include "sc/peripheral.h"
#include "sc/provider.h"
#include "sc/tool.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

/*
 * Registry entries borrow const vtables and factory function pointers. Registries
 * do not own handles created from factories. sc_observer_list is an owned list:
 * sc_observer_list_clear destroys each observer exactly once.
 */
typedef sc_status (*sc_provider_factory)(sc_allocator *alloc, sc_provider **out);
typedef sc_status (*sc_channel_factory)(sc_allocator *alloc, sc_channel **out);
typedef sc_status (*sc_tool_factory)(sc_allocator *alloc, sc_tool **out);
typedef sc_status (*sc_memory_factory)(sc_allocator *alloc, sc_memory **out);
typedef sc_status (*sc_peripheral_factory)(sc_allocator *alloc, sc_peripheral **out);

typedef struct sc_provider_registry_entry {
    const sc_provider_vtab *vtab;
    sc_provider_factory factory;
} sc_provider_registry_entry;

typedef struct sc_channel_registry_entry {
    const sc_channel_vtab *vtab;
    sc_channel_factory factory;
} sc_channel_registry_entry;

typedef struct sc_tool_registry_entry {
    const sc_tool_vtab *vtab;
    sc_tool_factory factory;
} sc_tool_registry_entry;

typedef struct sc_memory_registry_entry {
    const sc_memory_vtab *vtab;
    sc_memory_factory factory;
} sc_memory_registry_entry;

typedef struct sc_peripheral_registry_entry {
    const sc_peripheral_vtab *vtab;
    sc_peripheral_factory factory;
} sc_peripheral_registry_entry;

typedef struct sc_provider_registry {
    sc_vec entries;
} sc_provider_registry;

typedef struct sc_channel_registry {
    sc_vec entries;
} sc_channel_registry;

typedef struct sc_tool_registry {
    sc_vec entries;
} sc_tool_registry;

typedef struct sc_memory_registry {
    sc_vec entries;
} sc_memory_registry;

typedef struct sc_peripheral_registry {
    sc_vec entries;
} sc_peripheral_registry;

typedef struct sc_observer_list {
    sc_ptr_vec observers;
} sc_observer_list;

void sc_provider_registry_init(sc_provider_registry *registry, sc_allocator *alloc);
sc_status sc_provider_registry_register(sc_provider_registry *registry,
                                        const sc_provider_vtab *vtab,
                                        sc_provider_factory factory);
const sc_provider_registry_entry *sc_provider_registry_find(const sc_provider_registry *registry, sc_str name);
const sc_provider_registry_entry *sc_provider_registry_at(const sc_provider_registry *registry, size_t index);
size_t sc_provider_registry_len(const sc_provider_registry *registry);
sc_status sc_provider_registry_create(const sc_provider_registry *registry,
                                      sc_str name,
                                      sc_allocator *alloc,
                                      sc_provider **out);
void sc_provider_registry_clear(sc_provider_registry *registry);

void sc_channel_registry_init(sc_channel_registry *registry, sc_allocator *alloc);
sc_status sc_channel_registry_register(sc_channel_registry *registry,
                                       const sc_channel_vtab *vtab,
                                       sc_channel_factory factory);
const sc_channel_registry_entry *sc_channel_registry_find(const sc_channel_registry *registry, sc_str name);
const sc_channel_registry_entry *sc_channel_registry_at(const sc_channel_registry *registry, size_t index);
size_t sc_channel_registry_len(const sc_channel_registry *registry);
sc_status sc_channel_registry_create(const sc_channel_registry *registry,
                                     sc_str name,
                                     sc_allocator *alloc,
                                     sc_channel **out);
void sc_channel_registry_clear(sc_channel_registry *registry);

void sc_tool_registry_init(sc_tool_registry *registry, sc_allocator *alloc);
sc_status sc_tool_registry_register(sc_tool_registry *registry, const sc_tool_vtab *vtab, sc_tool_factory factory);
const sc_tool_registry_entry *sc_tool_registry_find(const sc_tool_registry *registry, sc_str name);
const sc_tool_registry_entry *sc_tool_registry_at(const sc_tool_registry *registry, size_t index);
size_t sc_tool_registry_len(const sc_tool_registry *registry);
sc_status sc_tool_registry_create(const sc_tool_registry *registry, sc_str name, sc_allocator *alloc, sc_tool **out);
void sc_tool_registry_clear(sc_tool_registry *registry);

void sc_memory_registry_init(sc_memory_registry *registry, sc_allocator *alloc);
sc_status sc_memory_registry_register(sc_memory_registry *registry,
                                      const sc_memory_vtab *vtab,
                                      sc_memory_factory factory);
const sc_memory_registry_entry *sc_memory_registry_find(const sc_memory_registry *registry, sc_str name);
const sc_memory_registry_entry *sc_memory_registry_at(const sc_memory_registry *registry, size_t index);
size_t sc_memory_registry_len(const sc_memory_registry *registry);
sc_status sc_memory_registry_create(const sc_memory_registry *registry,
                                    sc_str name,
                                    sc_allocator *alloc,
                                    sc_memory **out);
void sc_memory_registry_clear(sc_memory_registry *registry);

void sc_peripheral_registry_init(sc_peripheral_registry *registry, sc_allocator *alloc);
sc_status sc_peripheral_registry_register(sc_peripheral_registry *registry,
                                          const sc_peripheral_vtab *vtab,
                                          sc_peripheral_factory factory);
const sc_peripheral_registry_entry *sc_peripheral_registry_find(const sc_peripheral_registry *registry, sc_str name);
const sc_peripheral_registry_entry *sc_peripheral_registry_at(const sc_peripheral_registry *registry, size_t index);
size_t sc_peripheral_registry_len(const sc_peripheral_registry *registry);
sc_status sc_peripheral_registry_create(const sc_peripheral_registry *registry,
                                        sc_str name,
                                        sc_allocator *alloc,
                                        sc_peripheral **out);
void sc_peripheral_registry_clear(sc_peripheral_registry *registry);

void sc_observer_list_init(sc_observer_list *list, sc_allocator *alloc);
sc_status sc_observer_list_add(sc_observer_list *list, sc_observer *observer);
sc_status sc_observer_list_emit(sc_observer_list *list, const sc_observer_event *event);
sc_status sc_observer_list_emit_isolated(sc_observer_list *list,
                                         const sc_observer_event *event,
                                         size_t *failure_count);
sc_status sc_observer_list_flush(sc_observer_list *list);
sc_status sc_observer_list_flush_isolated(sc_observer_list *list, size_t *failure_count);
size_t sc_observer_list_len(const sc_observer_list *list);
void sc_observer_list_clear(sc_observer_list *list);

SC_END_DECLS
