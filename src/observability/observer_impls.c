#include "sc/observer.h"
#include "sc/vector.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sc_log_observer {
    sc_allocator *alloc;
    FILE *stream;
    sc_log_level min_level;
} sc_log_observer;

struct sc_metrics_observer {
    sc_allocator *alloc;
    sc_metrics_snapshot snapshot;
};

typedef struct sc_event_buffer_record {
    sc_string target;
    sc_string name;
    sc_string fields_json;
} sc_event_buffer_record;

struct sc_event_buffer_observer {
    sc_allocator *alloc;
    size_t limit;
    size_t dropped_events;
    sc_vec events;
};

static sc_status noop_emit(void *impl, const sc_observer_event *event);
static void noop_destroy(void *impl);
static sc_status log_emit(void *impl, const sc_observer_event *event);
static void log_destroy(void *impl);
static sc_status metrics_emit(void *impl, const sc_observer_event *event);
static void metrics_destroy(void *impl);
static sc_status buffer_emit(void *impl, const sc_observer_event *event);
static void buffer_destroy(void *impl);
static sc_status event_fields_json(const sc_observer_event *event, sc_allocator *alloc, sc_string *out);
static sc_status append_json_string(sc_string_builder *builder, sc_str text);
static void buffer_record_clear(sc_event_buffer_record *record);
static bool has_prefix(sc_str text, const char *prefix);
static bool contains_cstr(sc_str text, const char *needle);
static bool str_equal_cstr(sc_str text, const char *value);
static sc_str event_field_value(const sc_observer_event *event, const char *key);
static sc_str observer_event_subsystem(const sc_observer_event *event);
static sc_str observer_event_operation(const sc_observer_event *event);
static sc_str observer_event_outcome(const sc_observer_event *event);
static sc_str observer_event_error_key(const sc_observer_event *event);
static int64_t observer_event_duration_ms(const sc_observer_event *event);
static void metrics_count_domain_event(sc_metrics_snapshot *snapshot, const sc_observer_event *event);
static sc_log_level event_log_level(const sc_observer_event *event);

static const sc_observer_vtab noop_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "noop-observer",
    .display_name = "No-op observer",
    .feature_flag = "SC_OBSERVER_NOOP",
    .capabilities = SC_OBSERVER_CAP_NOOP,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = noop_emit,
    .destroy = noop_destroy,
};

static const sc_observer_vtab log_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "log-observer",
    .display_name = "Log observer",
    .feature_flag = "SC_OBSERVER_LOG",
    .capabilities = SC_OBSERVER_CAP_LOG,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = log_emit,
    .destroy = log_destroy,
};

static const sc_observer_vtab metrics_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "metrics-observer",
    .display_name = "Metrics observer",
    .feature_flag = "SC_OBSERVER_METRICS",
    .capabilities = SC_OBSERVER_CAP_METRICS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = metrics_emit,
    .destroy = metrics_destroy,
};

static const sc_observer_vtab buffer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "event-buffer-observer",
    .display_name = "Event buffer observer",
    .feature_flag = "SC_OBSERVER_EVENT_BUFFER",
    .capabilities = SC_OBSERVER_CAP_EVENT_BUFFER | SC_OBSERVER_CAP_EXPORT,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = buffer_emit,
    .destroy = buffer_destroy,
};

sc_event_family sc_observer_event_family(sc_str event_name)
{
    if (has_prefix(event_name, "turn.") || has_prefix(event_name, "runtime.")) {
        return SC_EVENT_RUNTIME;
    }
    if (has_prefix(event_name, "provider.")) {
        return SC_EVENT_PROVIDER;
    }
    if (has_prefix(event_name, "tool.")) {
        return SC_EVENT_TOOL;
    }
    if (has_prefix(event_name, "security.") || has_prefix(event_name, "audit.") ||
        has_prefix(event_name, "sandbox.") || has_prefix(event_name, "estop.")) {
        return SC_EVENT_SECURITY;
    }
    if (has_prefix(event_name, "memory.")) {
        return SC_EVENT_MEMORY;
    }
    if (has_prefix(event_name, "gateway.") || has_prefix(event_name, "websocket.")) {
        return SC_EVENT_GATEWAY;
    }
    if (has_prefix(event_name, "channel.")) {
        return SC_EVENT_CHANNEL;
    }
    if (has_prefix(event_name, "hardware.") || has_prefix(event_name, "peripheral.")) {
        return SC_EVENT_HARDWARE;
    }
    return SC_EVENT_UNKNOWN;
}

