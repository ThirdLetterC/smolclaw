#include "sc/allocator.h"
#include "sc/buffer.h"
#include "sc/map.h"
#include "sc/result.h"
#include "sc/string.h"
#include "sc/time.h"
#include "sc/vector.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int expect_size(const char *label, size_t actual, size_t expected);
static int test_status_owned_message(void);
static int test_allocator_failure(void);
static int test_arena(void);
static int test_arena_allocator(void);
static int test_string_and_builder(void);
static int test_bytes_overflow(void);
static int test_vector(void);
static int test_map_collisions(void);
static int test_time_and_uuid(void);
static int test_secure_zero(void);

int main(void)
{
    int failures = 0;

    failures += test_status_owned_message();
    failures += test_allocator_failure();
    failures += test_arena();
    failures += test_arena_allocator();
    failures += test_string_and_builder();
    failures += test_bytes_overflow();
    failures += test_vector();
    failures += test_map_collisions();
    failures += test_time_and_uuid();
    failures += test_secure_zero();

    return failures == 0 ? 0 : 1;
}

static int expect_size(const char *label, size_t actual, size_t expected)
{
    if (actual != expected) {
        (void)fprintf(stderr, "%s: expected %zu, got %zu\n", label, expected, actual);
        return 1;
    }

    return 0;
}

static int test_status_owned_message(void)
{
    int failures = 0;
    sc_test_allocator test_alloc = {0};
    sc_status status = {0};

    sc_test_allocator_init(&test_alloc);
    status = sc_status_make_owned(&test_alloc.base,
                                  SC_ERR_PARSE,
                                  "sc.test.parse",
                                  "fixture parse failed");
    failures += sc_test_expect_status("owned status code", status, SC_ERR_PARSE);
    failures += expect_size("owned status freed", test_alloc.deallocations, 1);

    sc_test_allocator_fail_after(&test_alloc, 0);
    status = sc_status_make_owned(&test_alloc.base,
                                  SC_ERR_PARSE,
                                  "sc.test.parse",
                                  "fixture parse failed");
    failures += sc_test_expect_status("owned status allocation failure", status, SC_ERR_NO_MEMORY);

    return failures;
}

static int test_allocator_failure(void)
{
    int failures = 0;
    sc_test_allocator test_alloc = {0};
    sc_string string = {0};
    sc_status status = {0};

    sc_test_allocator_init(&test_alloc);
    sc_test_allocator_fail_after(&test_alloc, 0);

    status = sc_string_from_cstr(&test_alloc.base, "hello", &string);
    failures += sc_test_expect_status("string allocation failure", status, SC_ERR_NO_MEMORY);
    failures += sc_test_expect_true("failed string left untouched", string.ptr == nullptr);
    failures += expect_size("no failed free", test_alloc.deallocations, 0);

    return failures;
}

static int test_arena(void)
{
    int failures = 0;
    sc_arena arena = {0};
    const char *first = nullptr;
    const char *second = nullptr;

    sc_arena_init(&arena, sc_allocator_heap(), 32);
    first = sc_arena_alloc(&arena, 8, _Alignof(char));
    second = sc_arena_alloc(&arena, 8, _Alignof(char));

    failures += sc_test_expect_true("arena first allocation", first != nullptr);
    failures += sc_test_expect_true("arena second allocation", second != nullptr);
    failures += sc_test_expect_true("arena distinct allocations", first != second);

    sc_arena_reset(&arena);
    first = sc_arena_alloc(&arena, 8, _Alignof(char));
    failures += sc_test_expect_true("arena allocation after reset", first != nullptr);
    sc_arena_clear(&arena);

    return failures;
}

static int test_arena_allocator(void)
{
    int failures = 0;
    sc_arena arena = {0};
    sc_allocator *alloc = nullptr;
    sc_string string = {0};
    void *first = nullptr;
    const void *second = nullptr;

    sc_arena_init(&arena, sc_allocator_heap(), 32);
    alloc = sc_arena_allocator(&arena);
    failures += sc_test_expect_true("arena allocator handle", alloc != nullptr);
    failures += sc_test_expect_status("arena string", sc_string_from_cstr(alloc, "scratch", &string), SC_OK);
    failures += sc_test_expect_true("arena string content", strcmp(string.ptr, "scratch") == 0);
    first = string.ptr;
    second = sc_realloc(alloc, first, string.len + 1, 64, _Alignof(char));
    failures += sc_test_expect_true("arena resize moved", second != nullptr);
    failures += sc_test_expect_true("arena resize copied", second != nullptr && memcmp(second, "scratch", 7) == 0);
    sc_string_clear(&string);
    sc_arena_clear(&arena);
    return failures;
}

