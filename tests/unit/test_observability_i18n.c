#include "sc/i18n.h"
#include "sc/observer.h"
#include "sc/registry.h"
#include "sc/security.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int test_observer_implementations(void);
static int test_observer_isolation(void);
static int test_observer_receipt_metadata(void);
static int test_i18n_catalog(void);
static int test_i18n_scanner(void);
static sc_status failing_emit(void *impl, const sc_observer_event *event);
static void failing_destroy(void *impl);
static sc_observer_event make_event(const char *target,
                                    const char *name,
                                    const sc_log_field *fields,
                                    size_t field_count);

static const sc_observer_vtab failing_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "failing-observer",
    .display_name = "Failing observer",
    .feature_flag = "SC_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = failing_emit,
    .destroy = failing_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_observer_implementations();
    failures += test_observer_isolation();
    failures += test_observer_receipt_metadata();
    failures += test_i18n_catalog();
    failures += test_i18n_scanner();

    return failures == 0 ? 0 : 1;
}

static int test_observer_implementations(void)
{
    int failures = 0;
    sc_observer *noop = nullptr;
    sc_observer *metrics_observer = nullptr;
    sc_observer *buffer_observer = nullptr;
    sc_observer *log_observer = nullptr;
    sc_metrics_observer *metrics = nullptr;
    sc_event_buffer_observer *buffer = nullptr;
    sc_metrics_snapshot snapshot = {0};
    sc_event_buffer_entry entry = {0};
    sc_string exported = {0};
    FILE *stream = tmpfile();
    char log_text[256] = {0};
    sc_log_field fields[] = {
        {.key = "status", .value = sc_str_from_cstr("ok")},
        {.key = "Authorization", .value = sc_str_from_cstr("Bearer secret-value"), .secret = false},
        {.key = "Cookie", .value = sc_str_from_cstr("session=secret-cookie"), .secret = false},
        {.key = "pairing_secret", .value = sc_str_from_cstr("pairing-code"), .secret = false},
        {.key = "otp_material", .value = sc_str_from_cstr("123456"), .secret = false},
        {.key = "Prompt", .value = sc_str_from_cstr("raw prompt secret"), .secret = false},
        {.key = "raw_message", .value = sc_str_from_cstr("raw channel message"), .secret = false},
        {.key = "payload_body", .value = sc_str_from_cstr("raw provider payload"), .secret = false},
        {.key = "correlation", .value = sc_str_from_cstr("corr-1"), .secret = false},
    };
    sc_observer_event runtime = make_event("runtime", "turn.start", fields, SC_ARRAY_LEN(fields));
    sc_observer_event provider = make_event("provider", "provider.request", fields, SC_ARRAY_LEN(fields));
    sc_observer_event tool = make_event("tool", "tool.finish", fields, SC_ARRAY_LEN(fields));
    sc_log_field failed_fields[] = {
        {.key = "status", .value = sc_str_from_cstr("failed")},
        {.key = "duration_ms", .value = sc_str_from_cstr("37")},
        {.key = "category", .value = sc_str_from_cstr("readonly")},
    };
    sc_observer_event failed_runtime = make_event("runtime", "runtime.turn.complete", failed_fields, SC_ARRAY_LEN(failed_fields));
    failed_runtime.subsystem = sc_str_from_cstr("runtime");
    failed_runtime.operation = sc_str_from_cstr("turn.complete");
    failed_runtime.outcome = sc_str_from_cstr("failed");
    failed_runtime.error_key = sc_str_from_cstr("sc.runtime.failed");
    failed_runtime.duration_ms = 37;
    failed_runtime.turn_id = sc_str_from_cstr("turn-1");

    failures += sc_test_expect_status("noop new", sc_observer_noop_new(sc_allocator_heap(), &noop), SC_OK);
    failures += sc_test_expect_status("noop emit", sc_observer_emit(noop, &runtime), SC_OK);
    failures += sc_test_expect_status("metrics new",
                              sc_observer_metrics_new(sc_allocator_heap(), &metrics, &metrics_observer),
                              SC_OK);
    failures += sc_test_expect_status("buffer new",
                              sc_observer_event_buffer_new(sc_allocator_heap(), 2, &buffer, &buffer_observer),
                              SC_OK);
    failures += sc_test_expect_true("tmpfile available", stream != nullptr);
    if (stream != nullptr) {
        failures += sc_test_expect_status("log new",
                                  sc_observer_log_new(sc_allocator_heap(), stream, SC_LOG_TRACE, &log_observer),
                                  SC_OK);
    }

    failures += sc_test_expect_status("metrics emit runtime", sc_observer_emit(metrics_observer, &runtime), SC_OK);
    failures += sc_test_expect_status("metrics emit provider", sc_observer_emit(metrics_observer, &provider), SC_OK);
    failures += sc_test_expect_status("metrics snapshot", sc_observer_metrics_snapshot(metrics, &snapshot), SC_OK);
    failures += sc_test_expect_true("metrics count", snapshot.total_events == 2);
    failures += sc_test_expect_true("metrics family", snapshot.runtime_events == 1 && snapshot.provider_events == 1);
    failures += sc_test_expect_status("metrics emit failed runtime", sc_observer_emit(metrics_observer, &failed_runtime), SC_OK);
    failures += sc_test_expect_status("metrics snapshot updated", sc_observer_metrics_snapshot(metrics, &snapshot), SC_OK);
    failures += sc_test_expect_true("metrics secrets", snapshot.secret_fields >= 14);
    failures += sc_test_expect_true("metrics runtime turns", snapshot.runtime_turns >= 1 && snapshot.runtime_turn_failures >= 1);
    failures += sc_test_expect_true("metrics runtime duration", snapshot.runtime_turn_duration_ms_total >= 37);

    failures += sc_test_expect_status("buffer emit runtime", sc_observer_emit(buffer_observer, &runtime), SC_OK);
    failures += sc_test_expect_status("buffer emit provider", sc_observer_emit(buffer_observer, &provider), SC_OK);
    failures += sc_test_expect_status("buffer emit tool", sc_observer_emit(buffer_observer, &tool), SC_OK);
    failures += sc_test_expect_true("buffer limit", sc_observer_event_buffer_len(buffer) == 2);
    failures += sc_test_expect_true("buffer dropped count", sc_observer_event_buffer_dropped(buffer) == 1);
    failures += sc_test_expect_status("buffer at", sc_observer_event_buffer_at(buffer, 0, &entry), SC_OK);
    failures += sc_test_expect_true("oldest dropped", sc_str_equal(entry.name, sc_str_from_cstr("provider.request")));
    failures += sc_test_expect_true("secret redacted", strstr(entry.fields_json.ptr, "secret-value") == nullptr);
    failures += sc_test_expect_true("cookie redacted", strstr(entry.fields_json.ptr, "secret-cookie") == nullptr);
    failures += sc_test_expect_true("pairing redacted", strstr(entry.fields_json.ptr, "pairing-code") == nullptr);
    failures += sc_test_expect_true("otp redacted", strstr(entry.fields_json.ptr, "123456") == nullptr);
    failures += sc_test_expect_true("prompt redacted", strstr(entry.fields_json.ptr, "raw prompt secret") == nullptr);
    failures += sc_test_expect_true("raw message redacted", strstr(entry.fields_json.ptr, "raw channel message") == nullptr);
    failures += sc_test_expect_true("payload redacted", strstr(entry.fields_json.ptr, "raw provider payload") == nullptr);
    failures += sc_test_expect_true("normalized fields", strstr(entry.fields_json.ptr, "event_key") != nullptr && strstr(entry.fields_json.ptr, "subsystem") != nullptr);
    failures += sc_test_expect_true("redaction marker", strstr(entry.fields_json.ptr, "[REDACTED]") != nullptr);
    failures += sc_test_expect_status("buffer export",
                              sc_observer_event_buffer_export_json(buffer, sc_allocator_heap(), &exported),
                              SC_OK);
    failures += sc_test_expect_true("export contains tool", strstr(exported.ptr, "tool.finish") != nullptr);

    if (log_observer != nullptr) {
        failures += sc_test_expect_status("log emit", sc_observer_emit(log_observer, &runtime), SC_OK);
        (void)fflush(stream);
        (void)rewind(stream);
        size_t read_count = fread(log_text, 1, sizeof(log_text) - 1, stream);
        log_text[read_count] = '\0';
        failures += sc_test_expect_true("log redacted", strstr(log_text, "secret-value") == nullptr);
        failures += sc_test_expect_true("log prompt redacted", strstr(log_text, "raw prompt secret") == nullptr);
        failures += sc_test_expect_true("log raw message redacted", strstr(log_text, "raw channel message") == nullptr);
        failures += sc_test_expect_true("log has event", strstr(log_text, "turn.start") != nullptr);
    }

    sc_string_clear(&exported);
    sc_observer_destroy(log_observer);
    sc_observer_destroy(buffer_observer);
    sc_observer_destroy(metrics_observer);
    sc_observer_destroy(noop);
    if (stream != nullptr) {
        (void)fclose(stream);
    }
    return failures;
}