const char *sc_observer_event_family_name(sc_event_family family)
{
    switch (family) {
    case SC_EVENT_RUNTIME:
        return "runtime";
    case SC_EVENT_PROVIDER:
        return "provider";
    case SC_EVENT_TOOL:
        return "tool";
    case SC_EVENT_SECURITY:
        return "security";
    case SC_EVENT_MEMORY:
        return "memory";
    case SC_EVENT_GATEWAY:
        return "gateway";
    case SC_EVENT_CHANNEL:
        return "channel";
    case SC_EVENT_HARDWARE:
        return "hardware";
    case SC_EVENT_UNKNOWN:
        return "unknown";
    }
    return "unknown";
}

sc_str sc_observer_redact_field(const sc_log_field *field)
{
    return sc_log_redact_field(field);
}

sc_status sc_observer_noop_new(sc_allocator *alloc, sc_observer **out)
{
    return sc_observer_new(alloc, &noop_vtab, nullptr, out);
}

sc_status sc_observer_log_new(sc_allocator *alloc, FILE *stream, sc_log_level min_level, sc_observer **out)
{
    sc_log_observer *observer = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.log_observer.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    observer = sc_alloc(alloc, sizeof(*observer), _Alignof(sc_log_observer));
    if (observer == nullptr) {
        return sc_status_no_memory();
    }
    *observer = (sc_log_observer){
        .alloc = alloc,
        .stream = stream == nullptr ? stderr : stream,
        .min_level = min_level,
    };
    status = sc_observer_new(alloc, &log_vtab, observer, out);
    if (!sc_status_is_ok(status)) {
        log_destroy(observer);
    }
    return status;
}

sc_status sc_observer_metrics_new(sc_allocator *alloc,
                                  sc_metrics_observer **metrics,
                                  sc_observer **out)
{
    sc_metrics_observer *observer = nullptr;
    sc_status status;

    if (metrics == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.metrics_observer.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    observer = sc_alloc(alloc, sizeof(*observer), _Alignof(sc_metrics_observer));
    if (observer == nullptr) {
        return sc_status_no_memory();
    }
    *observer = (sc_metrics_observer){
        .alloc = alloc,
        .snapshot = {.struct_size = sizeof(sc_metrics_snapshot)},
    };
    status = sc_observer_new(alloc, &metrics_vtab, observer, out);
    if (!sc_status_is_ok(status)) {
        metrics_destroy(observer);
        return status;
    }
    *metrics = observer;
    return sc_status_ok();
}

sc_status sc_observer_metrics_snapshot(const sc_metrics_observer *metrics, sc_metrics_snapshot *out)
{
    if (metrics == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.metrics_observer.invalid_argument");
    }
    *out = metrics->snapshot;
    out->struct_size = sizeof(*out);
    return sc_status_ok();
}

sc_status sc_observer_event_buffer_new(sc_allocator *alloc,
                                       size_t limit,
                                       sc_event_buffer_observer **buffer,
                                       sc_observer **out)
{
    sc_event_buffer_observer *observer = nullptr;
    sc_status status;

    if (buffer == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.event_buffer.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    observer = sc_alloc(alloc, sizeof(*observer), _Alignof(sc_event_buffer_observer));
    if (observer == nullptr) {
        return sc_status_no_memory();
    }
    *observer = (sc_event_buffer_observer){.alloc = alloc, .limit = limit == 0 ? 64 : limit};
    sc_vec_init(&observer->events, alloc, sizeof(sc_event_buffer_record));
    status = sc_observer_new(alloc, &buffer_vtab, observer, out);
    if (!sc_status_is_ok(status)) {
        buffer_destroy(observer);
        return status;
    }
    *buffer = observer;
    return sc_status_ok();
}

size_t sc_observer_event_buffer_len(const sc_event_buffer_observer *buffer)
{
    return buffer == nullptr ? 0 : buffer->events.len;
}

size_t sc_observer_event_buffer_dropped(const sc_event_buffer_observer *buffer)
{
    return buffer == nullptr ? 0 : buffer->dropped_events;
}

sc_status sc_observer_event_buffer_at(const sc_event_buffer_observer *buffer,
                                      size_t index,
                                      sc_event_buffer_entry *out)
{
    const sc_event_buffer_record *record = nullptr;

    if (buffer == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.event_buffer.invalid_argument");
    }
    record = sc_vec_at_const(&buffer->events, index);
    if (record == nullptr) {
        return sc_status_invalid_argument("sc.event_buffer.index_out_of_bounds");
    }
    *out = (sc_event_buffer_entry){
        .struct_size = sizeof(*out),
        .target = sc_string_as_str(&record->target),
        .name = sc_string_as_str(&record->name),
        .fields_json = sc_string_as_str(&record->fields_json),
    };
    return sc_status_ok();
}

sc_status sc_observer_event_buffer_export_json(const sc_event_buffer_observer *buffer,
                                              sc_allocator *alloc,
                                              sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (buffer == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.event_buffer.invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "[");
    for (size_t i = 0; sc_status_is_ok(status) && i < buffer->events.len; i += 1) {
        const sc_event_buffer_record *record = sc_vec_at_const(&buffer->events, i);
        if (i > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "{\"target\":");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, sc_string_as_str(&record->target));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ",\"name\":");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, sc_string_as_str(&record->name));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ",\"fields\":");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&record->fields_json));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "}");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "]");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status noop_emit(void *impl, const sc_observer_event *event)
{
    (void)impl;
    if (event == nullptr) {
        return sc_status_invalid_argument("sc.noop_observer.invalid_argument");
    }
    return sc_status_ok();
}

