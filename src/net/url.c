#include "sc/url.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static sc_status set_part(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status lowercase_host(sc_string *host);
static bool is_scheme_char(char ch);
static bool parse_ipv4(sc_str host, unsigned int octets[4]);

void sc_url_init(sc_url *url, sc_allocator *alloc)
{
    if (url == nullptr) {
        return;
    }
    *url = (sc_url){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
}

sc_status sc_url_parse(sc_allocator *alloc, sc_str input, sc_url *out)
{
    sc_url tmp = {0};
    size_t scheme_end = SIZE_MAX;
    size_t authority_start = 0;
    size_t authority_end = 0;
    size_t host_start = 0;
    size_t host_end = 0;
    size_t at = SIZE_MAX;
    sc_status status;

    if (out == nullptr || input.len == 0 || input.ptr == nullptr) {
        return sc_status_invalid_argument("sc.url.invalid_argument");
    }

    for (size_t i = 0; i < input.len; i += 1) {
        if (input.ptr[i] == ':') {
            scheme_end = i;
            break;
        }
        if (!is_scheme_char(input.ptr[i])) {
            return sc_status_parse("sc.url.invalid_scheme");
        }
    }
    if (scheme_end == SIZE_MAX || scheme_end == 0 || input.len < scheme_end + 3 ||
        input.ptr[scheme_end + 1] != '/' || input.ptr[scheme_end + 2] != '/') {
        return sc_status_parse("sc.url.expected_absolute_url");
    }

    sc_url_init(&tmp, alloc);
    status = set_part(tmp.alloc, input, &tmp.original);
    if (sc_status_is_ok(status)) {
        status = set_part(tmp.alloc, sc_str_from_parts(input.ptr, scheme_end), &tmp.scheme);
    }
    authority_start = scheme_end + 3;
    authority_end = input.len;
    for (size_t i = authority_start; i < input.len; i += 1) {
        if (input.ptr[i] == '/' || input.ptr[i] == '?' || input.ptr[i] == '#') {
            authority_end = i;
            break;
        }
    }
    if (authority_start == authority_end) {
        status = sc_status_parse("sc.url.missing_host");
    }

    for (size_t i = authority_start; sc_status_is_ok(status) && i < authority_end; i += 1) {
        if (input.ptr[i] == '@') {
            at = i;
        }
    }
    host_start = at == SIZE_MAX ? authority_start : at + 1;
    if (at != SIZE_MAX) {
        size_t colon = SIZE_MAX;
        for (size_t i = authority_start; i < at; i += 1) {
            if (input.ptr[i] == ':') {
                colon = i;
                break;
            }
        }
        if (colon == SIZE_MAX) {
            status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[authority_start], at - authority_start), &tmp.username);
        } else {
            status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[authority_start], colon - authority_start), &tmp.username);
            if (sc_status_is_ok(status)) {
                status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[colon + 1], at - colon - 1), &tmp.password);
            }
        }
    }

    host_end = authority_end;
    for (size_t i = host_start; sc_status_is_ok(status) && i < authority_end; i += 1) {
        if (input.ptr[i] == ':') {
            char port_text[8] = {0};
            char *end = nullptr;
            long port = 0;
            size_t port_len = authority_end - i - 1;
            host_end = i;
            if (port_len == 0 || port_len >= sizeof(port_text)) {
                status = sc_status_parse("sc.url.invalid_port");
                break;
            }
            (void)memcpy(port_text, &input.ptr[i + 1], port_len);
            port = strtol(port_text, &end, 10);
            if (end == port_text || *end != '\0' || port <= 0 || port > 65535) {
                status = sc_status_parse("sc.url.invalid_port");
            } else {
                tmp.port = (uint16_t)port;
                tmp.has_port = true;
            }
            break;
        }
    }
    if (sc_status_is_ok(status)) {
        status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[host_start], host_end - host_start), &tmp.host);
    }
    if (sc_status_is_ok(status)) {
        status = lowercase_host(&tmp.host);
    }

    if (sc_status_is_ok(status)) {
        size_t path_start = authority_end;
        size_t query_start = SIZE_MAX;
        size_t fragment_start = SIZE_MAX;
        for (size_t i = authority_end; i < input.len; i += 1) {
            if (input.ptr[i] == '?' && query_start == SIZE_MAX && fragment_start == SIZE_MAX) {
                query_start = i;
            } else if (input.ptr[i] == '#' && fragment_start == SIZE_MAX) {
                fragment_start = i;
            }
        }
        if (path_start < input.len && input.ptr[path_start] == '/') {
            size_t path_end = query_start != SIZE_MAX ? query_start : (fragment_start != SIZE_MAX ? fragment_start : input.len);
            status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[path_start], path_end - path_start), &tmp.path);
        } else {
            status = set_part(tmp.alloc, sc_str_from_cstr("/"), &tmp.path);
        }
        if (sc_status_is_ok(status) && query_start != SIZE_MAX) {
            size_t query_end = fragment_start != SIZE_MAX ? fragment_start : input.len;
            status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[query_start + 1], query_end - query_start - 1), &tmp.query);
        }
        if (sc_status_is_ok(status) && fragment_start != SIZE_MAX) {
            status = set_part(tmp.alloc, sc_str_from_parts(&input.ptr[fragment_start + 1], input.len - fragment_start - 1), &tmp.fragment);
        }
    }

    if (!sc_status_is_ok(status)) {
        sc_url_clear(&tmp);
        return status;
    }

    *out = tmp;
    return sc_status_ok();
}

