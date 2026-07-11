// cppcheck-suppress-file redundantInitialization
#define _POSIX_C_SOURCE 200809L

#include "security/security_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

static sc_status path_to_cstr(sc_allocator *alloc, sc_str path, char **out);
static sc_status open_store_writer(sc_allocator *alloc, sc_str path, FILE **out);
static sc_status open_store_reader(sc_allocator *alloc, sc_str path, FILE **out);
static sc_status read_line(FILE *file, sc_allocator *alloc, sc_string *out, bool *found);
static sc_status write_escaped(FILE *file, sc_str value);
static sc_status decode_escaped(sc_allocator *alloc, sc_str value, sc_string *out);
static bool split_fields(sc_str line, sc_str *fields, size_t count);
static bool parse_u64_hex(sc_str text, uint64_t *out);
static bool parse_i64(sc_str text, int64_t *out);
static sc_status receipt_key_path(sc_allocator *alloc, sc_str path, sc_string *out);
static sc_status save_receipt_key(sc_allocator *alloc, sc_str path, const unsigned char *key, size_t key_len);
static sc_status load_receipt_key(sc_allocator *alloc, sc_str path, unsigned char *key, size_t key_len);
static int hex_value(char ch);
static sc_status write_audit_record(FILE *file, const sc_audit_record *record);
static sc_status load_audit_record(sc_audit_chain *chain, sc_str line);
static sc_status write_receipt(FILE *file, const sc_tool_receipt *receipt);
static sc_status load_receipt(sc_receipt_chain *chain, sc_str line);

sc_status sc_audit_chain_save_file(const sc_audit_chain *chain, sc_str path)
{
    FILE *file = nullptr;
    sc_status status = sc_status_ok();

    if (chain == nullptr) {
        return sc_status_invalid_argument("sc.audit.store.invalid_argument");
    }
    if (!sc_audit_chain_verify(chain)) {
        return sc_status_security_denied("sc.audit.store.tamper_detected");
    }
    status = open_store_writer(chain->alloc, path, &file);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (fputs("sc-audit-store-v1\n", file) == EOF) {
        status = sc_status_io("sc.audit.store.write_failed");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < chain->records.len; i += 1) {
        const sc_audit_record *record = sc_vec_at_const(&chain->records, i);
        status = write_audit_record(file, record);
    }
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.audit.store.close_failed");
    }
    return status;
}

sc_status sc_audit_chain_load_file(sc_allocator *alloc, sc_str path, sc_audit_chain *out)
{
    FILE *file = nullptr;
    sc_string line = {0};
    bool found = false;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.audit.store.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = open_store_reader(alloc, path, &file);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = read_line(file, alloc, &line, &found);
    if (sc_status_is_ok(status) &&
        (!found || !sc_str_equal(sc_string_as_str(&line), sc_str_from_cstr("sc-audit-store-v1")))) {
        status = sc_status_parse("sc.audit.store.header_invalid");
    }
    sc_string_clear(&line);
    sc_audit_chain_init(out, alloc);
    while (sc_status_is_ok(status)) {
        status = read_line(file, alloc, &line, &found);
        if (!sc_status_is_ok(status) || !found) {
            break;
        }
        status = load_audit_record(out, sc_string_as_str(&line));
        sc_string_clear(&line);
    }
    sc_string_clear(&line);
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.audit.store.close_failed");
    }
    if (sc_status_is_ok(status) && !sc_audit_chain_verify(out)) {
        status = sc_status_security_denied("sc.audit.store.tamper_detected");
    }
    if (!sc_status_is_ok(status)) {
        sc_audit_chain_clear(out);
    }
    return status;
}

sc_status sc_receipt_chain_save_file(const sc_receipt_chain *chain, sc_str path)
{
    FILE *file = nullptr;
    sc_status status = sc_status_ok();

    if (chain == nullptr || !chain->key_initialized) {
        return sc_status_invalid_argument("sc.receipt.store.invalid_argument");
    }
    if (!sc_receipt_chain_verify(chain)) {
        return sc_status_security_denied("sc.receipt.store.tamper_detected");
    }
    status = save_receipt_key(chain->alloc, path, chain->session_key, sizeof(chain->session_key));
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = open_store_writer(chain->alloc, path, &file);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (fputs("sc-receipt-store-v2\n", file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < chain->receipts.len; i += 1) {
        const sc_tool_receipt *receipt = sc_vec_at_const(&chain->receipts, i);
        status = write_receipt(file, receipt);
    }
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.receipt.store.close_failed");
    }
    return status;
}