static void noop_destroy(void *impl)
{
    (void)impl;
}

static sc_status log_emit(void *impl, const sc_observer_event *event)
{
    sc_log_observer *observer = impl;
    sc_log_level level = SC_LOG_INFO;
    if (observer == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.log_observer.invalid_argument");
    }
    level = event_log_level(event);
    if (observer->min_level <= level) {
        sc_log_write_to(observer->stream,
                        level,
                        event->target.ptr,
                        event->name.ptr,
                        event->fields,
                        event->field_count);
    }
    return sc_status_ok();
}

static void log_destroy(void *impl)
{
    sc_log_observer *observer = impl;
    if (observer == nullptr) {
        return;
    }
    sc_free(observer->alloc, observer, sizeof(*observer), _Alignof(sc_log_observer));
}

static sc_status metrics_emit(void *impl, const sc_observer_event *event)
{
    sc_metrics_observer *observer = impl;
    sc_event_family family = SC_EVENT_UNKNOWN;

    if (observer == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.metrics_observer.invalid_argument");
    }
    observer->snapshot.total_events += 1;
    observer->snapshot.total_fields += event->field_count;
    for (size_t i = 0; i < event->field_count; i += 1) {
        if (event->fields[i].secret) {
            observer->snapshot.secret_fields += 1;
        }
    }
    metrics_count_domain_event(&observer->snapshot, event);
    family = sc_observer_event_family(event->name);
    switch (family) {
    case SC_EVENT_RUNTIME:
        observer->snapshot.runtime_events += 1;
        break;
    case SC_EVENT_PROVIDER:
        observer->snapshot.provider_events += 1;
        break;
    case SC_EVENT_TOOL:
        observer->snapshot.tool_events += 1;
        break;
    case SC_EVENT_SECURITY:
        observer->snapshot.security_events += 1;
        break;
    case SC_EVENT_MEMORY:
        observer->snapshot.memory_events += 1;
        break;
    case SC_EVENT_GATEWAY:
        observer->snapshot.gateway_events += 1;
        break;
    case SC_EVENT_CHANNEL:
        observer->snapshot.channel_events += 1;
        break;
    case SC_EVENT_HARDWARE:
        observer->snapshot.hardware_events += 1;
        break;
    case SC_EVENT_UNKNOWN:
        observer->snapshot.unknown_events += 1;
        break;
    }
    return sc_status_ok();
}

static void metrics_destroy(void *impl)
{
    sc_metrics_observer *observer = impl;
    if (observer == nullptr) {
        return;
    }
    sc_free(observer->alloc, observer, sizeof(*observer), _Alignof(sc_metrics_observer));
}

