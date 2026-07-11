#include "security/security_internal.h"

#include <string.h>

static bool domain_list_matches(const sc_vec *domains, const sc_url *url);
static bool secret_key(const char *key);
static bool span_contains_ascii_ci(sc_str haystack, const char *needle);
static bool receipt_token_at(sc_str text, size_t pos);

sc_status sc_security_validate_url(const sc_security_policy *policy, sc_str url_text)
{
    sc_url url = {0};
    sc_status status;

    if (policy == nullptr || url_text.len == 0 || url_text.ptr == nullptr) {
        return sc_status_invalid_argument("sc.security.url_invalid_argument");
    }
    status = sc_url_parse(policy->alloc, url_text, &url);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (!sc_str_equal(sc_string_as_str(&url.scheme), sc_str_from_cstr("http")) &&
        !sc_str_equal(sc_string_as_str(&url.scheme), sc_str_from_cstr("https"))) {
        status = sc_status_security_denied("sc.security.url_scheme_denied");
    }
    if (!policy->url_credentials_allowed) {
        if (sc_status_is_ok(status)) {
            status = sc_url_reject_credentials(&url);
        }
    }
    if (sc_status_is_ok(status) && domain_list_matches(&policy->denied_domains, &url)) {
        status = sc_status_security_denied("sc.security.domain_denied");
    }
    if (sc_status_is_ok(status) && policy->allowed_domains.len > 0 &&
        !domain_list_matches(&policy->allowed_domains, &url)) {
        status = sc_status_security_denied("sc.security.domain_not_allowed");
    }
    if (sc_status_is_ok(status) && policy->private_network_policy == SC_PRIVATE_NETWORK_BLOCK &&
        sc_url_host_is_private_address(&url)) {
        status = sc_status_security_denied("sc.security.private_network_denied");
    }
    sc_url_clear(&url);
    return status;
}

sc_status sc_security_validate_redirect(const sc_security_policy *policy,
                                        sc_str original_url,
                                        sc_str redirect_url)
{
    sc_status status = sc_security_validate_url(policy, original_url);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_security_validate_url(policy, redirect_url);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_status_ok();
}

sc_status sc_security_validate_sandbox_network(const sc_security_policy *policy, sc_str url_text)
{
    sc_url url = {0};
    sc_status status;

    if (policy == nullptr || url_text.len == 0 || url_text.ptr == nullptr) {
        return sc_status_invalid_argument("sc.security.sandbox_network_invalid_argument");
    }
    if (policy->sandbox_network == SC_SANDBOX_NETWORK_FULL) {
        return sc_status_ok();
    }
    if (policy->sandbox_network == SC_SANDBOX_NETWORK_NONE) {
        return sc_status_security_denied("sc.security.sandbox_network_denied");
    }
    status = sc_url_parse(policy->alloc, url_text, &url);
    if (sc_status_is_ok(status) && !domain_list_matches(&policy->sandbox_allowed_domains, &url)) {
        status = sc_status_security_denied("sc.security.sandbox_domain_not_allowed");
    }
    sc_url_clear(&url);
    return status;
}

sc_status sc_security_validate_prompt_injection(sc_str text)
{
    static const char *needles[] = {
        "ignore previous instructions",
        "ignore all previous",
        "developer message",
        "system prompt",
        "reveal hidden",
        "exfiltrate",
        "disable safety",
        "bypass policy",
    };

    if (text.ptr == nullptr || text.len == 0) {
        return sc_status_ok();
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(needles); i += 1) {
        if (span_contains_ascii_ci(text, needles[i])) {
            return sc_status_security_denied("sc.security.prompt_injection_detected");
        }
    }
    return sc_status_ok();
}

sc_status sc_security_validate_outbound_message(sc_str text)
{
    static const char *needles[] = {
        "authorization:",
        "api_key",
        "secret=",
        "token=",
        "password=",
        "begin private key",
        "sk-",
    };

    if (text.ptr == nullptr || text.len == 0) {
        return sc_status_ok();
    }
    for (size_t i = 0; i < text.len; i += 1) {
        for (size_t j = 0; j < SC_ARRAY_LEN(needles); j += 1) {
            size_t needle_len = strlen(needles[j]);
            if (i + needle_len <= text.len &&
                span_contains_ascii_ci(sc_str_from_parts(text.ptr + i, needle_len), needles[j]) &&
                !receipt_token_at(text, i)) {
                return sc_status_security_denied("sc.security.outbound_leak_detected");
            }
        }
    }
    return sc_status_ok();
}

sc_status sc_security_validate_pairing(bool required, bool paired, sc_str subject)
{
    (void)subject;
    if (required && !paired) {
        return sc_status_security_denied("sc.security.pairing_required");
    }
    return sc_status_ok();
}

sc_str sc_security_redact_value_for_key(const char *key, sc_str value)
{
    static const char redacted[] = "[REDACTED]";

    if (secret_key(key)) {
        return sc_str_from_parts(redacted, sizeof(redacted) - 1);
    }
    return value;
}

static bool domain_list_matches(const sc_vec *domains, const sc_url *url)
{
    if (domains == nullptr || url == nullptr) {
        return false;
    }
    for (size_t i = 0; i < domains->len; ++i) {
        const sc_string *domain = sc_vec_at_const(domains, i);
        if (domain != nullptr && sc_url_host_matches_domain(url, sc_string_as_str(domain))) {
            return true;
        }
    }
    return false;
}

static bool secret_key(const char *key)
{
    return key != nullptr &&
           (strstr(key, "key") != nullptr ||
            strstr(key, "token") != nullptr ||
            strstr(key, "secret") != nullptr ||
            strstr(key, "password") != nullptr ||
            strstr(key, "authorization") != nullptr ||
            strstr(key, "cookie") != nullptr);
}

static bool span_contains_ascii_ci(sc_str haystack, const char *needle)
{
    size_t needle_len = needle == nullptr ? 0U : strlen(needle);

    if (haystack.ptr == nullptr || needle_len == 0 || haystack.len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack.len; i += 1) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j += 1) {
            char left = haystack.ptr[i + j];
            char right = needle[j];
            if (left >= 'A' && left <= 'Z') {
                left = (char)(left - 'A' + 'a');
            }
            if (right >= 'A' && right <= 'Z') {
                right = (char)(right - 'A' + 'a');
            }
            if (left != right) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static bool receipt_token_at(sc_str text, size_t pos)
{
    const char prefix[] = "sc-receipt-";
    size_t prefix_len = sizeof(prefix) - 1u;

    if (text.ptr == nullptr || pos + prefix_len > text.len) {
        return false;
    }
    return memcmp(text.ptr + pos, prefix, prefix_len) == 0;
}