static int test_observer_isolation(void)
{
    int failures = 0;
    sc_observer_list list = {0};
    sc_observer *failing = nullptr;
    sc_observer *metrics_observer = nullptr;
    sc_metrics_observer *metrics = nullptr;
    sc_metrics_snapshot snapshot = {0};
    sc_observer_event event = make_event("security", "security.denial", nullptr, 0);
    size_t failure_count = 0;

    sc_observer_list_init(&list, sc_allocator_heap());
    failures += sc_test_expect_status("failing observer",
                              sc_observer_new(sc_allocator_heap(), &failing_vtab, nullptr, &failing),
                              SC_OK);
    failures += sc_test_expect_status("metrics observer",
                              sc_observer_metrics_new(sc_allocator_heap(), &metrics, &metrics_observer),
                              SC_OK);
    failures += sc_test_expect_status("list add failing", sc_observer_list_add(&list, failing), SC_OK);
    failures += sc_test_expect_status("list add metrics", sc_observer_list_add(&list, metrics_observer), SC_OK);
    failures += sc_test_expect_status("strict fanout fails", sc_observer_list_emit(&list, &event), SC_ERR_IO);
    failures += sc_test_expect_status("isolated fanout",
                              sc_observer_list_emit_isolated(&list, &event, &failure_count),
                              SC_OK);
    failures += sc_test_expect_true("isolated counted failure", failure_count == 1);
    failures += sc_test_expect_status("isolated metrics updated", sc_observer_metrics_snapshot(metrics, &snapshot), SC_OK);
    failures += sc_test_expect_true("isolated continued", snapshot.security_events == 1);
    failures += sc_test_expect_status("single safe emit", sc_observer_emit_safe(failing, &event, &failure_count), SC_OK);
    failures += sc_test_expect_true("single safe counted failure", failure_count == 1);

    sc_observer_list_clear(&list);
    return failures;
}