static sc_status buffer_emit(void *impl, const sc_observer_event *event)
{
    sc_event_buffer_observer *observer = impl;
    sc_event_buffer_record record = {0};
    sc_status status;

    if (observer == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.event_buffer.invalid_argument");
    }
    status = sc_string_from_str(observer->alloc, event->target, &record.target);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(observer->alloc, event->name, &record.name);
    }
    if (sc_status_is_ok(status)) {
        status = event_fields_json(event, observer->alloc, &record.fields_json);
    }
    if (!sc_status_is_ok(status)) {
        buffer_record_clear(&record);
        return status;
    }
    if (observer->events.len == observer->limit) {
        sc_event_buffer_record *oldest = sc_vec_at(&observer->events, 0);
        buffer_record_clear(oldest);
        if (observer->events.len > 1) {
            void *base = observer->events.ptr;
            (void)memmove(base,
                          (unsigned char *)base + observer->events.item_size,
                          (observer->events.len - 1) * observer->events.item_size);
        }
        observer->events.len -= 1;
        observer->dropped_events += 1;
    }
    status = sc_vec_push(&observer->events, &record);
    if (!sc_status_is_ok(status)) {
        buffer_record_clear(&record);
    }
    return status;
}

static void buffer_destroy(void *impl)
{
    sc_event_buffer_observer *observer = impl;
    if (observer == nullptr) {
        return;
    }
    for (size_t i = 0; i < observer->events.len; i += 1) {
        sc_event_buffer_record *record = sc_vec_at(&observer->events, i);
        buffer_record_clear(record);
    }
    sc_vec_clear(&observer->events);
    sc_free(observer->alloc, observer, sizeof(*observer), _Alignof(sc_event_buffer_observer));
}

