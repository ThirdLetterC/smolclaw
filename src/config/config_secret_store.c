// cppcheck-suppress-file redundantInitialization
#define _POSIX_C_SOURCE 200809L

#include "config/config_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef SC_HAVE_LIBSODIUM
#include <sodium.h>
#endif

typedef struct memory_secret_store {
    sc_allocator *alloc;
    sc_map values;
} memory_secret_store;

typedef struct file_secret_store {
    sc_allocator *alloc;
    sc_string path;
    sc_string key_path;
    sc_map values;
#ifdef SC_HAVE_LIBSODIUM
    unsigned char key[crypto_secretbox_KEYBYTES];
#endif
} file_secret_store;

static sc_status memory_secret_put(void *impl, sc_str path, sc_str value);
static sc_status memory_secret_get(void *impl, sc_str path, sc_allocator *alloc, sc_string *out);
static void memory_secret_destroy(void *impl);
static sc_status file_secret_put(void *impl, sc_str path, sc_str value);
static sc_status file_secret_get(void *impl, sc_str path, sc_allocator *alloc, sc_string *out);
static void file_secret_destroy(void *impl);

static const sc_secret_store_vtab memory_secret_vtab = {
    .struct_size = sizeof(sc_secret_store_vtab),
    .put = memory_secret_put,
    .get = memory_secret_get,
    .destroy = memory_secret_destroy,
};

static const sc_secret_store_vtab file_secret_vtab = {
    .struct_size = sizeof(sc_secret_store_vtab),
    .put = file_secret_put,
    .get = file_secret_get,
    .destroy = file_secret_destroy,
};

sc_status sc_secret_store_new(sc_allocator *alloc,
                              const sc_secret_store_vtab *vtab,
                              void *impl,
                              sc_secret_store **out)
{
    sc_secret_store *store = nullptr;
    if (out == nullptr || vtab == nullptr || vtab->put == nullptr || vtab->get == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    store = sc_alloc(alloc, sizeof(*store), _Alignof(sc_secret_store));
    if (store == nullptr) {
        return sc_status_no_memory();
    }
    *store = (sc_secret_store){.alloc = alloc, .vtab = vtab, .impl = impl};
    *out = store;
    return sc_status_ok();
}

sc_status sc_secret_store_memory_new(sc_allocator *alloc, sc_secret_store **out)
{
    memory_secret_store *impl = nullptr;
    sc_status status = sc_status_ok();
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.memory_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(memory_secret_store));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (memory_secret_store){.alloc = alloc};
    sc_map_init(&impl->values, alloc);
    status = sc_secret_store_new(alloc, &memory_secret_vtab, impl, out);
    if (!sc_status_is_ok(status)) {
        memory_secret_destroy(impl);
    }
    return status;
}

static sc_status default_secret_path(sc_allocator *alloc, sc_string *out)
{
    const char *home = getenv("HOME");
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.file_invalid_argument");
    }
    if (home == nullptr || home[0] == '\0') {
        return sc_status_io("sc.secret_store.home_missing");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, home);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/.smolclaw/secrets");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static int secret_mkdir(const char *path)
{
    return mkdir(path, 0700);
}

static sc_status ensure_secret_parent_dirs(sc_str path)
{
    char mutable_path[4096] = {0};
    size_t len = 0;

    if (path.ptr == nullptr || path.len == 0 || path.len >= sizeof(mutable_path)) {
        return sc_status_invalid_argument("sc.secret_store.path_invalid");
    }
    memcpy(mutable_path, path.ptr, path.len);
    mutable_path[path.len] = '\0';
    len = path.len;
    for (size_t i = 1U; i < len; i += 1U) {
        if (mutable_path[i] != '/') {
            continue;
        }
        mutable_path[i] = '\0';
        if (mutable_path[0] != '\0' && secret_mkdir(mutable_path) != 0 && errno != EEXIST) {
            return sc_status_io("sc.secret_store.mkdir_failed");
        }
        mutable_path[i] = '/';
    }
    return sc_status_ok();
}

static sc_status key_path_from_secret_path(sc_allocator *alloc, sc_str path, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, path);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".key");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