static int test_observer_receipt_metadata(void)
{
    int failures = 0;
    sc_receipt_chain receipts = {0};

    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    failures += sc_test_expect_status("receipt append ex",
                              sc_receipt_chain_append_ex(&receipts,
                                                         sc_str_from_cstr("file_write"),
                                                         sc_str_from_cstr("hash=abc len=12"),
                                                         sc_str_from_cstr("hash=def len=3"),
                                                         false,
                                                         sc_str_from_cstr("denied"),
                                                         sc_str_from_cstr("sc.tool.security_denied"),
                                                         sc_str_from_cstr("denied")),
                              SC_OK);
    failures += sc_test_expect_true("receipt verifies", sc_receipt_chain_verify(&receipts));
    if (receipts.receipts.len == 1) {
        const sc_tool_receipt *receipt = sc_vec_at_const(&receipts.receipts, 0);
        failures += sc_test_expect_true("receipt policy", receipt != nullptr && strcmp(receipt->policy_decision.ptr, "denied") == 0);
        failures += sc_test_expect_true("receipt reason", receipt != nullptr && strcmp(receipt->failure_reason.ptr, "sc.tool.security_denied") == 0);
        failures += sc_test_expect_true("receipt outcome", receipt != nullptr && strcmp(receipt->outcome.ptr, "denied") == 0);
    } else {
        failures += sc_test_expect_true("receipt count", false);
    }
    sc_receipt_chain_clear(&receipts);
    return failures;
}

