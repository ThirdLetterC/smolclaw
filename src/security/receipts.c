#include "security/security_internal.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>

static uint64_t receipt_hash(const sc_tool_receipt *receipt);
static sc_status receipt_token(sc_receipt_chain *chain, const sc_tool_receipt *receipt, sc_string *out);
static void receipt_mac_update_str(crypto_auth_hmacsha256_state *state, sc_str value);
static void receipt_mac_update_u64(crypto_auth_hmacsha256_state *state, uint64_t value);
static void receipt_mac_update_bool(crypto_auth_hmacsha256_state *state, bool value);
static void receipt_clear(sc_tool_receipt *receipt);

void sc_receipt_chain_init(sc_receipt_chain *chain, sc_allocator *alloc)
{
    if (chain == nullptr) {
        return;
    }
    *chain = (sc_receipt_chain){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_vec_init(&chain->receipts, chain->alloc, sizeof(sc_tool_receipt));
    if (sodium_init() >= 0) {
        randombytes_buf(chain->session_key, sizeof(chain->session_key));
        chain->key_initialized = true;
    }
}

sc_status sc_receipt_chain_append(sc_receipt_chain *chain,
                                  sc_str tool_name,
                                  sc_str args_summary,
                                  sc_str output_summary,
                                  bool success)
{
    return sc_receipt_chain_append_ex(chain,
                                      tool_name,
                                      args_summary,
                                      output_summary,
                                      success,
                                      sc_str_from_cstr("allowed"),
                                      success ? sc_str_from_parts(nullptr, 0) : sc_str_from_cstr("sc.tool.failed"),
                                      success ? sc_str_from_cstr("ok") : sc_str_from_cstr("error"));
}

sc_status sc_receipt_chain_append_ex(sc_receipt_chain *chain,
                                     sc_str tool_name,
                                     sc_str args_summary,
                                     sc_str output_summary,
                                     bool success,
                                     sc_str policy_decision,
                                     sc_str failure_reason,
                                     sc_str outcome)
{
    sc_tool_receipt receipt = {0};
    sc_status status;

    if (chain == nullptr || tool_name.len == 0 || tool_name.ptr == nullptr) {
        return sc_status_invalid_argument("sc.receipt.invalid_argument");
    }
    status = sc_string_from_str(chain->alloc, tool_name, &receipt.tool_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(chain->alloc, args_summary, &receipt.args_summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(chain->alloc, output_summary, &receipt.output_summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(chain->alloc, policy_decision, &receipt.policy_decision);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(chain->alloc, failure_reason, &receipt.failure_reason);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(chain->alloc, outcome, &receipt.outcome);
    }
    if (sc_status_is_ok(status)) {
        status = sc_clock_wall(&receipt.started_at);
    }
    if (sc_status_is_ok(status)) {
        receipt.ended_at = receipt.started_at;
        receipt.success = success;
        if (chain->receipts.len > 0) {
            const sc_tool_receipt *previous = sc_vec_at_const(&chain->receipts, chain->receipts.len - 1);
            /* Link each receipt to the last one so deletion or reordering is visible to verification. */
            receipt.previous_hash = previous == nullptr ? 0 : previous->hash;
        }
        receipt.hash = receipt_hash(&receipt);
        status = receipt_token(chain, &receipt, &receipt.token);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&chain->receipts, &receipt);
    }
    if (!sc_status_is_ok(status)) {
        receipt_clear(&receipt);
    }
    return status;
}

bool sc_receipt_chain_verify(const sc_receipt_chain *chain)
{
    uint64_t previous_hash = 0;

    if (chain == nullptr) {
        return false;
    }
    for (size_t i = 0; i < chain->receipts.len; ++i) {
        const sc_tool_receipt *receipt = sc_vec_at_const(&chain->receipts, i);
        if (receipt == nullptr || receipt->previous_hash != previous_hash || receipt_hash(receipt) != receipt->hash ||
            !sc_receipt_chain_verify_token(chain, i, sc_string_as_str(&receipt->token))) {
            return false;
        }
        previous_hash = receipt->hash;
    }
    return true;
}

bool sc_receipt_chain_verify_token(const sc_receipt_chain *chain, size_t index, sc_str token)
{
    sc_receipt_chain *mutable_chain = (sc_receipt_chain *)chain;
    const sc_tool_receipt *receipt = nullptr;
    sc_string expected = {0};
    bool ok = false;

    if (chain == nullptr || !chain->key_initialized || index >= chain->receipts.len ||
        token.ptr == nullptr || token.len == 0) {
        return false;
    }
    receipt = sc_vec_at_const(&chain->receipts, index);
    if (receipt == nullptr || !sc_status_is_ok(receipt_token(mutable_chain, receipt, &expected))) {
        sc_string_clear(&expected);
        return false;
    }
    ok = sc_str_equal(sc_string_as_str(&expected), token);
    sc_string_clear(&expected);
    return ok;
}

void sc_receipt_chain_clear(sc_receipt_chain *chain)
{
    if (chain == nullptr) {
        return;
    }
    for (size_t i = 0; i < chain->receipts.len; ++i) {
        sc_tool_receipt *receipt = sc_vec_at(&chain->receipts, i);
        receipt_clear(receipt);
    }
    sc_vec_clear(&chain->receipts);
    if (chain->key_initialized) {
        sodium_memzero(chain->session_key, sizeof(chain->session_key));
    }
    *chain = (sc_receipt_chain){0};
}

static uint64_t receipt_hash(const sc_tool_receipt *receipt)
{
    uint64_t hash = sc_security_hash_init();
    hash = sc_security_hash_u64(hash, receipt->previous_hash);
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&receipt->tool_name));
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&receipt->args_summary));
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&receipt->output_summary));
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&receipt->policy_decision));
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&receipt->failure_reason));
    hash = sc_security_hash_bytes(hash, sc_string_as_str(&receipt->outcome));
    hash = sc_security_hash_u64(hash, (uint64_t)receipt->started_at.unix_ns);
    hash = sc_security_hash_u64(hash, (uint64_t)receipt->ended_at.unix_ns);
    hash = sc_security_hash_bool(hash, receipt->success);
    return hash;
}