#ifdef SC_HAVE_LIBSODIUM
static sc_status read_or_create_secret_key(file_secret_store *store)
{
    FILE *file = nullptr;
    struct stat st = {0};
    int fd = -1;
    sc_status status = sc_status_ok();

    if (sodium_init() < 0) {
        return sc_status_unsupported("sc.secret_store.sodium_init_failed");
    }
    fd = open(store->key_path.ptr, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
            (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            (void)close(fd);
            return sc_status_security_denied("sc.secret_store.key_permissions_invalid");
        }
        file = fdopen(fd, "rb");
        if (file == nullptr) {
            (void)close(fd);
            return sc_status_io("sc.secret_store.key_open_failed");
        }
        size_t read_len = fread(store->key, 1, sizeof(store->key), file);
        (void)fclose(file);
        if (read_len != sizeof(store->key)) {
            return sc_status_io("sc.secret_store.key_read_failed");
        }
        return sc_status_ok();
    }
    if (ensure_secret_parent_dirs(sc_string_as_str(&store->key_path)).code != SC_OK) {
        status = sc_status_io("sc.secret_store.mkdir_failed");
        goto cleanup;
    }
    randombytes_buf(store->key, sizeof(store->key));
    fd = open(store->key_path.ptr, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        status = sc_status_io("sc.secret_store.key_open_failed");
        goto cleanup;
    }
    file = fdopen(fd, "wb");
    if (file == nullptr) {
        (void)close(fd);
        status = sc_status_io("sc.secret_store.key_open_failed");
        goto cleanup;
    }
    if (fwrite(store->key, 1, sizeof(store->key), file) != sizeof(store->key)) {
        status = sc_status_io("sc.secret_store.key_write_failed");
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.secret_store.key_close_failed");
        goto cleanup;
    }
    file = nullptr;
    return status;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sodium_memzero(store->key, sizeof(store->key));
    return status;
}

static sc_status file_store_set_plain(file_secret_store *store, sc_str path, sc_str value)
{
    sc_string *owned = nullptr;
    sc_string *existing = nullptr;
    sc_status status = sc_status_ok();

    existing = sc_map_get(&store->values, path);
    owned = sc_alloc(store->alloc, sizeof(*owned), _Alignof(sc_string));
    if (owned == nullptr) {
        return sc_status_no_memory();
    }
    *owned = (sc_string){0};
    status = sc_string_from_str(store->alloc, value, owned);
    if (sc_status_is_ok(status)) {
        status = sc_map_put(&store->values, path, owned);
    }
    if (sc_status_is_ok(status) && existing != nullptr) {
        sc_string_secure_clear(existing);
        sc_free(store->alloc, existing, sizeof(*existing), _Alignof(sc_string));
    }
    if (!sc_status_is_ok(status)) {
        sc_string_secure_clear(owned);
        sc_free(store->alloc, owned, sizeof(*owned), _Alignof(sc_string));
    }
    return status;
}

