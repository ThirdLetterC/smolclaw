#define _XOPEN_SOURCE 700

#include "sc/memory.h"
#include "test_helpers.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef SC_HAVE_SQLITE
#include <sqlite3.h>
#endif

static int test_none_memory(void);
static int test_markdown_memory(void);
static int test_sqlite_memory(void);
static int test_sqlite_recovery_paths(void);
static int test_memory_hygiene_and_hydrate(void);
static int test_allocation_failure_cleanup(void);
#ifndef SC_HAVE_SQLITE
static void load_sqlite_test_symbol(void *library, const char *name, void *out, size_t out_size);
#endif
static int exercise_crud(sc_memory *memory);

int main(void)
{
    int failures = 0;

    failures += test_none_memory();
    failures += test_markdown_memory();
    failures += test_sqlite_memory();
    failures += test_sqlite_recovery_paths();
    failures += test_memory_hygiene_and_hydrate();
    failures += test_allocation_failure_cleanup();

    return failures == 0 ? 0 : 1;
}

static int test_none_memory(void)
{
    int failures = 0;
    sc_memory *memory = nullptr;
    sc_memory_result result = {0};
    sc_string snapshot = {0};
    sc_memory_record record = {
        .struct_size = sizeof(record),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("k"),
        .value = sc_str_from_cstr("v"),
    };

    failures += sc_test_expect_status("none open", sc_memory_none_new(sc_allocator_heap(), &memory), SC_OK);
    failures += sc_test_expect_status("none put", sc_memory_put(memory, &record), SC_OK);
    failures += sc_test_expect_status("none search", sc_memory_search(memory, nullptr, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("none empty", result.entries.len == 0);
    failures += sc_test_expect_status("none export", sc_memory_export_snapshot(memory, nullptr, sc_allocator_heap(), &snapshot), SC_OK);
    failures += sc_test_expect_true("none snapshot empty", strcmp(snapshot.ptr, "[]") == 0);

    sc_string_clear(&snapshot);
    sc_memory_result_clear(&result);
    sc_memory_destroy(memory);
    return failures;
}

static int test_markdown_memory(void)
{
    int failures = 0;
    sc_memory *memory = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    sc_string text = {0};
    sc_memory_record escaped = {
        .struct_size = sizeof(escaped),
        .namespace_name = sc_str_from_cstr("default"),
        .session_id = sc_str_from_cstr("session-a"),
        .category = sc_str_from_cstr("notes"),
        .key = sc_str_from_cstr("escaped"),
        .value = sc_str_from_cstr("pipe | newline\nbackslash \\"),
        .score = 1.0,
        .importance = 1,
    };

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("memory", &dir), SC_OK);
    failures += sc_test_expect_status("markdown path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("memory.md"), &path), SC_OK);
    failures += sc_test_expect_status("markdown open", sc_memory_markdown_open(sc_allocator_heap(), sc_string_as_str(&path), &memory), SC_OK);
    failures += exercise_crud(memory);
    failures += sc_test_expect_status("markdown escaping put", sc_memory_put(memory, &escaped), SC_OK);
    failures += sc_test_expect_status("markdown read", sc_test_read_file(sc_allocator_heap(), sc_string_as_str(&path), &text), SC_OK);
    failures += sc_test_expect_true("markdown pipe escaped", strstr(text.ptr, "pipe \\| newline\\nbackslash \\\\") != nullptr);

    sc_string_clear(&text);
    sc_memory_destroy(memory);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_sqlite_memory(void)
{
    int failures = 0;
    sc_memory *memory = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    sc_status status = {0};
    sc_memory_result result = {0};
    sc_memory_query query = {
        .struct_size = sizeof(query),
        .namespace_name = sc_str_from_cstr("default"),
        .limit = 10,
    };
    sc_memory_record key_hit = {
        .struct_size = sizeof(key_hit),
        .namespace_name = sc_str_from_cstr("bm25"),
        .key = sc_str_from_cstr("release"),
        .value = sc_str_from_cstr("plain body"),
        .score = 0.0,
    };
    sc_memory_record content_hit = {
        .struct_size = sizeof(content_hit),
        .namespace_name = sc_str_from_cstr("bm25"),
        .key = sc_str_from_cstr("body"),
        .value = sc_str_from_cstr("release"),
        .score = 1000.0,
    };
    sc_memory_query bm25_query = {
        .struct_size = sizeof(bm25_query),
        .namespace_name = sc_str_from_cstr("bm25"),
        .query = sc_str_from_cstr("release"),
        .limit = 2,
    };

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("memory", &dir), SC_OK);
    failures += sc_test_expect_status("sqlite path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("memory.sqlite"), &path), SC_OK);
    status = sc_memory_sqlite_open(sc_allocator_heap(), sc_string_as_str(&path), &memory);
    if (status.code == SC_ERR_UNSUPPORTED) {
#ifdef SC_HAVE_SQLITE
        failures += sc_test_expect_status("sqlite linked open", status, SC_OK);
        sc_string_clear(&path);
        sc_string_clear(&dir);
        return failures;
#else
        sc_status_clear(&status);
        sc_string_clear(&path);
        sc_string_clear(&dir);
        return failures;
#endif
    } else {
        failures += sc_test_expect_status("sqlite open", status, SC_OK);
    }
    failures += exercise_crud(memory);
    failures += sc_test_expect_status("sqlite migration survives reopen search",
                              sc_memory_search(memory, &query, sc_allocator_heap(), &result),
                              SC_OK);
    failures += sc_test_expect_true("sqlite deterministic api", result.entries.len == 0);
    sc_memory_result_clear(&result);

    failures += sc_test_expect_status("sqlite bm25 key put", sc_memory_put(memory, &key_hit), SC_OK);
    failures += sc_test_expect_status("sqlite bm25 content put", sc_memory_put(memory, &content_hit), SC_OK);
    failures += sc_test_expect_status("sqlite bm25 search", sc_memory_search(memory, &bm25_query, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("sqlite bm25 key weight",
                            result.entries.len == 2 &&
                                ((sc_memory_entry *)result.entries.ptr)[0].key.ptr != nullptr &&
                                strcmp(((sc_memory_entry *)result.entries.ptr)[0].key.ptr, "release") == 0);

    sc_memory_result_clear(&result);
    sc_memory_destroy(memory);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_sqlite_recovery_paths(void)
{
    int failures = 0;
    sc_memory *memory = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    sc_string corrupt = {0};
    sc_status status = {0};
    void *sqlite = nullptr;
    int (*sqlite_open)(const char *, void **) = nullptr;
    int (*sqlite_close)(void *) = nullptr;
    int (*sqlite_exec)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **) = nullptr;
    void *locked_db = nullptr;
    sc_memory_record record = {
        .struct_size = sizeof(record),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("locked"),
        .value = sc_str_from_cstr("v"),
    };

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("memory", &dir), SC_OK);
    failures += sc_test_expect_status("corrupt path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("corrupt.sqlite"), &corrupt), SC_OK);
    failures += sc_test_expect_true("write corrupt", sc_test_write_file(sc_string_as_str(&corrupt), "not sqlite") == 0);
    status = sc_memory_sqlite_open(sc_allocator_heap(), sc_string_as_str(&corrupt), &memory);
    if (status.code != SC_ERR_UNSUPPORTED) {
        failures += sc_test_expect_status("corrupt rejected", status, SC_ERR_IO);
    } else {
        sc_status_clear(&status);
    }
    sc_memory_destroy(memory);
    memory = nullptr;

    failures += sc_test_expect_status("sqlite path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("locked.sqlite"), &path), SC_OK);
    status = sc_memory_sqlite_open(sc_allocator_heap(), sc_string_as_str(&path), &memory);
    if (status.code == SC_ERR_UNSUPPORTED) {
#ifdef SC_HAVE_SQLITE
        failures += sc_test_expect_status("sqlite linked open", status, SC_OK);
        sc_string_clear(&path);
        sc_string_clear(&corrupt);
        sc_string_clear(&dir);
        return failures;
#else
        sc_status_clear(&status);
        sc_string_clear(&path);
        sc_string_clear(&corrupt);
        sc_string_clear(&dir);
        return failures;
#endif
    } else {
        failures += sc_test_expect_status("sqlite open", status, SC_OK);
    }
#ifdef SC_HAVE_SQLITE
    sqlite_open = (int (*)(const char *, void **))sqlite3_open;
    sqlite_close = (int (*)(void *))sqlite3_close;
    sqlite_exec = (int (*)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **))sqlite3_exec;
#else
    sqlite = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (sqlite == nullptr) {
        sqlite = dlopen("libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (sqlite != nullptr) {
        load_sqlite_test_symbol(sqlite, "sqlite3_open", &sqlite_open, sizeof(sqlite_open));
        load_sqlite_test_symbol(sqlite, "sqlite3_close", &sqlite_close, sizeof(sqlite_close));
        load_sqlite_test_symbol(sqlite, "sqlite3_exec", &sqlite_exec, sizeof(sqlite_exec));
    }
#endif
    if (sqlite_open != nullptr && sqlite_close != nullptr && sqlite_exec != nullptr &&
        sqlite_open(path.ptr, &locked_db) == 0 &&
        sqlite_exec(locked_db, "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr) == 0) {
        failures += sc_test_expect_status("locked put", sc_memory_put(memory, &record), SC_ERR_IO);
        (void)sqlite_exec(locked_db, "COMMIT", nullptr, nullptr, nullptr);
        failures += sc_test_expect_status("put after lock released", sc_memory_put(memory, &record), SC_OK);
    }
    if (locked_db != nullptr && sqlite_close != nullptr) {
        (void)sqlite_close(locked_db);
    }
    if (sqlite != nullptr) {
        (void)dlclose(sqlite);
    }

    sc_memory_destroy(memory);
    sc_string_clear(&path);
    sc_string_clear(&corrupt);
    sc_string_clear(&dir);
    return failures;
}

static int test_memory_hygiene_and_hydrate(void)
{
    int failures = 0;
    sc_memory *memory = nullptr;
    sc_memory *hydrated = nullptr;
    sc_string dir = {0};
    sc_string path = {0};
    sc_string hydrated_path = {0};
    sc_string duplicate_key = {0};
    sc_string snapshot = {0};
    sc_string value = {0};
    sc_memory_export_options raw_export = {
        .struct_size = sizeof(raw_export),
        .include_raw_sensitive_content = true,
        .include_redacted = true,
        .include_deleted = true,
    };
    sc_memory_record low = {
        .struct_size = sizeof(low),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("low"),
        .value = sc_str_from_cstr("same"),
        .score = 1.0,
    };
    sc_memory_record high = {
        .struct_size = sizeof(high),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("high"),
        .value = sc_str_from_cstr("keep"),
        .score = 10.0,
    };
    sc_memory_record mid = {
        .struct_size = sizeof(mid),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("mid"),
        .value = sc_str_from_cstr("same"),
        .score = 5.0,
    };

    failures += sc_test_expect_status("temp dir", sc_test_make_temp_dir("memory", &dir), SC_OK);
    failures += sc_test_expect_status("memory path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("memory.md"), &path), SC_OK);
    failures += sc_test_expect_status("hydrate path", sc_test_path_join(sc_allocator_heap(), sc_string_as_str(&dir), sc_str_from_cstr("hydrated.md"), &hydrated_path), SC_OK);
    failures += sc_test_expect_status("markdown open", sc_memory_markdown_open(sc_allocator_heap(), sc_string_as_str(&path), &memory), SC_OK);
    failures += sc_test_expect_status("put low", sc_memory_put(memory, &low), SC_OK);
    failures += sc_test_expect_status("put high", sc_memory_put(memory, &high), SC_OK);
    failures += sc_test_expect_status("put mid", sc_memory_put(memory, &mid), SC_OK);
    failures += sc_test_expect_status("duplicate", sc_memory_find_duplicate(memory, sc_str_from_cstr("default"), sc_str_from_cstr("same"), sc_allocator_heap(), &duplicate_key), SC_OK);
    failures += sc_test_expect_true("duplicate key returned", strcmp(duplicate_key.ptr, "mid") == 0 || strcmp(duplicate_key.ptr, "low") == 0);
    failures += sc_test_expect_status("retention", sc_memory_apply_retention(memory, sc_str_from_cstr("default"), 2), SC_OK);
    failures += sc_test_expect_status("low pruned", sc_memory_get(memory, sc_str_from_cstr("default"), sc_str_from_cstr("low"), sc_allocator_heap(), &value), SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("snapshot", sc_memory_export_snapshot_ex(memory, nullptr, &raw_export, sc_allocator_heap(), &snapshot), SC_OK);
    failures += sc_test_expect_status("hydrated open", sc_memory_markdown_open(sc_allocator_heap(), sc_string_as_str(&hydrated_path), &hydrated), SC_OK);
    failures += sc_test_expect_status("hydrate snapshot", sc_memory_hydrate_snapshot(hydrated, sc_string_as_str(&snapshot)), SC_OK);
    failures += sc_test_expect_status("hydrated high", sc_memory_get(hydrated, sc_str_from_cstr("default"), sc_str_from_cstr("high"), sc_allocator_heap(), &value), SC_OK);
    failures += sc_test_expect_true("hydrated high value", strcmp(value.ptr, "keep") == 0);

    sc_string_clear(&value);
    sc_string_clear(&snapshot);
    sc_string_clear(&duplicate_key);
    sc_memory_destroy(hydrated);
    sc_memory_destroy(memory);
    sc_string_clear(&hydrated_path);
    sc_string_clear(&path);
    sc_string_clear(&dir);
    return failures;
}

static int test_allocation_failure_cleanup(void)
{
    int failures = 0;
    sc_test_allocator test_alloc = {0};
    sc_memory_result result = {0};
    sc_memory_entry entry = {
        .struct_size = sizeof(entry),
        .score = 1.0,
        .importance = 1,
    };
    sc_status status;

    sc_test_allocator_init(&test_alloc);
    failures += sc_test_expect_status("entry id", sc_string_from_cstr(sc_allocator_heap(), "id", &entry.id), SC_OK);
    failures += sc_test_expect_status("entry namespace", sc_string_from_cstr(sc_allocator_heap(), "default", &entry.namespace_name), SC_OK);
    failures += sc_test_expect_status("entry key", sc_string_from_cstr(sc_allocator_heap(), "k", &entry.key), SC_OK);
    failures += sc_test_expect_status("entry content", sc_string_from_cstr(sc_allocator_heap(), "v", &entry.content), SC_OK);

    sc_memory_result_init(&result, &test_alloc.base);
    sc_test_allocator_fail_after(&test_alloc, 1);
    status = sc_memory_result_push(&result, &entry);
    failures += sc_test_expect_status("result push failure", status, SC_ERR_NO_MEMORY);
    failures += sc_test_expect_true("result empty after failure", result.entries.len == 0);

    sc_memory_result_clear(&result);
    sc_memory_entry_clear(&entry);
    return failures;
}

static int exercise_crud(sc_memory *memory)
{
    int failures = 0;
    sc_string value = {0};
    sc_string snapshot = {0};
    sc_memory_result result = {0};
    sc_memory_record first = {
        .struct_size = sizeof(first),
        .id = sc_str_from_cstr("id-1"),
        .namespace_name = sc_str_from_cstr("default"),
        .session_id = sc_str_from_cstr("session-a"),
        .category = sc_str_from_cstr("notes"),
        .key = sc_str_from_cstr("alpha"),
        .value = sc_str_from_cstr("Alpha content"),
        .score = 1.0,
        .importance = 2,
    };
    sc_memory_record second = {
        .struct_size = sizeof(second),
        .id = sc_str_from_cstr("id-2"),
        .namespace_name = sc_str_from_cstr("default"),
        .session_id = sc_str_from_cstr("session-b"),
        .category = sc_str_from_cstr("notes"),
        .key = sc_str_from_cstr("beta"),
        .value = sc_str_from_cstr("Beta content"),
        .score = 9.0,
        .importance = 1,
    };
    sc_memory_record other_namespace = {
        .struct_size = sizeof(other_namespace),
        .id = sc_str_from_cstr("id-3"),
        .namespace_name = sc_str_from_cstr("other"),
        .session_id = sc_str_from_cstr("session-a"),
        .category = sc_str_from_cstr("notes"),
        .key = sc_str_from_cstr("gamma"),
        .value = sc_str_from_cstr("Gamma content"),
        .score = 5.0,
        .importance = 1,
    };
    sc_memory_record reserved = {
        .struct_size = sizeof(reserved),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("__smolclaw_autosave"),
        .value = sc_str_from_cstr("must not recurse"),
    };
    sc_memory_query notes_query = {
        .struct_size = sizeof(notes_query),
        .namespace_name = sc_str_from_cstr("default"),
        .category = sc_str_from_cstr("notes"),
        .query = sc_str_from_cstr("content"),
        .limit = 10,
    };
    sc_memory_query session_query = {
        .struct_size = sizeof(session_query),
        .namespace_name = sc_str_from_cstr("default"),
        .session_id = sc_str_from_cstr("session-a"),
        .limit = 10,
    };

    failures += sc_test_expect_status("put first", sc_memory_put(memory, &first), SC_OK);
    failures += sc_test_expect_status("put second", sc_memory_put(memory, &second), SC_OK);
    failures += sc_test_expect_status("put other namespace", sc_memory_put(memory, &other_namespace), SC_OK);
    failures += sc_test_expect_status("put reserved", sc_memory_put(memory, &reserved), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("get alpha",
                              sc_memory_get(memory,
                                            sc_str_from_cstr("default"),
                                            sc_str_from_cstr("alpha"),
                                            sc_allocator_heap(),
                                            &value),
                              SC_OK);
    failures += sc_test_expect_true("get alpha content", strcmp(value.ptr, "Alpha content") == 0);
    sc_string_clear(&value);

    failures += sc_test_expect_status("search notes", sc_memory_search(memory, &notes_query, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("search filtered count", result.entries.len == 2);
    if (result.entries.len == 2) {
        const sc_memory_entry *first_result = sc_vec_at_const(&result.entries, 0);
        failures += sc_test_expect_true("search deterministic sort",
                                first_result != nullptr && strcmp(first_result->key.ptr, "beta") == 0);
    }
    sc_memory_result_clear(&result);

    failures += sc_test_expect_status("reserved not stored",
                              sc_memory_get(memory,
                                            sc_str_from_cstr("default"),
                                            sc_str_from_cstr("__smolclaw_autosave"),
                                            sc_allocator_heap(),
                                            &value),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("export filtered",
                              sc_memory_export_snapshot(memory, &session_query, sc_allocator_heap(), &snapshot),
                              SC_OK);
    failures += sc_test_expect_true("snapshot contains alpha", strstr(snapshot.ptr, "\"alpha\"") != nullptr);
    failures += sc_test_expect_true("snapshot excludes beta", strstr(snapshot.ptr, "\"beta\"") == nullptr);
    failures += sc_test_expect_true("snapshot excludes embeddings", strstr(snapshot.ptr, "embedding") == nullptr);
    sc_string_clear(&snapshot);

    failures += sc_test_expect_status("forget alpha", sc_memory_forget(memory, sc_str_from_cstr("default"), sc_str_from_cstr("alpha")), SC_OK);
    failures += sc_test_expect_status("forgot alpha",
                              sc_memory_get(memory,
                                            sc_str_from_cstr("default"),
                                            sc_str_from_cstr("alpha"),
                                            sc_allocator_heap(),
                                            &value),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("purge session", sc_memory_purge_session(memory, sc_str_from_cstr("default"), sc_str_from_cstr("session-b")), SC_OK);
    failures += sc_test_expect_status("purge namespace", sc_memory_purge_namespace(memory, sc_str_from_cstr("other")), SC_OK);
    failures += sc_test_expect_status("search after purge", sc_memory_search(memory, nullptr, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("all purged", result.entries.len == 0);

    sc_memory_result_clear(&result);
    sc_string_clear(&value);
    return failures;
}

#ifndef SC_HAVE_SQLITE
static void load_sqlite_test_symbol(void *library, const char *name, void *out, size_t out_size)
{
    void *symbol = library == nullptr ? nullptr : dlsym(library, name);
    if (out == nullptr || out_size != sizeof(symbol)) {
        return;
    }
    (void)memcpy(out, &symbol, out_size);
}
#endif

