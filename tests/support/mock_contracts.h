#pragma once

#include "sc/channel.h"
#include "sc/memory.h"
#include "sc/observer.h"
#include "sc/provider.h"
#include "sc/tool.h"

typedef struct sc_mock_counts {
    int generate_calls;
    int invoke_calls;
    int send_calls;
    int memory_put_calls;
    int memory_get_calls;
    int emit_calls;
    int destroy_calls;
} sc_mock_counts;

typedef struct sc_mock_provider_state {
    sc_allocator *alloc;
    sc_mock_counts *counts;
    sc_string text;
} sc_mock_provider_state;

typedef struct sc_mock_tool_state {
    sc_allocator *alloc;
    sc_mock_counts *counts;
    sc_string output;
} sc_mock_tool_state;

typedef struct sc_mock_channel_state {
    sc_allocator *alloc;
    sc_mock_counts *counts;
    sc_string last_message;
} sc_mock_channel_state;

typedef struct sc_mock_memory_state {
    sc_allocator *alloc;
    sc_mock_counts *counts;
    sc_string key;
    sc_string value;
} sc_mock_memory_state;

typedef struct sc_mock_observer_state {
    sc_allocator *alloc;
    sc_mock_counts *counts;
    sc_string last_event;
} sc_mock_observer_state;

extern const sc_provider_vtab sc_mock_provider_vtab;
extern const sc_tool_vtab sc_mock_tool_vtab;
extern const sc_channel_vtab sc_mock_channel_vtab;
extern const sc_memory_vtab sc_mock_memory_vtab;
extern const sc_observer_vtab sc_mock_observer_vtab;

sc_status sc_mock_provider_create(sc_allocator *alloc,
                                  sc_mock_counts *counts,
                                  sc_str text,
                                  sc_provider **out);
sc_status sc_mock_provider_factory(sc_allocator *alloc, sc_provider **out);
sc_status sc_mock_tool_create(sc_allocator *alloc, sc_mock_counts *counts, sc_str output, sc_tool **out);
sc_status sc_mock_tool_factory(sc_allocator *alloc, sc_tool **out);
sc_status sc_mock_channel_create(sc_allocator *alloc,
                                 sc_mock_counts *counts,
                                 sc_mock_channel_state **state,
                                 sc_channel **out);
sc_status sc_mock_channel_factory(sc_allocator *alloc, sc_channel **out);
sc_status sc_mock_memory_create(sc_allocator *alloc,
                                sc_mock_counts *counts,
                                sc_mock_memory_state **state,
                                sc_memory **out);
sc_status sc_mock_memory_factory(sc_allocator *alloc, sc_memory **out);
sc_status sc_mock_observer_create(sc_allocator *alloc,
                                  sc_mock_counts *counts,
                                  sc_mock_observer_state **state,
                                  sc_observer **out);
sc_status sc_mock_observer_factory(sc_allocator *alloc, sc_observer **out);