static sc_status decrypt_secret_line(file_secret_store *store, sc_str path, sc_str encoded)
{
    unsigned char *packed = nullptr;
    unsigned char *plain = nullptr;
    size_t packed_len = 0;
    size_t plain_len = 0;
    size_t max_packed = encoded.len;
    sc_status status = sc_status_ok();

    if (encoded.ptr == nullptr || encoded.len == 0 || max_packed < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
        return sc_status_parse("sc.secret_store.invalid_record");
    }
    packed = sc_alloc(store->alloc, max_packed, _Alignof(unsigned char));
    if (packed == nullptr) {
        return sc_status_no_memory();
    }
    if (sodium_base642bin(packed,
                          max_packed,
                          encoded.ptr,
                          encoded.len,
                          nullptr,
                          &packed_len,
                          nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0 ||
        packed_len < crypto_secretbox_NONCEBYTES + crypto_secretbox_MACBYTES) {
        sc_free(store->alloc, packed, max_packed, _Alignof(unsigned char));
        return sc_status_parse("sc.secret_store.invalid_record");
    }
    plain_len = packed_len - crypto_secretbox_NONCEBYTES - crypto_secretbox_MACBYTES;
    plain = sc_alloc(store->alloc, plain_len + 1U, _Alignof(unsigned char));
    if (plain == nullptr) {
        sc_free(store->alloc, packed, max_packed, _Alignof(unsigned char));
        return sc_status_no_memory();
    }
    if (crypto_secretbox_open_easy(plain,
                                   packed + crypto_secretbox_NONCEBYTES,
                                   packed_len - crypto_secretbox_NONCEBYTES,
                                   packed,
                                   store->key) != 0) {
        status = sc_status_security_denied("sc.secret_store.decrypt_failed");
    } else {
        plain[plain_len] = '\0';
        status = file_store_set_plain(store, path, sc_str_from_parts((const char *)plain, plain_len));
    }
    sodium_memzero(plain, plain_len + 1U);
    sc_free(store->alloc, plain, plain_len + 1U, _Alignof(unsigned char));
    sodium_memzero(packed, max_packed);
    sc_free(store->alloc, packed, max_packed, _Alignof(unsigned char));
    return status;
}

static sc_status load_secret_file(file_secret_store *store)
{
    FILE *file = nullptr;
    struct stat st = {0};
    int fd = -1;
    char line[65'536] = {0};
    bool first = true;
    sc_status status = sc_status_ok();

    fd = open(store->path.ptr, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 && errno == ENOENT) {
        goto cleanup;
    }
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return sc_status_security_denied("sc.secret_store.file_permissions_invalid");
    }
    file = fdopen(fd, "rb");
    if (file == nullptr) {
        (void)close(fd);
        return sc_status_io("sc.secret_store.open_failed");
    }
    while (fgets(line, sizeof(line), file) != nullptr) {
        char *tab = nullptr;
        size_t len = strlen(line);

        while (len > 0 && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
            line[--len] = '\0';
        }
        if (first) {
            first = false;
            if (strcmp(line, "SCSECRETS1") == 0) {
                continue;
            }
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        tab = strchr(line, '\t');
        if (tab == nullptr || tab == line || tab[1] == '\0') {
            status = sc_status_parse("sc.secret_store.invalid_record");
            goto cleanup;
        }
        *tab = '\0';
        status = decrypt_secret_line(store, sc_str_from_cstr(line), sc_str_from_cstr(tab + 1));
        if (!sc_status_is_ok(status)) {
            goto cleanup;
        }
    }
    if (ferror(file) != 0) {
        status = sc_status_io("sc.secret_store.read_failed");
        goto cleanup;
    }
    (void)fclose(file);
    file = nullptr;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    return status;
}

static sc_status append_encrypted_record(file_secret_store *store, FILE *file, sc_str path, sc_str value)
{
    unsigned char nonce[crypto_secretbox_NONCEBYTES] = {0};
    unsigned char *packed = nullptr;
    unsigned char *cipher = nullptr;
    char *encoded = nullptr;
    size_t cipher_len = value.len + crypto_secretbox_MACBYTES;
    size_t packed_len = sizeof(nonce) + cipher_len;
    size_t encoded_len = sodium_base64_ENCODED_LEN(packed_len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    sc_status status = sc_status_ok();

    packed = sc_alloc(store->alloc, packed_len, _Alignof(unsigned char));
    encoded = sc_alloc(store->alloc, encoded_len, _Alignof(char));
    if (packed == nullptr || encoded == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    randombytes_buf(nonce, sizeof(nonce));
    memcpy(packed, nonce, sizeof(nonce));
    cipher = packed + sizeof(nonce);
    if (crypto_secretbox_easy(cipher, (const unsigned char *)value.ptr, value.len, nonce, store->key) != 0) {
        status = sc_status_io("sc.secret_store.encrypt_failed");
        goto cleanup;
    }
    sodium_bin2base64(encoded, encoded_len, packed, packed_len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    if (fprintf(file, "%.*s\t%s\n", (int)path.len, path.ptr, encoded) < 0) {
        status = sc_status_io("sc.secret_store.write_failed");
    }

cleanup:
    if (encoded != nullptr) {
        sodium_memzero(encoded, encoded_len);
        sc_free(store->alloc, encoded, encoded_len, _Alignof(char));
    }
    if (packed != nullptr) {
        sodium_memzero(packed, packed_len);
        sc_free(store->alloc, packed, packed_len, _Alignof(unsigned char));
    }
    sodium_memzero(nonce, sizeof(nonce));
    return status;
}

static sc_status flush_secret_file(file_secret_store *store)
{
    FILE *file = nullptr;
    sc_string_builder builder = {0};
    sc_string tmp_path = {0};
    int fd = -1;
    char suffix[64] = {0};
    sc_status status = sc_status_ok();

    status = ensure_secret_parent_dirs(sc_string_as_str(&store->path));
    if (!sc_status_is_ok(status)) {
        return status;
    }
    int written = snprintf(suffix, sizeof(suffix), ".tmp.%ld", (long)getpid());
    if (written <= 0 || (size_t)written >= sizeof(suffix)) {
        return sc_status_invalid_argument("sc.secret_store.temp_path_failed");
    }
    sc_string_builder_init(&builder, store->alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(&store->path));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, suffix);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &tmp_path);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    fd = open(tmp_path.ptr, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        status = sc_status_io("sc.secret_store.open_failed");
        goto cleanup;
    }
    file = fdopen(fd, "wb");
    if (file == nullptr) {
        (void)close(fd);
        status = sc_status_io("sc.secret_store.open_failed");
        goto cleanup;
    }
    if (fprintf(file, "SCSECRETS1\n") < 0) {
        status = sc_status_io("sc.secret_store.write_failed");
        goto cleanup;
    }
    for (size_t i = 0; i < store->values.cap; i += 1) {
        sc_map_entry *entry = &store->values.entries[i];
        if (!entry->occupied || entry->value == nullptr) {
            continue;
        }
        status = append_encrypted_record(store, file, sc_string_as_str(&entry->key), sc_string_as_str(entry->value));
        if (!sc_status_is_ok(status)) {
            goto cleanup;
        }
    }
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.secret_store.close_failed");
        goto cleanup;
    }
    file = nullptr;
    if (rename(tmp_path.ptr, store->path.ptr) != 0) {
        status = sc_status_io("sc.secret_store.rename_failed");
        goto cleanup;
    }
    status = sc_status_ok();

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    if (!sc_status_is_ok(status) && tmp_path.ptr != nullptr) {
        (void)unlink(tmp_path.ptr);
    }
    sc_string_clear(&tmp_path);
    return status;
}
#endif

sc_status sc_secret_store_file_new(sc_allocator *alloc, sc_str path, sc_secret_store **out)
{
#ifndef SC_HAVE_LIBSODIUM
    (void)alloc;
    (void)path;
    if (out != nullptr) {
        *out = nullptr;
    }
    return sc_status_unsupported("sc.secret_store.file_unsupported");
#else
    file_secret_store *impl = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.file_invalid_argument");
    }
    *out = nullptr;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(file_secret_store));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (file_secret_store){.alloc = alloc};
    sc_map_init(&impl->values, alloc);
    if (path.ptr == nullptr || path.len == 0) {
        status = default_secret_path(alloc, &impl->path);
    } else {
        status = sc_string_from_str(alloc, path, &impl->path);
    }
    if (sc_status_is_ok(status)) {
        status = key_path_from_secret_path(alloc, sc_string_as_str(&impl->path), &impl->key_path);
    }
    if (sc_status_is_ok(status)) {
        status = read_or_create_secret_key(impl);
    }
    if (sc_status_is_ok(status)) {
        status = load_secret_file(impl);
    }
    if (sc_status_is_ok(status)) {
        status = sc_secret_store_new(alloc, &file_secret_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        file_secret_destroy(impl);
    }
    return status;
#endif
}

sc_status sc_secret_store_put(sc_secret_store *store, sc_str path, sc_str value)
{
    if (store == nullptr || store->vtab == nullptr || store->vtab->put == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.invalid_argument");
    }
    return store->vtab->put(store->impl, path, value);
}

sc_status sc_secret_store_get(sc_secret_store *store, sc_str path, sc_allocator *alloc, sc_string *out)
{
    if (store == nullptr || store->vtab == nullptr || store->vtab->get == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.invalid_argument");
    }
    return store->vtab->get(store->impl, path, alloc, out);
}

void sc_secret_store_destroy(sc_secret_store *store)
{
    if (store == nullptr) {
        return;
    }
    if (store->vtab != nullptr && store->vtab->destroy != nullptr) {
        store->vtab->destroy(store->impl);
    }
    sc_free(store->alloc, store, sizeof(*store), _Alignof(sc_secret_store));
}

static sc_status memory_secret_put(void *impl, sc_str path, sc_str value)
{
    memory_secret_store *store = impl;
    sc_string *owned = nullptr;
    sc_string *existing = nullptr;
    sc_status status = sc_status_ok();
    if (store == nullptr || path.ptr == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.memory_invalid_argument");
    }
    existing = sc_map_get(&store->values, path);
    owned = sc_alloc(store->alloc, sizeof(*owned), _Alignof(sc_string));
    if (owned == nullptr) {
        return sc_status_no_memory();
    }
    *owned = (sc_string){0};
    status = sc_string_from_str(store->alloc, value, owned);
    if (sc_status_is_ok(status)) {
        status = sc_map_put(&store->values, path, owned);
    }
    if (sc_status_is_ok(status) && existing != nullptr) {
        sc_string_secure_clear(existing);
        sc_free(store->alloc, existing, sizeof(*existing), _Alignof(sc_string));
    }
    if (!sc_status_is_ok(status)) {
        sc_string_secure_clear(owned);
        sc_free(store->alloc, owned, sizeof(*owned), _Alignof(sc_string));
    }
    return status;
}

static sc_status memory_secret_get(void *impl, sc_str path, sc_allocator *alloc, sc_string *out)
{
    memory_secret_store *store = impl;
    sc_string *value = nullptr;
    if (store == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.memory_invalid_argument");
    }
    value = sc_map_get(&store->values, path);
    if (value == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.not_found");
    }
    return sc_string_from_str(alloc, sc_string_as_str(value), out);
}

static void memory_secret_destroy(void *impl)
{
    memory_secret_store *store = impl;
    if (store == nullptr) {
        return;
    }
    for (size_t i = 0; i < store->values.cap; i += 1) {
        sc_map_entry *entry = &store->values.entries[i];
        if (entry->occupied && entry->value != nullptr) {
            sc_string_secure_clear(entry->value);
            sc_free(store->alloc, entry->value, sizeof(sc_string), _Alignof(sc_string));
        }
    }
    sc_map_clear(&store->values);
    sc_free(store->alloc, store, sizeof(*store), _Alignof(memory_secret_store));
}

static sc_status file_secret_put(void *impl, sc_str path, sc_str value)
{
#ifndef SC_HAVE_LIBSODIUM
    (void)impl;
    (void)path;
    (void)value;
    return sc_status_unsupported("sc.secret_store.file_unsupported");
#else
    file_secret_store *store = impl;
    sc_status status = sc_status_ok();

    if (store == nullptr || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.secret_store.file_invalid_argument");
    }
    status = file_store_set_plain(store, path, value);
    if (sc_status_is_ok(status)) {
        status = flush_secret_file(store);
    }
    return status;
#endif
}

static sc_status file_secret_get(void *impl, sc_str path, sc_allocator *alloc, sc_string *out)
{
    file_secret_store *store = impl;
    sc_string *value = nullptr;

    if (store == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.file_invalid_argument");
    }
    value = sc_map_get(&store->values, path);
    if (value == nullptr) {
        return sc_status_invalid_argument("sc.secret_store.not_found");
    }
    return sc_string_from_str(alloc, sc_string_as_str(value), out);
}

static void file_secret_destroy(void *impl)
{
    file_secret_store *store = impl;

    if (store == nullptr) {
        return;
    }
    for (size_t i = 0; i < store->values.cap; i += 1) {
        sc_map_entry *entry = &store->values.entries[i];
        if (entry->occupied && entry->value != nullptr) {
            sc_string_secure_clear(entry->value);
            sc_free(store->alloc, entry->value, sizeof(sc_string), _Alignof(sc_string));
        }
    }
    sc_map_clear(&store->values);
#ifdef SC_HAVE_LIBSODIUM
    sodium_memzero(store->key, sizeof(store->key));
#endif
    sc_string_clear(&store->key_path);
    sc_string_clear(&store->path);
    sc_free(store->alloc, store, sizeof(*store), _Alignof(file_secret_store));
}
