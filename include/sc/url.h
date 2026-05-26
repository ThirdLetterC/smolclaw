#pragma once

#include <stdint.h>

#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_url {
    sc_allocator *alloc;
    sc_string original;
    sc_string scheme;
    sc_string host;
    sc_string path;
    sc_string query;
    sc_string fragment;
    sc_string username;
    sc_string password;
    uint16_t port;
    bool has_port;
} sc_url;

void sc_url_init(sc_url *url, sc_allocator *alloc);
sc_status sc_url_parse(sc_allocator *alloc, sc_str input, sc_url *out);
bool sc_url_has_credentials(const sc_url *url);
sc_status sc_url_reject_credentials(const sc_url *url);
bool sc_url_host_matches_domain(const sc_url *url, sc_str domain);
bool sc_url_host_is_private_address(const sc_url *url);
void sc_url_clear(sc_url *url);