sc_status sc_receipt_chain_load_file(sc_allocator *alloc, sc_str path, sc_receipt_chain *out)
{
    FILE *file = nullptr;
    sc_string line = {0};
    bool found = false;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.receipt.store.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = open_store_reader(alloc, path, &file);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = read_line(file, alloc, &line, &found);
    if (sc_status_is_ok(status) &&
        (!found || !sc_str_equal(sc_string_as_str(&line), sc_str_from_cstr("sc-receipt-store-v2")))) {
        status = sc_status_parse("sc.receipt.store.header_invalid");
    }
    sc_receipt_chain_init(out, alloc);
    if (sc_status_is_ok(status)) {
        status = load_receipt_key(alloc, path, out->session_key, sizeof(out->session_key));
        if (sc_status_is_ok(status)) {
            out->key_initialized = true;
        }
    }
    sc_string_clear(&line);
    while (sc_status_is_ok(status)) {
        status = read_line(file, alloc, &line, &found);
        if (!sc_status_is_ok(status) || !found) {
            break;
        }
        status = load_receipt(out, sc_string_as_str(&line));
        sc_string_clear(&line);
    }
    sc_string_clear(&line);
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.receipt.store.close_failed");
    }
    if (sc_status_is_ok(status) && !sc_receipt_chain_verify(out)) {
        status = sc_status_security_denied("sc.receipt.store.tamper_detected");
    }
    if (!sc_status_is_ok(status)) {
        sc_receipt_chain_clear(out);
    }
    return status;
}

static sc_status path_to_cstr(sc_allocator *alloc, sc_str path, char **out)
{
    char *copy = nullptr;

    if (out == nullptr || path.ptr == nullptr || path.len == 0 || path.len == SIZE_MAX ||
        memchr(path.ptr, '\0', path.len) != nullptr) {
        return sc_status_invalid_argument("sc.security.store.path_invalid");
    }
    copy = sc_alloc(alloc, path.len + 1u, _Alignof(char));
    if (copy == nullptr) {
        return sc_status_no_memory();
    }
    (void)memcpy(copy, path.ptr, path.len);
    copy[path.len] = '\0';
    *out = copy;
    return sc_status_ok();
}

static sc_status open_store_writer(sc_allocator *alloc, sc_str path, FILE **out)
{
    char *path_cstr = nullptr;
    int fd = -1;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.security.store.invalid_argument");
    }
    status = path_to_cstr(alloc, path, &path_cstr);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    fd = open(path_cstr, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    sc_free(alloc, path_cstr, path.len + 1u, _Alignof(char));
    if (fd < 0) {
        return sc_status_io("sc.security.store.open_failed");
    }
    *out = fdopen(fd, "wb");
    if (*out == nullptr) {
        (void)close(fd);
        return sc_status_io("sc.security.store.fdopen_failed");
    }
    return sc_status_ok();
}

static sc_status open_store_reader(sc_allocator *alloc, sc_str path, FILE **out)
{
    char *path_cstr = nullptr;
    struct stat st = {0};
    int fd = -1;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.security.store.invalid_argument");
    }
    status = path_to_cstr(alloc, path, &path_cstr);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    fd = open(path_cstr, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    sc_free(alloc, path_cstr, path.len + 1u, _Alignof(char));
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (fd >= 0) {
            (void)close(fd);
        }
        return sc_status_security_denied("sc.security.store.unsafe_file");
    }
    *out = fdopen(fd, "rb");
    if (*out == nullptr) {
        (void)close(fd);
        return sc_status_io("sc.security.store.fdopen_failed");
    }
    return sc_status_ok();
}

static sc_status read_line(FILE *file, sc_allocator *alloc, sc_string *out, bool *found)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    int ch = 0;
    bool saw = false;

    if (file == nullptr || out == nullptr || found == nullptr) {
        return sc_status_invalid_argument("sc.security.store.read_invalid_argument");
    }
    *found = false;
    sc_string_builder_init(&builder, alloc);
    while ((ch = fgetc(file)) != EOF) {
        char byte = (char)ch;
        saw = true;
        if (byte == '\n') {
            break;
        }
        status = sc_string_builder_append(&builder, sc_str_from_parts(&byte, 1));
        if (!sc_status_is_ok(status)) {
            break;
        }
    }
    if (ferror(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.security.store.read_failed");
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
        return status;
    }
    if (!saw) {
        sc_string_builder_clear(&builder);
        return sc_status_ok();
    }
    status = sc_string_builder_finish(&builder, out);
    if (sc_status_is_ok(status)) {
        *found = true;
    }
    return status;
}

