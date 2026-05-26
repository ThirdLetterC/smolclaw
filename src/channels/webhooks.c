#include "sc/channel.h"

#include "sc/json.h"

#include <sodium.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct webhook_endpoint {
    sc_string name;
    bool enabled;
    sc_string path;
    sc_string secret;
    sc_vec allow_list;
    sc_string dispatch_to;
    sc_string template_text;
    sc_webhook_reply_mode reply_mode;
    uint32_t rate_limit_per_sec;
    size_t max_body_bytes;
    time_t rate_window;
    uint32_t rate_count;
} webhook_endpoint;

struct sc_webhook_router {
    sc_allocator *alloc;
    sc_vec endpoints;
};

static sc_status copy_string(sc_allocator *alloc, sc_str value, sc_string *out);
static sc_status endpoint_copy(sc_allocator *alloc, const sc_webhook_named_options *options, webhook_endpoint *out);
static void endpoint_clear(webhook_endpoint *endpoint);
static sc_status string_vec_copy(sc_allocator *alloc, const sc_str *values, size_t count, sc_vec *out);
static void string_vec_clear(sc_vec *vec);
static bool string_vec_allows(const sc_vec *vec, sc_str value);
static webhook_endpoint *endpoint_find(sc_webhook_router *router, sc_str path);
static sc_status endpoint_rate_check(webhook_endpoint *endpoint);
static sc_status endpoint_verify_signature(const webhook_endpoint *endpoint, const sc_webhook_named_request *request);
static sc_status hmac_sha256_hex(sc_str secret, sc_str body, sc_allocator *alloc, sc_string *out);
static sc_status hmac_sha256_hex2(sc_str secret, sc_str prefix, sc_str body, sc_allocator *alloc, sc_string *out);
static sc_status hmac_sha256_base64(sc_str secret, sc_str body, sc_allocator *alloc, sc_string *out);
static bool signature_matches(sc_str provided, sc_str expected);
static sc_str strip_signature_prefix(sc_str signature);
static sc_webhook_dispatch_kind dispatch_kind_from_target(sc_str target, sc_str *trimmed);
static sc_status render_template(sc_allocator *alloc, sc_str template_text, sc_str body, sc_string *out);
static sc_status render_payload_field(sc_allocator *alloc, sc_str body, sc_str field, sc_string *out);
static sc_status response_for_mode(sc_allocator *alloc,
                                   sc_webhook_reply_mode mode,
                                   sc_str rendered,
                                   sc_string *out);
static sc_status append_json_string(sc_string_builder *builder, sc_str value);
static bool str_has_prefix(sc_str value, const char *prefix);

sc_status sc_webhook_router_new(sc_allocator *alloc, sc_webhook_router **out)
{
    sc_webhook_router *router = nullptr;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.webhook_router.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    router = sc_alloc(alloc, sizeof(*router), _Alignof(sc_webhook_router));
    if (router == nullptr) {
        return sc_status_no_memory();
    }
    *router = (sc_webhook_router){.alloc = alloc};
    sc_vec_init(&router->endpoints, alloc, sizeof(webhook_endpoint));
    *out = router;
    return sc_status_ok();
}

sc_status sc_webhook_router_add(sc_webhook_router *router, const sc_webhook_named_options *options)
{
    webhook_endpoint endpoint = {0};
    sc_status status;

    if (router == nullptr || options == nullptr || options->name.len == 0 || options->path.len == 0) {
        return sc_status_invalid_argument("sc.webhook_router.add_invalid_argument");
    }
    status = endpoint_copy(router->alloc, options, &endpoint);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&router->endpoints, &endpoint);
    }
    if (!sc_status_is_ok(status)) {
        endpoint_clear(&endpoint);
    }
    return status;
}

