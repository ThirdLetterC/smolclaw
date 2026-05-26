#pragma once

#include "sc/allocator.h"
#include "sc/contract.h"
#include "sc/log.h"
#include "sc/result.h"
#include "sc/string.h"

#include <stdint.h>
#include <stdio.h>

SC_BEGIN_DECLS

/*
 * Ownership/threading: event fields and normalized string fields are borrowed
 * for the call. The handle owns impl and calls destroy exactly once. Observer
 * list emission is sequential and not synchronized; observers document their
 * own thread-safety. Runtime-owned paths use safe/isolated emission so observer
 * callback failures are counted but do not corrupt runtime state. Observer
 * implementations must redact secret-bearing fields before storage/export.
 * Bounded observers document and count their own drop behavior.
 */
typedef struct sc_observer sc_observer;
typedef struct sc_metrics_observer sc_metrics_observer;
typedef struct sc_event_buffer_observer sc_event_buffer_observer;

enum {
    SC_OBSERVER_CAP_NOOP = 1u << 0u,
    SC_OBSERVER_CAP_LOG = 1u << 1u,
    SC_OBSERVER_CAP_METRICS = 1u << 2u,
    SC_OBSERVER_CAP_EVENT_BUFFER = 1u << 3u,
    SC_OBSERVER_CAP_EXPORT = 1u << 4u
};

typedef enum sc_event_family {
    SC_EVENT_RUNTIME = 0,
    SC_EVENT_PROVIDER,
    SC_EVENT_TOOL,
    SC_EVENT_SECURITY,
    SC_EVENT_MEMORY,
    SC_EVENT_GATEWAY,
    SC_EVENT_CHANNEL,
    SC_EVENT_HARDWARE,
    SC_EVENT_UNKNOWN
} sc_event_family;

typedef enum sc_observer_event_kind {
    SC_OBSERVER_EVENT_LOG = 0,
    SC_OBSERVER_EVENT_RUNTIME,
    SC_OBSERVER_EVENT_METRIC,
    SC_OBSERVER_EVENT_TRACE,
    SC_OBSERVER_EVENT_AUDIT,
    SC_OBSERVER_EVENT_RECEIPT
} sc_observer_event_kind;

typedef struct sc_observer_event {
    size_t struct_size;
    sc_str target;
    sc_str name;
    const sc_log_field *fields;
    size_t field_count;
    sc_observer_event_kind kind;
    sc_str subsystem;
    sc_str operation;
    sc_str outcome;
    sc_str error_key;
    sc_str redacted_target;
    int64_t duration_ms;
    sc_str correlation_id;
    sc_str turn_id;
    sc_str trace_id;
    sc_str span_id;
} sc_observer_event;

typedef struct sc_metrics_snapshot {
    size_t struct_size;
    size_t total_events;
    size_t total_fields;
    size_t secret_fields;
    size_t runtime_events;
    size_t provider_events;
    size_t tool_events;
    size_t security_events;
    size_t memory_events;
    size_t gateway_events;
    size_t channel_events;
    size_t hardware_events;
    size_t unknown_events;
    size_t runtime_turns;
    size_t runtime_turn_failures;
    uint64_t runtime_turn_duration_ms_total;
    size_t provider_requests;
    size_t provider_failures;
    uint64_t provider_latency_ms_total;
    size_t tool_calls;
    size_t tool_failures;
    size_t tool_readonly_calls;
    size_t tool_side_effect_calls;
    size_t security_denials;
    size_t gateway_requests;
    size_t gateway_failures;
    size_t channel_inbound_events;
    size_t channel_outbound_events;
    size_t memory_retrievals;
    size_t memory_retrieval_failures;
    uint64_t memory_retrieval_latency_ms_total;
    size_t dropped_events;
    size_t observer_failures;
} sc_metrics_snapshot;

typedef struct sc_event_buffer_entry {
    size_t struct_size;
    sc_str target;
    sc_str name;
    sc_str fields_json;
} sc_event_buffer_entry;

typedef struct sc_observer_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*emit)(void *impl, const sc_observer_event *event);
    sc_status (*flush)(void *impl);
    void (*destroy)(void *impl);
} sc_observer_vtab;

static inline bool sc_observer_handle_is_null(const sc_observer *observer)
{
    return observer == nullptr;
}

bool sc_observer_vtab_valid(const sc_observer_vtab *vtab);
sc_status sc_observer_new(sc_allocator *alloc, const sc_observer_vtab *vtab, void *impl, sc_observer **out);
sc_status sc_observer_emit(sc_observer *observer, const sc_observer_event *event);
sc_status sc_observer_emit_safe(sc_observer *observer, const sc_observer_event *event, size_t *failure_count);
sc_status sc_observer_flush(sc_observer *observer);
const sc_observer_vtab *sc_observer_vtab_of(const sc_observer *observer);
void sc_observer_destroy(sc_observer *observer);

sc_event_family sc_observer_event_family(sc_str event_name);
const char *sc_observer_event_family_name(sc_event_family family);
sc_str sc_observer_redact_field(const sc_log_field *field);
sc_status sc_observer_noop_new(sc_allocator *alloc, sc_observer **out);
sc_status sc_observer_log_new(sc_allocator *alloc, FILE *stream, sc_log_level min_level, sc_observer **out);
sc_status sc_observer_metrics_new(sc_allocator *alloc,
                                  sc_metrics_observer **metrics,
                                  sc_observer **out);
sc_status sc_observer_metrics_snapshot(const sc_metrics_observer *metrics, sc_metrics_snapshot *out);
sc_status sc_observer_event_buffer_new(sc_allocator *alloc,
                                       size_t limit,
                                       sc_event_buffer_observer **buffer,
                                       sc_observer **out);
size_t sc_observer_event_buffer_len(const sc_event_buffer_observer *buffer);
size_t sc_observer_event_buffer_dropped(const sc_event_buffer_observer *buffer);
sc_status sc_observer_event_buffer_at(const sc_event_buffer_observer *buffer,
                                      size_t index,
                                      sc_event_buffer_entry *out);
sc_status sc_observer_event_buffer_export_json(const sc_event_buffer_observer *buffer,
                                              sc_allocator *alloc,
                                              sc_string *out);

SC_END_DECLS