static sc_status write_escaped(FILE *file, sc_str value)
{
    static const char hex[] = "0123456789ABCDEF";

    if (file == nullptr) {
        return sc_status_invalid_argument("sc.security.store.write_invalid_argument");
    }
    for (size_t i = 0; i < value.len; i += 1) {
        unsigned char ch = (unsigned char)value.ptr[i];
        if (ch == '|' || ch == '%' || ch < 0x20u) {
            if (fputc('%', file) == EOF || fputc(hex[ch >> 4u], file) == EOF || fputc(hex[ch & 0x0fu], file) == EOF) {
                return sc_status_io("sc.security.store.write_failed");
            }
        } else if (fputc(ch, file) == EOF) {
            return sc_status_io("sc.security.store.write_failed");
        }
    }
    return sc_status_ok();
}

static sc_status decode_escaped(sc_allocator *alloc, sc_str value, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.security.store.decode_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char byte = value.ptr[i];
        if (byte == '%') {
            int hi = i + 2u < value.len ? hex_value(value.ptr[i + 1u]) : -1;
            int lo = i + 2u < value.len ? hex_value(value.ptr[i + 2u]) : -1;
            if (hi < 0 || lo < 0) {
                status = sc_status_parse("sc.security.store.escape_invalid");
                break;
            }
            byte = (char)((hi << 4u) | lo);
            i += 2u;
        }
        status = sc_string_builder_append(&builder, sc_str_from_parts(&byte, 1));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool split_fields(sc_str line, sc_str *fields, size_t count)
{
    size_t start = 0;

    if (fields == nullptr || count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; i += 1) {
        size_t end = start;
        while (end < line.len && line.ptr[end] != '|') {
            end += 1;
        }
        if (i + 1u < count && end == line.len) {
            return false;
        }
        fields[i] = sc_str_from_parts(line.ptr + start, end - start);
        start = end + 1u;
    }
    return start > line.len;
}

static bool parse_u64_hex(sc_str text, uint64_t *out)
{
    uint64_t value = 0;

    if (out == nullptr || text.ptr == nullptr || text.len == 0) {
        return false;
    }
    for (size_t i = 0; i < text.len; i += 1) {
        int nibble = hex_value(text.ptr[i]);
        if (nibble < 0) {
            return false;
        }
        value = (value << 4u) | (uint64_t)nibble;
    }
    *out = value;
    return true;
}

static bool parse_i64(sc_str text, int64_t *out)
{
    int64_t sign = 1;
    int64_t value = 0;
    size_t pos = 0;

    if (out == nullptr || text.ptr == nullptr || text.len == 0) {
        return false;
    }
    if (text.ptr[0] == '-') {
        sign = -1;
        pos = 1;
    }
    if (pos == text.len) {
        return false;
    }
    for (; pos < text.len; pos += 1) {
        if (text.ptr[pos] < '0' || text.ptr[pos] > '9') {
            return false;
        }
        value = value * 10 + (int64_t)(text.ptr[pos] - '0');
    }
    *out = value * sign;
    return true;
}

static sc_status receipt_key_path(sc_allocator *alloc, sc_str path, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (out == nullptr || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.receipt.store.key_path_invalid");
    }
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

static sc_status save_receipt_key(sc_allocator *alloc,
                                  sc_str path,
                                  const unsigned char *key,
                                  size_t key_len)
{
    sc_string key_path = {0};
    sc_status status = receipt_key_path(alloc, path, &key_path);
    int fd = -1;
    size_t offset = 0;

    if (!sc_status_is_ok(status)) {
        return status;
    }
    fd = open(key_path.ptr, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        sc_string_clear(&key_path);
        return sc_status_io("sc.receipt.store.key_open_failed");
    }
    while (offset < key_len) {
        ssize_t count = write(fd, key + offset, key_len - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            status = sc_status_io("sc.receipt.store.key_write_failed");
            break;
        }
        offset += (size_t)count;
    }
    if (close(fd) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.receipt.store.key_close_failed");
    }
    sc_string_clear(&key_path);
    return status;
}

static sc_status load_receipt_key(sc_allocator *alloc,
                                  sc_str path,
                                  unsigned char *key,
                                  size_t key_len)
{
    sc_string key_path = {0};
    struct stat st = {0};
    sc_status status = receipt_key_path(alloc, path, &key_path);
    int fd = -1;
    size_t offset = 0;

    if (!sc_status_is_ok(status)) {
        return status;
    }
    fd = open(key_path.ptr, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        (st.st_mode & (S_IRWXG | S_IRWXO)) != 0 || st.st_size != (off_t)key_len) {
        if (fd >= 0) {
            (void)close(fd);
        }
        sc_string_clear(&key_path);
        return sc_status_security_denied("sc.receipt.store.key_invalid");
    }
    while (offset < key_len) {
        ssize_t count = read(fd, key + offset, key_len - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            status = sc_status_io("sc.receipt.store.key_read_failed");
            break;
        }
        offset += (size_t)count;
    }
    if (close(fd) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.receipt.store.key_close_failed");
    }
    sc_string_clear(&key_path);
    return status;
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static sc_status write_audit_record(FILE *file, const sc_audit_record *record)
{
    sc_status status = sc_status_ok();

    if (file == nullptr || record == nullptr) {
        return sc_status_invalid_argument("sc.audit.store.record_invalid");
    }
    if (fprintf(file,
                "%lld|%016llx|%016llx|",
                (long long)record->timestamp.unix_ns,
                (unsigned long long)record->previous_hash,
                (unsigned long long)record->hash) < 0) {
        return sc_status_io("sc.audit.store.write_failed");
    }
    status = write_escaped(file, sc_string_as_str(&record->event_type));
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.audit.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&record->summary));
    }
    if (sc_status_is_ok(status) && fputc('\n', file) == EOF) {
        status = sc_status_io("sc.audit.store.write_failed");
    }
    return status;
}