bool sc_url_has_credentials(const sc_url *url)
{
    return url != nullptr && (url->username.len > 0 || url->password.len > 0);
}

sc_status sc_url_reject_credentials(const sc_url *url)
{
    if (sc_url_has_credentials(url)) {
        return sc_status_security_denied("sc.url.credentials_denied");
    }
    return sc_status_ok();
}

bool sc_url_host_matches_domain(const sc_url *url, sc_str domain)
{
    sc_str host = {0};

    if (url == nullptr || domain.len == 0 || domain.ptr == nullptr) {
        return false;
    }
    host = sc_string_as_str(&url->host);
    if (sc_str_equal(host, domain)) {
        return true;
    }
    if (host.len <= domain.len || host.ptr[host.len - domain.len - 1] != '.') {
        return false;
    }
    return memcmp(&host.ptr[host.len - domain.len], domain.ptr, domain.len) == 0;
}

bool sc_url_host_is_private_address(const sc_url *url)
{
    unsigned int octets[4] = {0};
    sc_str host = {0};

    if (url == nullptr) {
        return false;
    }
    host = sc_string_as_str(&url->host);
    if (sc_str_equal(host, sc_str_from_cstr("localhost"))) {
        return true;
    }
    if (!parse_ipv4(host, octets)) {
        return false;
    }

    return octets[0] == 0 ||
           octets[0] == 10 ||
           octets[0] == 127 ||
           octets[0] >= 224 ||
           (octets[0] == 100 && octets[1] >= 64 && octets[1] <= 127) ||
           (octets[0] == 172 && octets[1] >= 16 && octets[1] <= 31) ||
           (octets[0] == 192 && octets[1] == 168) ||
           (octets[0] == 169 && octets[1] == 254) ||
           (octets[0] == 198 && (octets[1] == 18 || octets[1] == 19));
}

void sc_url_clear(sc_url *url)
{
    if (url == nullptr) {
        return;
    }
    sc_string_clear(&url->original);
    sc_string_clear(&url->scheme);
    sc_string_clear(&url->host);
    sc_string_clear(&url->path);
    sc_string_clear(&url->query);
    sc_string_clear(&url->fragment);
    sc_string_clear(&url->username);
    sc_string_clear(&url->password);
    *url = (sc_url){0};
}

static sc_status set_part(sc_allocator *alloc, sc_str input, sc_string *out)
{
    if (input.len == 0) {
        sc_string_init(out);
        out->alloc = alloc;
        return sc_status_ok();
    }
    return sc_string_from_str(alloc, input, out);
}

static sc_status lowercase_host(sc_string *host)
{
    if (host == nullptr || host->ptr == nullptr || host->len == 0) {
        return sc_status_parse("sc.url.missing_host");
    }
    for (size_t i = 0; i < host->len; i += 1) {
        host->ptr[i] = (char)tolower((unsigned char)host->ptr[i]);
    }
    return sc_status_ok();
}

static bool is_scheme_char(char ch)
{
    return isalnum((unsigned char)ch) || ch == '+' || ch == '-' || ch == '.';
}

static bool parse_ipv4(sc_str host, unsigned int octets[4])
{
    char text[32] = {0};
    char *cursor = text;
    char *end = nullptr;

    if (host.len >= sizeof(text)) {
        return false;
    }
    (void)memcpy(text, host.ptr, host.len);
    for (size_t i = 0; i < 4; i += 1) {
        long value = strtol(cursor, &end, 10);
        if (end == cursor || value < 0 || value > 255) {
            return false;
        }
        octets[i] = (unsigned int)value;
        if (i < 3) {
            if (*end != '.') {
                return false;
            }
            cursor = end + 1;
        } else if (*end != '\0') {
            return false;
        }
    }
    return true;
}
