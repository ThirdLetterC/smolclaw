#include "security/security_internal.h"

sc_status sc_approval_request_new(sc_allocator *alloc,
                                  sc_str tool_name,
                                  sc_str summary,
                                  sc_tool_risk risk,
                                  int64_t timeout_ms,
                                  sc_approval_request *out)
{
    sc_status status;

    if (out == nullptr || tool_name.len == 0 || tool_name.ptr == nullptr) {
        return sc_status_invalid_argument("sc.approval.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_approval_request){.struct_size = sizeof(*out), .risk = risk, .timeout_ms = timeout_ms};
    status = sc_uuid_v4(alloc, &out->id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, tool_name, &out->tool_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, summary, &out->summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_clock_wall(&out->created_at);
    }
    if (!sc_status_is_ok(status)) {
        sc_approval_request_clear(out);
    }
    return status;
}

sc_status sc_approval_response_new(sc_allocator *alloc,
                                   sc_str request_id,
                                   sc_approval_decision decision,
                                   sc_str reason,
                                   sc_approval_response *out)
{
    sc_status status;

    if (out == nullptr || request_id.len == 0 || request_id.ptr == nullptr) {
        return sc_status_invalid_argument("sc.approval.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_approval_response){.struct_size = sizeof(*out), .decision = decision};
    status = sc_string_from_str(alloc, request_id, &out->request_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, reason, &out->reason);
    }
    if (sc_status_is_ok(status)) {
        status = sc_clock_wall(&out->decided_at);
    }
    if (!sc_status_is_ok(status)) {
        sc_approval_response_clear(out);
    }
    return status;
}

bool sc_approval_response_allows(const sc_approval_request *request, const sc_approval_response *response)
{
    if (request == nullptr || response == nullptr || response->decision != SC_APPROVAL_APPROVED) {
        return false;
    }
    return sc_str_equal(sc_string_as_str(&request->id), sc_string_as_str(&response->request_id));
}

void sc_approval_request_clear(sc_approval_request *request)
{
    if (request == nullptr) {
        return;
    }
    sc_string_clear(&request->id);
    sc_string_clear(&request->tool_name);
    sc_string_clear(&request->summary);
    *request = (sc_approval_request){0};
}

void sc_approval_response_clear(sc_approval_response *response)
{
    if (response == nullptr) {
        return;
    }
    sc_string_clear(&response->request_id);
    sc_string_clear(&response->reason);
    *response = (sc_approval_response){0};
}