static sc_status load_audit_record(sc_audit_chain *chain, sc_str line)
{
    sc_str fields[5] = {0};
    sc_audit_record record = {0};
    sc_status status = sc_status_ok();

    if (chain == nullptr || !split_fields(line, fields, SC_ARRAY_LEN(fields))) {
        return sc_status_parse("sc.audit.store.record_invalid");
    }
    if (!parse_i64(fields[0], &record.timestamp.unix_ns) ||
        !parse_u64_hex(fields[1], &record.previous_hash) ||
        !parse_u64_hex(fields[2], &record.hash)) {
        return sc_status_parse("sc.audit.store.record_invalid");
    }
    status = decode_escaped(chain->alloc, fields[3], &record.event_type);
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[4], &record.summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&chain->records, &record);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&record.event_type);
        sc_string_clear(&record.summary);
    }
    return status;
}

static sc_status write_receipt(FILE *file, const sc_tool_receipt *receipt)
{
    sc_status status = sc_status_ok();

    if (file == nullptr || receipt == nullptr) {
        return sc_status_invalid_argument("sc.receipt.store.record_invalid");
    }
    if (fprintf(file,
                "%lld|%lld|%d|%016llx|%016llx|",
                (long long)receipt->started_at.unix_ns,
                (long long)receipt->ended_at.unix_ns,
                receipt->success ? 1 : 0,
                (unsigned long long)receipt->previous_hash,
                (unsigned long long)receipt->hash) < 0) {
        return sc_status_io("sc.receipt.store.write_failed");
    }
    status = write_escaped(file, sc_string_as_str(&receipt->tool_name));
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&receipt->args_summary));
    }
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&receipt->output_summary));
    }
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&receipt->policy_decision));
    }
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&receipt->failure_reason));
    }
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&receipt->outcome));
    }
    if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    if (sc_status_is_ok(status)) {
        status = write_escaped(file, sc_string_as_str(&receipt->token));
    }
    if (sc_status_is_ok(status) && fputc('\n', file) == EOF) {
        status = sc_status_io("sc.receipt.store.write_failed");
    }
    return status;
}

static sc_status load_receipt(sc_receipt_chain *chain, sc_str line)
{
    sc_str fields[12] = {0};
    sc_tool_receipt receipt = {0};
    sc_status status = sc_status_ok();

    if (chain == nullptr || !split_fields(line, fields, SC_ARRAY_LEN(fields))) {
        return sc_status_parse("sc.receipt.store.record_invalid");
    }
    if (!parse_i64(fields[0], &receipt.started_at.unix_ns) ||
        !parse_i64(fields[1], &receipt.ended_at.unix_ns) ||
        fields[2].len != 1 ||
        (fields[2].ptr[0] != '0' && fields[2].ptr[0] != '1') ||
        !parse_u64_hex(fields[3], &receipt.previous_hash) ||
        !parse_u64_hex(fields[4], &receipt.hash)) {
        return sc_status_parse("sc.receipt.store.record_invalid");
    }
    receipt.success = fields[2].ptr[0] == '1';
    status = decode_escaped(chain->alloc, fields[5], &receipt.tool_name);
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[6], &receipt.args_summary);
    }
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[7], &receipt.output_summary);
    }
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[8], &receipt.policy_decision);
    }
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[9], &receipt.failure_reason);
    }
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[10], &receipt.outcome);
    }
    if (sc_status_is_ok(status)) {
        status = decode_escaped(chain->alloc, fields[11], &receipt.token);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&chain->receipts, &receipt);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&receipt.tool_name);
        sc_string_clear(&receipt.args_summary);
        sc_string_clear(&receipt.output_summary);
        sc_string_clear(&receipt.policy_decision);
        sc_string_clear(&receipt.failure_reason);
        sc_string_clear(&receipt.outcome);
        sc_string_clear(&receipt.token);
    }
    return status;
}
