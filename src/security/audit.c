#include "security/security_internal.h"

static uint64_t audit_hash(const sc_audit_record *record);
static void audit_record_clear(sc_audit_record *record);

void sc_audit_chain_init(sc_audit_chain *chain, sc_allocator *alloc)
{
    if (chain == nullptr) {
        return;
    }
    *chain = (sc_audit_chain){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_vec_init(&chain->records, chain->alloc, sizeof(sc_audit_record));
}

sc_status sc_audit_chain_append(sc_audit_chain *chain, sc_str event_type, sc_str summary)
{
    sc_audit_record record = {0};
    sc_status status;

    if (chain == nullptr || event_type.len == 0 || event_type.ptr == nullptr) {
        return sc_status_invalid_argument("sc.audit.invalid_argument");
    }
    status = sc_string_from_str(chain->alloc, event_type, &record.event_type);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(chain->alloc, summary, &record.summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_clock_wall(&record.timestamp);
    }
    if (sc_status_is_ok(status) && chain->records.len > 0) {
        const sc_audit_record *previous = sc_vec_at_const(&chain->records, chain->records.len - 1);
        record.previous_hash = previous == nullptr ? 0 : previous->hash;
    }
    if (sc_status_is_ok(status)) {
        record.hash = audit_hash(&record);
        status = sc_vec_push(&chain->records, &record);
    }
    if (!sc_status_is_ok(status)) {
        audit_record_clear(&record);
    }
    return status;
}

bool sc_audit_chain_verify(const sc_audit_chain *chain)
{
    uint64_t previous_hash = 0;

    if (chain == nullptr) {
        return false;
    }
    for (size_t i = 0; i < chain->records.len; ++i) {
        const sc_audit_record *record = sc_vec_at_const(&chain->records, i);
        if (record == nullptr || record->previous_hash != previous_hash || audit_hash(record) != record->hash) {
            return false;
        }
        previous_hash = record->hash;
    }
    return true;
}

void sc_audit_chain_clear(sc_audit_chain *chain)
{
    if (chain == nullptr) {
        return;
    }
    for (size_t i = 0; i < chain->records.len; ++i) {
        sc_audit_record *record = sc_vec_at(&chain->records, i);
        audit_record_clear(record);
    }
    sc_vec_clear(&chain->records);
    *chain = (sc_audit_chain){0};
}

static uint64_t audit_hash(const sc_audit_record *record)
{
    uint64_t hash = sc_security_hash_init();
    hash = sc_security_hash_u64(hash, record->previous_hash);
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&record->event_type));
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&record->summary));
    hash = sc_security_hash_u64(hash, (uint64_t)record->timestamp.unix_ns);
    return hash;
}

static void audit_record_clear(sc_audit_record *record)
{
    if (record == nullptr) {
        return;
    }
    sc_string_clear(&record->event_type);
    sc_string_clear(&record->summary);
    *record = (sc_audit_record){0};
}