sc_status sc_webhook_router_ingest(sc_webhook_router *router,
                                   const sc_webhook_named_request *request,
                                   sc_allocator *alloc,
                                   sc_webhook_named_result *out)
{
    webhook_endpoint *endpoint = nullptr;
    sc_str dispatch_target = {0};
    sc_string rendered = {0};
    sc_status status;

    if (router == nullptr || request == nullptr || out == nullptr || request->body.ptr == nullptr) {
        return sc_status_invalid_argument("sc.webhook_router.ingest_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_webhook_named_result){.struct_size = sizeof(*out), .http_status = 500};
    endpoint = endpoint_find(router, request->path);
    if (endpoint == nullptr || !endpoint->enabled) {
        return sc_status_invalid_argument("sc.webhook_router.not_found");
    }
    if (!sc_str_equal(request->method, sc_str_from_cstr("POST"))) {
        return sc_status_invalid_argument("sc.webhook_router.method_rejected");
    }
    if (request->body.len > endpoint->max_body_bytes) {
        return sc_status_invalid_argument("sc.webhook_router.body_too_large");
    }
    if (!string_vec_allows(&endpoint->allow_list, request->remote_addr)) {
        return sc_status_security_denied("sc.webhook_router.allow_list_denied");
    }
    status = endpoint_rate_check(endpoint);
    if (sc_status_is_ok(status)) {
        status = endpoint_verify_signature(endpoint, request);
    }
    if (sc_status_is_ok(status)) {
        status = render_template(alloc, sc_string_as_str(&endpoint->template_text), request->body, &rendered);
    }
    if (sc_status_is_ok(status)) {
        out->dispatch_kind = dispatch_kind_from_target(sc_string_as_str(&endpoint->dispatch_to), &dispatch_target);
        status = copy_string(alloc, dispatch_target, &out->dispatch_target);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&rendered), &out->rendered_text);
    }
    if (sc_status_is_ok(status)) {
        out->reply_mode = endpoint->reply_mode;
        out->http_status = 200;
        status = response_for_mode(alloc, endpoint->reply_mode, sc_string_as_str(&rendered), &out->response_body);
    }
    sc_string_clear(&rendered);
    return status;
}

void sc_webhook_named_result_clear(sc_webhook_named_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->dispatch_target);
    sc_string_clear(&result->rendered_text);
    sc_string_clear(&result->response_body);
    *result = (sc_webhook_named_result){0};
}

void sc_webhook_router_destroy(sc_webhook_router *router)
{
    if (router == nullptr) {
        return;
    }
    for (size_t i = 0; i < router->endpoints.len; i += 1) {
        webhook_endpoint *endpoint = sc_vec_at(&router->endpoints, i);
        endpoint_clear(endpoint);
    }
    sc_vec_clear(&router->endpoints);
    sc_free(router->alloc, router, sizeof(*router), _Alignof(sc_webhook_router));
}

static sc_status copy_string(sc_allocator *alloc, sc_str value, sc_string *out)
{
    return sc_string_from_str(alloc, value.ptr == nullptr ? sc_str_from_cstr("") : value, out);
}

static sc_status endpoint_copy(sc_allocator *alloc, const sc_webhook_named_options *options, webhook_endpoint *out)
{
    sc_status status;

    *out = (webhook_endpoint){
        .enabled = options->enabled,
        .reply_mode = options->reply_mode,
        .rate_limit_per_sec = options->rate_limit_per_sec == 0 ? 10 : options->rate_limit_per_sec,
        .max_body_bytes = options->max_body_bytes == 0 ? 65'536 : options->max_body_bytes,
    };
    sc_vec_init(&out->allow_list, alloc, sizeof(sc_string));
    status = copy_string(alloc, options->name, &out->name);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->path, &out->path);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->secret, &out->secret);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->dispatch_to, &out->dispatch_to);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->template_text, &out->template_text);
    }
    if (sc_status_is_ok(status)) {
        status = string_vec_copy(alloc, options->allow_list, options->allow_list_count, &out->allow_list);
    }
    if (!sc_status_is_ok(status)) {
        endpoint_clear(out);
    }
    return status;
}

static void endpoint_clear(webhook_endpoint *endpoint)
{
    if (endpoint == nullptr) {
        return;
    }
    sc_string_clear(&endpoint->name);
    sc_string_clear(&endpoint->path);
    sc_string_secure_clear(&endpoint->secret);
    sc_string_clear(&endpoint->dispatch_to);
    sc_string_clear(&endpoint->template_text);
    string_vec_clear(&endpoint->allow_list);
    *endpoint = (webhook_endpoint){0};
}

