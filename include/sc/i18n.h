#pragma once

#include "sc/api.h"
#include "sc/allocator.h"
#include "sc/result.h"
#include "sc/string.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

typedef struct sc_i18n_catalog {
    sc_allocator *alloc;
    sc_string locale;
    sc_vec entries;
} sc_i18n_catalog;

typedef struct sc_i18n_arg {
    sc_str name;
    sc_str value;
    bool secret;
} sc_i18n_arg;

typedef struct sc_i18n_scan_result {
    size_t struct_size;
    bool ok;
    size_t bare_string_count;
} sc_i18n_scan_result;

void sc_i18n_catalog_init(sc_i18n_catalog *catalog, sc_allocator *alloc, sc_str locale);
sc_status sc_i18n_catalog_add(sc_i18n_catalog *catalog, sc_str key, sc_str message);
sc_status sc_i18n_catalog_load_ftl(sc_i18n_catalog *catalog, sc_str text);
sc_status sc_i18n_catalog_load_default_en(sc_i18n_catalog *catalog);
bool sc_i18n_catalog_has(const sc_i18n_catalog *catalog, sc_str key);
sc_status sc_i18n_format(const sc_i18n_catalog *catalog,
                         sc_str key,
                         const sc_i18n_arg *args,
                         size_t arg_count,
                         sc_allocator *alloc,
                         sc_string *out);
sc_status sc_i18n_coverage_report(const sc_i18n_catalog *catalog,
                                  const sc_str *required_keys,
                                  size_t required_key_count,
                                  sc_allocator *alloc,
                                  sc_string *out);
sc_status sc_i18n_scan_c_source(sc_str source, sc_i18n_scan_result *out);
void sc_i18n_catalog_clear(sc_i18n_catalog *catalog);

SC_END_DECLS