static sc_status event_fields_json(const sc_observer_event *event, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    size_t emitted = 0;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{");
    for (size_t i = 0; sc_status_is_ok(status) && i < event->field_count; i += 1) {
        sc_str value = sc_observer_redact_field(&event->fields[i]);
        if (emitted > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, sc_str_from_cstr(event->fields[i].key));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ":");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, value);
        }
        emitted += 1;
    }
    sc_log_field normalized[] = {
        {.key = "event_key", .value = event->name, .secret = false},
        {.key = "subsystem", .value = observer_event_subsystem(event), .secret = false},
        {.key = "operation", .value = observer_event_operation(event), .secret = false},
        {.key = "outcome", .value = observer_event_outcome(event), .secret = false},
        {.key = "error_key", .value = observer_event_error_key(event), .secret = false},
        {.key = "redacted_target", .value = event->redacted_target, .secret = false},
        {.key = "correlation_id", .value = event->correlation_id, .secret = false},
        {.key = "turn_id", .value = event->turn_id, .secret = false},
        {.key = "trace_id", .value = event->trace_id, .secret = false},
        {.key = "span_id", .value = event->span_id, .secret = false},
    };
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(normalized); i += 1) {
        if (normalized[i].value.ptr == nullptr || normalized[i].value.len == 0) {
            continue;
        }
        if (emitted > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, sc_str_from_cstr(normalized[i].key));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ":");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, sc_observer_redact_field(&normalized[i]));
        }
        emitted += 1;
    }
    if (sc_status_is_ok(status) && observer_event_duration_ms(event) > 0) {
        char duration[32] = {0};
        (void)snprintf(duration, sizeof(duration), "%lld", (long long)observer_event_duration_ms(event));
        if (emitted > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\"duration_ms\":");
        }
        if (sc_status_is_ok(status)) {
            status = append_json_string(&builder, sc_str_from_cstr(duration));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_json_string(sc_string_builder *builder, sc_str text)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < text.len; i += 1) {
        char ch = text.ptr == nullptr ? '\0' : text.ptr[i];
        char escaped[7] = {0};
        if (ch == '"' || ch == '\\') {
            escaped[0] = '\\';
            escaped[1] = ch;
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, 2));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else if (ch == '\r') {
            status = sc_string_builder_append_cstr(builder, "\\r");
        } else if (ch == '\t') {
            status = sc_string_builder_append_cstr(builder, "\\t");
        } else if ((unsigned char)ch < 0x20u) {
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)(unsigned char)ch);
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, 6));
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(&text.ptr[i], 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

static void buffer_record_clear(sc_event_buffer_record *record)
{
    if (record == nullptr) {
        return;
    }
    sc_string_clear(&record->target);
    sc_string_clear(&record->name);
    sc_string_clear(&record->fields_json);
    *record = (sc_event_buffer_record){0};
}

static bool has_prefix(sc_str text, const char *prefix)
{
    size_t len = strlen(prefix);
    return text.ptr != nullptr && text.len >= len && memcmp(text.ptr, prefix, len) == 0;
}

static bool contains_cstr(sc_str text, const char *needle)
{
    size_t needle_len = needle == nullptr ? 0 : strlen(needle);
    if (text.ptr == nullptr || needle_len == 0 || text.len < needle_len) {
        return false;
    }
    for (size_t i = 0; i <= text.len - needle_len; i += 1) {
        if (memcmp(text.ptr + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool str_equal_cstr(sc_str text, const char *value)
{
    size_t len = value == nullptr ? 0 : strlen(value);
    return text.ptr != nullptr && text.len == len && memcmp(text.ptr, value, len) == 0;
}

static sc_str event_field_value(const sc_observer_event *event, const char *key)
{
    if (event == nullptr || key == nullptr) {
        return sc_str_from_parts(nullptr, 0);
    }
    for (size_t i = 0; i < event->field_count; i += 1) {
        if (event->fields[i].key != nullptr && strcmp(event->fields[i].key, key) == 0) {
            return event->fields[i].value;
        }
    }
    return sc_str_from_parts(nullptr, 0);
}

static sc_str observer_event_subsystem(const sc_observer_event *event)
{
    sc_str value = event == nullptr ? sc_str_from_parts(nullptr, 0) : event->subsystem;
    if (value.ptr == nullptr || value.len == 0) {
        value = event_field_value(event, "subsystem");
    }
    if ((value.ptr == nullptr || value.len == 0) && event != nullptr) {
        value = sc_str_from_cstr(sc_observer_event_family_name(sc_observer_event_family(event->name)));
    }
    return value;
}

static sc_str observer_event_operation(const sc_observer_event *event)
{
    sc_str value = event == nullptr ? sc_str_from_parts(nullptr, 0) : event->operation;
    if (value.ptr == nullptr || value.len == 0) {
        value = event_field_value(event, "operation");
    }
    if ((value.ptr == nullptr || value.len == 0) && event != nullptr) {
        value = event->name;
    }
    return value;
}

static sc_str observer_event_outcome(const sc_observer_event *event)
{
    sc_str value = event == nullptr ? sc_str_from_parts(nullptr, 0) : event->outcome;
    if (value.ptr == nullptr || value.len == 0) {
        value = event_field_value(event, "outcome");
    }
    if (value.ptr == nullptr || value.len == 0) {
        value = event_field_value(event, "status");
    }
    return value;
}

static sc_str observer_event_error_key(const sc_observer_event *event)
{
    sc_str value = event == nullptr ? sc_str_from_parts(nullptr, 0) : event->error_key;
    if (value.ptr == nullptr || value.len == 0) {
        value = event_field_value(event, "error_key");
    }
    return value;
}

static int64_t observer_event_duration_ms(const sc_observer_event *event)
{
    sc_str value = event_field_value(event, "duration_ms");
    char buffer[32] = {0};
    size_t len = value.len < sizeof(buffer) - 1 ? value.len : sizeof(buffer) - 1;

    if (event != nullptr && event->duration_ms > 0) {
        return event->duration_ms;
    }
    if (value.ptr == nullptr || value.len == 0) {
        return 0;
    }
    (void)memcpy(buffer, value.ptr, len);
    return (int64_t)strtoll(buffer, nullptr, 10);
}

static void metrics_count_domain_event(sc_metrics_snapshot *snapshot, const sc_observer_event *event)
{
    sc_str subsystem = observer_event_subsystem(event);
    sc_str operation = observer_event_operation(event);
    sc_str outcome = observer_event_outcome(event);
    int64_t duration_ms = observer_event_duration_ms(event);

    if (snapshot == nullptr || event == nullptr) {
        return;
    }
    if (str_equal_cstr(subsystem, "runtime")) {
        if (contains_cstr(operation, "turn") || contains_cstr(event->name, "turn")) {
            snapshot->runtime_turns += 1;
            if (str_equal_cstr(outcome, "error") || str_equal_cstr(outcome, "failed")) {
                snapshot->runtime_turn_failures += 1;
            }
            if (duration_ms > 0) {
                snapshot->runtime_turn_duration_ms_total += (uint64_t)duration_ms;
            }
        }
    } else if (str_equal_cstr(subsystem, "provider")) {
        snapshot->provider_requests += 1;
        if (str_equal_cstr(outcome, "error") || str_equal_cstr(outcome, "failed")) {
            snapshot->provider_failures += 1;
        }
        if (duration_ms > 0) {
            snapshot->provider_latency_ms_total += (uint64_t)duration_ms;
        }
    } else if (str_equal_cstr(subsystem, "tool")) {
        snapshot->tool_calls += 1;
        if (str_equal_cstr(outcome, "error") || str_equal_cstr(outcome, "failed") || str_equal_cstr(outcome, "denied")) {
            snapshot->tool_failures += 1;
        }
        if (str_equal_cstr(event_field_value(event, "side_effect"), "none") ||
            str_equal_cstr(event_field_value(event, "category"), "readonly")) {
            snapshot->tool_readonly_calls += 1;
        } else {
            snapshot->tool_side_effect_calls += 1;
        }
    } else if (str_equal_cstr(subsystem, "security")) {
        if (contains_cstr(operation, "denied") || contains_cstr(event->name, "denied") ||
            str_equal_cstr(outcome, "denied")) {
            snapshot->security_denials += 1;
        }
    } else if (str_equal_cstr(subsystem, "gateway")) {
        snapshot->gateway_requests += 1;
        if (str_equal_cstr(outcome, "error") || str_equal_cstr(outcome, "failed") || str_equal_cstr(outcome, "denied")) {
            snapshot->gateway_failures += 1;
        }
    } else if (str_equal_cstr(subsystem, "channel")) {
        if (contains_cstr(operation, "inbound") || contains_cstr(event->name, "inbound")) {
            snapshot->channel_inbound_events += 1;
        }
        if (contains_cstr(operation, "outbound") || contains_cstr(event->name, "outbound")) {
            snapshot->channel_outbound_events += 1;
        }
    } else if (str_equal_cstr(subsystem, "memory")) {
        if (contains_cstr(operation, "retrieve") || contains_cstr(operation, "recall") ||
            contains_cstr(event->name, "retrieve") || contains_cstr(event->name, "recall")) {
            snapshot->memory_retrievals += 1;
            if (str_equal_cstr(outcome, "error") || str_equal_cstr(outcome, "failed")) {
                snapshot->memory_retrieval_failures += 1;
            }
            if (duration_ms > 0) {
                snapshot->memory_retrieval_latency_ms_total += (uint64_t)duration_ms;
            }
        }
    }
}

static sc_log_level event_log_level(const sc_observer_event *event)
{
    sc_str status = event_field_value(event, "status");
    sc_str name = event == nullptr ? sc_str_from_parts(nullptr, 0) : event->name;

    if (contains_cstr(name, "fatal") || sc_str_equal(status, sc_str_from_cstr("fatal"))) {
        return SC_LOG_FATAL;
    }
    if (contains_cstr(name, "error") || contains_cstr(name, "failed") ||
        sc_str_equal(status, sc_str_from_cstr("error")) || sc_str_equal(status, sc_str_from_cstr("failed"))) {
        return SC_LOG_ERROR;
    }
    if (contains_cstr(name, "denied") || contains_cstr(name, "rejected") || contains_cstr(name, "cancelled") ||
        sc_str_equal(status, sc_str_from_cstr("warn")) || sc_str_equal(status, sc_str_from_cstr("cancelled"))) {
        return SC_LOG_WARN;
    }
    if (contains_cstr(name, ".start") || contains_cstr(name, ".selected") || contains_cstr(name, ".loaded")) {
        return SC_LOG_DEBUG;
    }
    if (contains_cstr(name, ".tick") || contains_cstr(name, ".heartbeat") || contains_cstr(name, ".validated")) {
        return SC_LOG_TRACE;
    }
    return SC_LOG_INFO;
}