static sc_status string_vec_copy(sc_allocator *alloc, const sc_str *values, size_t count, sc_vec *out)
{
    sc_status status = sc_status_ok();

    for (size_t i = 0; sc_status_is_ok(status) && i < count; i += 1) {
        sc_string copy = {0};
        status = copy_string(alloc, values[i], &copy);
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(out, &copy);
        }
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&copy);
        }
    }
    return status;
}

static void string_vec_clear(sc_vec *vec)
{
    if (vec == nullptr) {
        return;
    }
    for (size_t i = 0; i < vec->len; i += 1) {
        sc_string *value = sc_vec_at(vec, i);
        sc_string_clear(value);
    }
    sc_vec_clear(vec);
}

static bool string_vec_allows(const sc_vec *vec, sc_str value)
{
    if (vec == nullptr || vec->len == 0) {
        return true;
    }
    for (size_t i = 0; i < vec->len; i += 1) {
        const sc_string *candidate = sc_vec_at_const(vec, i);
        if (candidate != nullptr &&
            (sc_str_equal(sc_string_as_str(candidate), sc_str_from_cstr("*")) ||
             sc_str_equal(sc_string_as_str(candidate), value))) {
            return true;
        }
    }
    return false;
}

static webhook_endpoint *endpoint_find(sc_webhook_router *router, sc_str path)
{
    if (router == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < router->endpoints.len; i += 1) {
        webhook_endpoint *endpoint = sc_vec_at(&router->endpoints, i);
        if (endpoint != nullptr && sc_str_equal(sc_string_as_str(&endpoint->path), path)) {
            return endpoint;
        }
    }
    return nullptr;
}

static sc_status endpoint_rate_check(webhook_endpoint *endpoint)
{
    time_t now = time(nullptr);

    if (endpoint == nullptr) {
        return sc_status_invalid_argument("sc.webhook_router.rate_invalid_argument");
    }
    if (endpoint->rate_window != now) {
        endpoint->rate_window = now;
        endpoint->rate_count = 0;
    }
    if (endpoint->rate_count >= endpoint->rate_limit_per_sec) {
        return sc_status_security_denied("sc.webhook_router.rate_limited");
    }
    endpoint->rate_count += 1;
    return sc_status_ok();
}