static int test_string_and_builder(void)
{
    int failures = 0;
    sc_string string = {0};
    sc_string_builder builder = {0};
    sc_status status = {0};

    status = sc_string_from_str(sc_allocator_heap(), sc_str_from_parts("ab\0cd", 5), &string);
    failures += sc_test_expect_status("string from borrowed", status, SC_OK);
    failures += expect_size("string length preserves embedded nul", string.len, 5);
    failures += sc_test_expect_true("string terminates", string.ptr[string.len] == '\0');
    sc_string_clear(&string);

    failures += sc_test_expect_true("valid utf8", sc_str_is_valid_utf8(sc_str_from_cstr("hello")));
    failures += sc_test_expect_true("invalid utf8", !sc_str_is_valid_utf8(sc_str_from_parts("\xC0\x80", 2)));
    status = sc_string_redacted(sc_allocator_heap(), &string);
    failures += sc_test_expect_status("redacted string", status, SC_OK);
    failures += sc_test_expect_true("redacted content", strcmp(string.ptr, "[REDACTED]") == 0);
    sc_string_clear(&string);

    sc_string_builder_init(&builder, sc_allocator_heap());
    status = sc_string_builder_append_cstr(&builder, "zero");
    failures += sc_test_expect_status("builder append first", status, SC_OK);
    status = sc_string_builder_append(&builder, sc_str_from_parts("claw", 4));
    failures += sc_test_expect_status("builder append second", status, SC_OK);
    status = sc_string_builder_finish(&builder, &string);
    failures += sc_test_expect_status("builder finish", status, SC_OK);
    failures += sc_test_expect_true("builder content", strcmp(string.ptr, "zeroclaw") == 0);
    sc_string_clear(&string);
    sc_string_builder_clear(&builder);

    return failures;
}

static int test_bytes_overflow(void)
{
    sc_bytes bytes = {
        .ptr = nullptr,
        .len = SIZE_MAX - 1,
        .cap = SIZE_MAX - 1,
        .alloc = sc_allocator_heap(),
    };

    return sc_test_expect_status("bytes overflow", sc_bytes_reserve(&bytes, 2), SC_ERR_NO_MEMORY);
}

static int test_vector(void)
{
    int failures = 0;
    sc_vec vec = {0};
    int first = 10;
    int second = 20;
    const int *read_back = nullptr;

    sc_vec_init(&vec, sc_allocator_heap(), sizeof(int));
    failures += sc_test_expect_status("vec push first", sc_vec_push(&vec, &first), SC_OK);
    failures += sc_test_expect_status("vec push second", sc_vec_push(&vec, &second), SC_OK);
    failures += expect_size("vec len", vec.len, 2);

    read_back = sc_vec_at(&vec, 1);
    failures += sc_test_expect_true("vec read", read_back != nullptr && *read_back == 20);
    sc_vec_clear(&vec);

    return failures;
}

static int test_map_collisions(void)
{
    int failures = 0;
    sc_map map = {0};
    int values[20] = {0};
    size_t i = 0;
    char key[16] = {0};

    sc_map_init(&map, sc_allocator_heap());
    for (i = 0; i < SC_ARRAY_LEN(values); i += 1) {
        int written = snprintf(key, sizeof(key), "key-%02zu", i);
        values[i] = (int)i;
        failures += sc_test_expect_true("map key format", written > 0);
        failures += sc_test_expect_status(
            "map put",
            sc_map_put(&map, sc_str_from_cstr(key), &values[i]),
            SC_OK
        );
    }

    for (i = 0; i < SC_ARRAY_LEN(values); i += 1) {
        const int *read_back = nullptr;
        (void)snprintf(key, sizeof(key), "key-%02zu", i);
        read_back = sc_map_get(&map, sc_str_from_cstr(key));
        failures += sc_test_expect_true("map get", read_back != nullptr && *read_back == (int)i);
    }

    failures += expect_size("map len", map.len, SC_ARRAY_LEN(values));
    sc_map_clear(&map);
    return failures;
}

static int test_time_and_uuid(void)
{
    int failures = 0;
    sc_instant instant = {0};
    sc_wall_time epoch = {.unix_ns = 0};
    sc_string formatted = {0};
    sc_string uuid = {0};

    failures += sc_test_expect_status("monotonic", sc_clock_monotonic(&instant), SC_OK);
    failures += sc_test_expect_true("monotonic positive", instant.ns > 0);
    failures += sc_test_expect_status("rfc3339", sc_time_format_rfc3339(sc_allocator_heap(), epoch, &formatted), SC_OK);
    failures += sc_test_expect_true("rfc3339 epoch", strcmp(formatted.ptr, "1970-01-01T00:00:00Z") == 0);
    sc_string_clear(&formatted);

    failures += sc_test_expect_status("uuid", sc_uuid_v4(sc_allocator_heap(), &uuid), SC_OK);
    failures += expect_size("uuid length", uuid.len, 36);
    failures += sc_test_expect_true("uuid version", uuid.ptr[14] == '4');
    failures += sc_test_expect_true("uuid hyphen", uuid.ptr[8] == '-' && uuid.ptr[13] == '-');
    sc_string_clear(&uuid);

    return failures;
}

static int test_secure_zero(void)
{
    int failures = 0;
    unsigned char secret[4] = {1, 2, 3, 4};

    sc_secure_zero(secret, sizeof(secret));
    failures += sc_test_expect_true("secure zero", secret[0] == 0 && secret[1] == 0 && secret[2] == 0 && secret[3] == 0);

    return failures;
}