static int test_i18n_catalog(void)
{
    int failures = 0;
    sc_i18n_catalog catalog = {0};
    sc_string output = {0};
    sc_string report = {0};
    sc_i18n_arg args[] = {
        {.name = sc_str_from_cstr("name"), .value = sc_str_from_cstr("Ada")},
        {.name = sc_str_from_cstr("token"), .value = sc_str_from_cstr("secret-token"), .secret = true},
    };
    sc_str required[] = {
        sc_str_from_cstr("hello"),
        sc_str_from_cstr("missing"),
    };

    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    failures += sc_test_expect_status("catalog load",
                              sc_i18n_catalog_load_ftl(&catalog,
                                                       sc_str_from_cstr("hello = Hello { $name }\n"
                                                                        "secret = Token { $token }\n")),
                              SC_OK);
    failures += sc_test_expect_true("catalog has", sc_i18n_catalog_has(&catalog, sc_str_from_cstr("hello")));
    failures += sc_test_expect_status("format named",
                              sc_i18n_format(&catalog,
                                             sc_str_from_cstr("hello"),
                                             args,
                                             SC_ARRAY_LEN(args),
                                             sc_allocator_heap(),
                                             &output),
                              SC_OK);
    failures += sc_test_expect_true("formatted", strcmp(output.ptr, "Hello Ada") == 0);
    sc_string_clear(&output);

    failures += sc_test_expect_status("format secret",
                              sc_i18n_format(&catalog,
                                             sc_str_from_cstr("secret"),
                                             args,
                                             SC_ARRAY_LEN(args),
                                             sc_allocator_heap(),
                                             &output),
                              SC_OK);
    failures += sc_test_expect_true("secret hidden", strcmp(output.ptr, "Token [REDACTED]") == 0);
    sc_string_clear(&output);

    failures += sc_test_expect_status("missing key",
                              sc_i18n_format(&catalog,
                                             sc_str_from_cstr("absent"),
                                             nullptr,
                                             0,
                                             sc_allocator_heap(),
                                             &output),
                              SC_OK);
    failures += sc_test_expect_true("missing fallback", strcmp(output.ptr, "!absent!") == 0);
    sc_string_clear(&output);

    failures += sc_test_expect_status("coverage",
                              sc_i18n_coverage_report(&catalog,
                                                      required,
                                                      SC_ARRAY_LEN(required),
                                                      sc_allocator_heap(),
                                                      &report),
                              SC_OK);
    failures += sc_test_expect_true("coverage missing", strstr(report.ptr, "\"missing\"") != nullptr);

    sc_string_clear(&report);
    sc_i18n_catalog_clear(&catalog);
    return failures;
}

static int test_i18n_scanner(void)
{
    int failures = 0;
    sc_i18n_scan_result result = {0};

    failures += sc_test_expect_status("scan clean",
                              sc_i18n_scan_c_source(sc_str_from_cstr("sc_i18n_format(&catalog, key, nullptr, 0, alloc, &out);\n"
                                                                     "sc_log_write(SC_LOG_INFO, \"target\", \"event\", nullptr, 0);\n"),
                                                    &result),
                              SC_OK);
    failures += sc_test_expect_true("scan clean ok", result.ok && result.bare_string_count == 0);
    failures += sc_test_expect_status("scan bare",
                              sc_i18n_scan_c_source(sc_str_from_cstr("printf(\"Hello user\\n\");\n"
                                                                     "puts(\"Another prompt\");\n"),
                                                    &result),
                              SC_OK);
    failures += sc_test_expect_true("scan catches", !result.ok && result.bare_string_count == 2);
    return failures;
}

static sc_status failing_emit(void *impl, const sc_observer_event *event)
{
    (void)impl;
    (void)event;
    return sc_status_io("sc.test_observer.fail");
}

static void failing_destroy(void *impl)
{
    (void)impl;
}

static sc_observer_event make_event(const char *target,
                                    const char *name,
                                    const sc_log_field *fields,
                                    size_t field_count)
{
    return (sc_observer_event){
        .struct_size = sizeof(sc_observer_event),
        .target = sc_str_from_cstr(target),
        .name = sc_str_from_cstr(name),
        .fields = fields,
        .field_count = field_count,
    };
}