static sc_status endpoint_verify_signature(const webhook_endpoint *endpoint, const sc_webhook_named_request *request)
{
    sc_string expected = {0};
    sc_status status = sc_status_ok();
    bool matched = false;

    if (endpoint == nullptr || request == nullptr || endpoint->secret.len == 0) {
        return sc_status_ok();
    }
    if (request->x_hub_signature_256.len == 0 && request->x_signature.len == 0 &&
        request->x_line_signature.len == 0 && request->x_nextcloud_talk_signature.len == 0) {
        return sc_status_security_denied("sc.webhook_router.signature_missing");
    }
    if (request->x_hub_signature_256.len > 0 || request->x_signature.len > 0) {
        status = hmac_sha256_hex(sc_string_as_str(&endpoint->secret), request->body, endpoint->secret.alloc, &expected);
        if (sc_status_is_ok(status)) {
            matched = signature_matches(strip_signature_prefix(request->x_hub_signature_256), sc_string_as_str(&expected)) ||
                      signature_matches(strip_signature_prefix(request->x_signature), sc_string_as_str(&expected));
        }
        sc_string_clear(&expected);
    }
    if (sc_status_is_ok(status) && !matched && request->x_nextcloud_talk_signature.len > 0) {
        status = hmac_sha256_hex2(sc_string_as_str(&endpoint->secret),
                                  request->x_nextcloud_talk_random,
                                  request->body,
                                  endpoint->secret.alloc,
                                  &expected);
        if (sc_status_is_ok(status)) {
            matched = signature_matches(request->x_nextcloud_talk_signature, sc_string_as_str(&expected));
        }
        sc_string_clear(&expected);
    }
    if (sc_status_is_ok(status) && !matched && request->x_line_signature.len > 0) {
        status = hmac_sha256_base64(sc_string_as_str(&endpoint->secret), request->body, endpoint->secret.alloc, &expected);
        if (sc_status_is_ok(status)) {
            matched = signature_matches(request->x_line_signature, sc_string_as_str(&expected));
        }
        sc_string_clear(&expected);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return matched ? sc_status_ok() : sc_status_security_denied("sc.webhook_router.signature_denied");
}

static sc_status hmac_sha256_hex(sc_str secret, sc_str body, sc_allocator *alloc, sc_string *out)
{
    return hmac_sha256_hex2(secret, sc_str_from_cstr(""), body, alloc, out);
}

static sc_status hmac_sha256_hex2(sc_str secret, sc_str prefix, sc_str body, sc_allocator *alloc, sc_string *out)
{
    unsigned char digest[crypto_auth_hmacsha256_BYTES] = {0};
    char hex[crypto_auth_hmacsha256_BYTES * 2u + 1u] = {0};
    crypto_auth_hmacsha256_state state = {0};

    if (sodium_init() < 0) {
        return sc_status_unsupported("sc.webhook_router.crypto_unavailable");
    }
    crypto_auth_hmacsha256_init(&state, (const unsigned char *)secret.ptr, secret.len);
    if (prefix.ptr != nullptr && prefix.len > 0) {
        crypto_auth_hmacsha256_update(&state, (const unsigned char *)prefix.ptr, prefix.len);
    }
    crypto_auth_hmacsha256_update(&state, (const unsigned char *)body.ptr, body.len);
    crypto_auth_hmacsha256_final(&state, digest);
    sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
    sodium_memzero(digest, sizeof(digest));
    sodium_memzero(&state, sizeof(state));
    return sc_string_from_cstr(alloc, hex, out);
}

static sc_status hmac_sha256_base64(sc_str secret, sc_str body, sc_allocator *alloc, sc_string *out)
{
    unsigned char digest[crypto_auth_hmacsha256_BYTES] = {0};
    char encoded[sodium_base64_ENCODED_LEN(crypto_auth_hmacsha256_BYTES, sodium_base64_VARIANT_ORIGINAL)] = {0};
    crypto_auth_hmacsha256_state state = {0};

    if (sodium_init() < 0) {
        return sc_status_unsupported("sc.webhook_router.crypto_unavailable");
    }
    crypto_auth_hmacsha256_init(&state, (const unsigned char *)secret.ptr, secret.len);
    crypto_auth_hmacsha256_update(&state, (const unsigned char *)body.ptr, body.len);
    crypto_auth_hmacsha256_final(&state, digest);
    sodium_bin2base64(encoded, sizeof(encoded), digest, sizeof(digest), sodium_base64_VARIANT_ORIGINAL);
    sodium_memzero(digest, sizeof(digest));
    sodium_memzero(&state, sizeof(state));
    return sc_string_from_cstr(alloc, encoded, out);
}

static bool signature_matches(sc_str provided, sc_str expected)
{
    if (provided.len == 0 || expected.len == 0 || provided.len != expected.len) {
        return false;
    }
    return sodium_memcmp(provided.ptr, expected.ptr, expected.len) == 0;
}

static sc_str strip_signature_prefix(sc_str signature)
{
    if (str_has_prefix(signature, "sha256=")) {
        return sc_str_from_parts(signature.ptr + 7, signature.len - 7);
    }
    return signature;
}

static sc_webhook_dispatch_kind dispatch_kind_from_target(sc_str target, sc_str *trimmed)
{
    if (str_has_prefix(target, "cron:")) {
        *trimmed = sc_str_from_parts(target.ptr + 5, target.len - 5);
        return SC_WEBHOOK_DISPATCH_CRON;
    }
    if (str_has_prefix(target, "system:")) {
        *trimmed = sc_str_from_parts(target.ptr + 7, target.len - 7);
        return SC_WEBHOOK_DISPATCH_SYSTEM_PROMPT;
    }
    if (str_has_prefix(target, "conversation:")) {
        *trimmed = sc_str_from_parts(target.ptr + 13, target.len - 13);
        return SC_WEBHOOK_DISPATCH_CONVERSATION;
    }
    *trimmed = target;
    return SC_WEBHOOK_DISPATCH_CONVERSATION;
}

static sc_status render_template(sc_allocator *alloc, sc_str template_text, sc_str body, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    size_t pos = 0;

    if (template_text.len == 0) {
        return copy_string(alloc, body, out);
    }
    sc_string_builder_init(&builder, alloc);
    while (sc_status_is_ok(status) && pos < template_text.len) {
        size_t start = pos;
        while (start + 1 < template_text.len && !(template_text.ptr[start] == '{' && template_text.ptr[start + 1] == '{')) {
            start += 1;
        }
        status = sc_string_builder_append(&builder, sc_str_from_parts(template_text.ptr + pos, start - pos));
        if (!sc_status_is_ok(status) || start + 1 >= template_text.len) {
            break;
        }
        size_t expr_start = start + 2;
        size_t end = expr_start;
        while (end + 1 < template_text.len && !(template_text.ptr[end] == '}' && template_text.ptr[end + 1] == '}')) {
            end += 1;
        }
        if (end + 1 >= template_text.len) {
            status = sc_string_builder_append(&builder, sc_str_from_parts(template_text.ptr + start, template_text.len - start));
            break;
        }
        sc_str expr = sc_str_from_parts(template_text.ptr + expr_start, end - expr_start);
        while (expr.len > 0 && (expr.ptr[0] == ' ' || expr.ptr[0] == '\t')) {
            expr.ptr += 1;
            expr.len -= 1;
        }
        while (expr.len > 0 && (expr.ptr[expr.len - 1] == ' ' || expr.ptr[expr.len - 1] == '\t')) {
            expr.len -= 1;
        }
        if (sc_str_equal(expr, sc_str_from_cstr("payload"))) {
            status = sc_string_builder_append(&builder, body);
        } else if (str_has_prefix(expr, "payload.")) {
            sc_string field = {0};
            status = render_payload_field(alloc, body, sc_str_from_parts(expr.ptr + 8, expr.len - 8), &field);
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&field));
            }
            sc_string_clear(&field);
        }
        pos = end + 2;
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status render_payload_field(sc_allocator *alloc, sc_str body, sc_str field, sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_str text = {0};
    double number = 0.0;
    sc_status status = sc_json_parse(alloc, body, &root, nullptr);

    if (sc_status_is_ok(status)) {
        sc_json_value *value = sc_json_object_get(root, field);
        if (sc_json_as_str(value, &text)) {
            status = copy_string(alloc, text, out);
        } else if (sc_json_as_number(value, &number)) {
            char number_text[64] = {0};

            (void)snprintf(number_text, sizeof(number_text), "%.0f", number);
            status = copy_string(alloc, sc_str_from_cstr(number_text), out);
        } else {
            status = copy_string(alloc, sc_str_from_cstr(""), out);
        }
    }
    sc_json_destroy(root);
    return status;
}

static sc_status response_for_mode(sc_allocator *alloc,
                                   sc_webhook_reply_mode mode,
                                   sc_str rendered,
                                   sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (mode == SC_WEBHOOK_REPLY_STATUS_ONLY) {
        return copy_string(alloc, sc_str_from_cstr("OK"), out);
    }
    if (mode == SC_WEBHOOK_REPLY_INLINE_TEXT) {
        return copy_string(alloc, rendered, out);
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"text\":\"");
    if (sc_status_is_ok(status)) {
        status = append_json_string(&builder, rendered);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\"}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_json_string(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_status_ok();

    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char ch = value.ptr[i];
        if (ch == '"' || ch == '\\') {
            char escaped[2] = {'\\', ch};
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(&ch, 1));
        }
    }
    return status;
}

static bool str_has_prefix(sc_str value, const char *prefix)
{
    size_t len = prefix == nullptr ? 0 : strlen(prefix);
    return value.ptr != nullptr && value.len >= len && memcmp(value.ptr, prefix, len) == 0;
}