static sc_status receipt_token(sc_receipt_chain *chain, const sc_tool_receipt *receipt, sc_string *out)
{
    crypto_auth_hmacsha256_state state;
    unsigned char digest[crypto_auth_hmacsha256_BYTES] = {0};
    char encoded[sodium_base64_ENCODED_LEN(crypto_auth_hmacsha256_BYTES, sodium_base64_VARIANT_URLSAFE_NO_PADDING)] = {0};
    char token[128] = {0};
    long long epoch_seconds = 0;
    int written = 0;

    if (chain == nullptr || receipt == nullptr || out == nullptr || !chain->key_initialized) {
        return sc_status_unsupported("sc.receipt.crypto_unavailable");
    }
    /*
     * The token signs the same fields as the public hash, but with a session
     * HMAC. Strings are length-prefixed below to avoid ambiguous concatenation.
     */
    crypto_auth_hmacsha256_init(&state, chain->session_key, sizeof(chain->session_key));
    receipt_mac_update_u64(&state, receipt->previous_hash);
    receipt_mac_update_str(&state, sc_string_as_str(&receipt->tool_name));
    receipt_mac_update_str(&state, sc_string_as_str(&receipt->args_summary));
    receipt_mac_update_str(&state, sc_string_as_str(&receipt->output_summary));
    receipt_mac_update_str(&state, sc_string_as_str(&receipt->policy_decision));
    receipt_mac_update_str(&state, sc_string_as_str(&receipt->failure_reason));
    receipt_mac_update_str(&state, sc_string_as_str(&receipt->outcome));
    receipt_mac_update_u64(&state, (uint64_t)receipt->started_at.unix_ns);
    receipt_mac_update_u64(&state, (uint64_t)receipt->ended_at.unix_ns);
    receipt_mac_update_bool(&state, receipt->success);
    crypto_auth_hmacsha256_final(&state, digest);
    (void)sodium_bin2base64(encoded,
                            sizeof(encoded),
                            digest,
                            sizeof(digest),
                            sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    epoch_seconds = receipt->ended_at.unix_ns / 1000000000LL;
    written = snprintf(token, sizeof(token), "sc-receipt-%lld-%s", epoch_seconds, encoded);
    sodium_memzero(digest, sizeof(digest));
    sodium_memzero(&state, sizeof(state));
    if (written <= 0 || (size_t)written >= sizeof(token)) {
        return sc_status_io("sc.receipt.format_failed");
    }
    return sc_string_from_cstr(chain->alloc, token, out);
}

static void receipt_mac_update_str(crypto_auth_hmacsha256_state *state, sc_str value)
{
    /* Length-prefix string fields so ["ab", "c"] cannot authenticate as ["a", "bc"]. */
    receipt_mac_update_u64(state, (uint64_t)value.len);
    if (value.ptr != nullptr && value.len > 0) {
        crypto_auth_hmacsha256_update(state, (const unsigned char *)value.ptr, value.len);
    }
}

static void receipt_mac_update_u64(crypto_auth_hmacsha256_state *state, uint64_t value)
{
    unsigned char bytes[8] = {0};

    for (size_t i = 0; i < sizeof(bytes); i += 1) {
        bytes[i] = (unsigned char)((value >> (i * 8u)) & 0xFFu);
    }
    crypto_auth_hmacsha256_update(state, bytes, sizeof(bytes));
}

static void receipt_mac_update_bool(crypto_auth_hmacsha256_state *state, bool value)
{
    receipt_mac_update_u64(state, value ? 1u : 0u);
}

static void receipt_clear(sc_tool_receipt *receipt)
{
    if (receipt == nullptr) {
        return;
    }
    sc_string_clear(&receipt->tool_name);
    sc_string_clear(&receipt->args_summary);
    sc_string_clear(&receipt->output_summary);
    sc_string_clear(&receipt->token);
    sc_string_clear(&receipt->policy_decision);
    sc_string_clear(&receipt->failure_reason);
    sc_string_clear(&receipt->outcome);
    *receipt = (sc_tool_receipt){0};
}
