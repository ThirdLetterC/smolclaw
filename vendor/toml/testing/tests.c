#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toml/toml.h"

static void fail(const char *name, const char *detail) {
  fprintf(stderr, "FAIL: %s: %s\n", name, detail);
}

[[nodiscard]] static bool expect_error_contains(const char *name,
                                                toml_result_t result,
                                                const char *needle) {
  if (result.ok) {
    fail(name, "unexpected success");
    toml_free(result);
    return false;
  }
  if (needle && !strstr(result.errmsg, needle)) {
    fprintf(stderr, "FAIL: %s: unexpected error: %s\n", name, result.errmsg);
    toml_free(result);
    return false;
  }
  toml_free(result);
  return true;
}

[[nodiscard]] static bool expect_string(const char *name, toml_datum_t datum,
                                        const char *value) {
  if (datum.type != TOML_STRING) {
    fail(name, "expected string");
    return false;
  }
  auto len = strlen(value);
  if (datum.u.str.len != (int)len || memcmp(datum.u.str.ptr, value, len) != 0) {
    fail(name, "unexpected string value");
    return false;
  }
  return true;
}

[[nodiscard]] static bool expect_int(const char *name, toml_datum_t datum,
                                     int64_t value) {
  if (datum.type != TOML_INT64) {
    fail(name, "expected int64");
    return false;
  }
  if (datum.u.int64 != value) {
    fail(name, "unexpected integer value");
    return false;
  }
  return true;
}

[[nodiscard]] static bool expect_bool_value(const char *name,
                                            toml_datum_t datum, bool value) {
  if (datum.type != TOML_BOOLEAN) {
    fail(name, "expected boolean");
    return false;
  }
  if (!!datum.u.boolean != !!value) {
    fail(name, "unexpected boolean value");
    return false;
  }
  return true;
}

[[nodiscard]] static bool expect_unknown(const char *name, toml_datum_t datum) {
  if (datum.type != TOML_UNKNOWN) {
    fail(name, "expected TOML_UNKNOWN");
    return false;
  }
  return true;
}

[[nodiscard]] static bool test_parse_and_lookup() {
  static constexpr char kDoc[] =
      "title = \"demo\"\n"
      "enabled = true\n"
      "[server]\n"
      "host = \"127.0.0.1\"\n"
      "port = 8080\n"
      "ports = [80, 443]\n";

  auto result = toml_parse(kDoc, (int)sizeof(kDoc) - 1);
  if (!result.ok) {
    fprintf(stderr, "FAIL: parse and lookup: %s\n", result.errmsg);
    toml_free(result);
    return false;
  }

  bool ok = true;
  ok &= expect_string("top-level string", toml_get(result.toptab, "title"),
                      "demo");
  ok &= expect_bool_value("top-level boolean",
                          toml_get(result.toptab, "enabled"), true);

  auto server = toml_get(result.toptab, "server");
  if (server.type != TOML_TABLE) {
    fail("server table", "expected table");
    ok = false;
  }

  ok &= expect_string("nested host", toml_seek(result.toptab, "server.host"),
                      "127.0.0.1");
  ok &=
      expect_int("nested port", toml_seek(result.toptab, "server.port"), 8080);

  auto ports = toml_seek(result.toptab, "server.ports");
  if (ports.type != TOML_ARRAY) {
    fail("ports array", "expected array");
    ok = false;
  } else if (ports.u.arr.size != 2) {
    fail("ports array", "unexpected array size");
    ok = false;
  } else {
    ok &= expect_int("ports[0]", ports.u.arr.elem[0], 80);
    ok &= expect_int("ports[1]", ports.u.arr.elem[1], 443);
  }

  toml_free(result);
  return ok;
}

[[nodiscard]] static bool test_argument_validation() {
  bool ok = true;

  ok &= expect_error_contains("null src", toml_parse(nullptr, 0),
                              "src must not be null");
  ok &= expect_error_contains("negative len", toml_parse("", -1),
                              "len must not be negative");

  char nonterminated[] = {'k', '=', '1', '\n', 'x'};
  ok &= expect_error_contains("nonterminated src", toml_parse(nonterminated, 4),
                              "NUL terminated");

  ok &= expect_error_contains("null file handle", toml_parse_file(nullptr),
                              "fp must not be null");
  ok &= expect_error_contains("null file path", toml_parse_file_ex(nullptr),
                              "fname must not be null");

  return ok;
}

[[nodiscard]] static bool write_fixture(FILE *fp, const char *text) {
  auto len = strlen(text);
  if (fwrite(text, 1, len, fp) != len) {
    return false;
  }
  if (fflush(fp) != 0) {
    return false;
  }
  return fseek(fp, 0, SEEK_SET) == 0;
}

[[nodiscard]] static bool test_parse_file_roundtrip() {
  static constexpr char kDoc[] =
      "name = \"fixture\"\n"
      "[paths]\n"
      "root = \"/tmp/example\"\n"
      "count = 3\n";

  FILE *fp = tmpfile();
  if (fp == nullptr) {
    fail("parse file roundtrip", "tmpfile failed");
    return false;
  }

  if (!write_fixture(fp, kDoc)) {
    fail("parse file roundtrip", "failed to prepare file fixture");
    fclose(fp);
    return false;
  }

  auto from_file = toml_parse_file(fp);
  auto from_memory = toml_parse(kDoc, (int)sizeof(kDoc) - 1);
  fclose(fp);

  bool ok = true;
  if (!from_file.ok) {
    fprintf(stderr, "FAIL: parse file roundtrip: file parse: %s\n",
            from_file.errmsg);
    ok = false;
  }
  if (!from_memory.ok) {
    fprintf(stderr, "FAIL: parse file roundtrip: memory parse: %s\n",
            from_memory.errmsg);
    ok = false;
  }
  if (ok && !toml_equiv(&from_file, &from_memory)) {
    fail("parse file roundtrip", "file and memory parse differ");
    ok = false;
  }
  if (ok) {
    ok &= expect_string("parse file root",
                        toml_seek(from_file.toptab, "paths.root"),
                        "/tmp/example");
    ok &= expect_int("parse file count",
                     toml_seek(from_file.toptab, "paths.count"), 3);
  }

  toml_free(from_file);
  toml_free(from_memory);
  return ok;
}

[[nodiscard]] static bool test_merge_and_equiv() {
  static constexpr char kBase[] =
      "title = \"base\"\n"
      "[server]\n"
      "port = 80\n"
      "mode = \"http\"\n"
      "[[clients]]\n"
      "name = \"alpha\"\n";

  static constexpr char kOverlay[] =
      "title = \"override\"\n"
      "[server]\n"
      "host = \"127.0.0.1\"\n"
      "port = 443\n"
      "[[clients]]\n"
      "name = \"beta\"\n";

  static constexpr char kExpected[] =
      "title = \"override\"\n"
      "[server]\n"
      "port = 443\n"
      "mode = \"http\"\n"
      "host = \"127.0.0.1\"\n"
      "[[clients]]\n"
      "name = \"alpha\"\n"
      "[[clients]]\n"
      "name = \"beta\"\n";

  static constexpr char kReordered[] =
      "title = \"override\"\n"
      "[server]\n"
      "port = 443\n"
      "mode = \"http\"\n"
      "host = \"127.0.0.1\"\n"
      "[[clients]]\n"
      "name = \"beta\"\n"
      "[[clients]]\n"
      "name = \"alpha\"\n";

  auto base = toml_parse(kBase, (int)sizeof(kBase) - 1);
  auto overlay = toml_parse(kOverlay, (int)sizeof(kOverlay) - 1);
  auto expected = toml_parse(kExpected, (int)sizeof(kExpected) - 1);
  auto reordered = toml_parse(kReordered, (int)sizeof(kReordered) - 1);

  bool ok = true;
  if (!base.ok || !overlay.ok || !expected.ok || !reordered.ok) {
    fail("merge fixture parsing", "failed to parse one of the fixtures");
    ok = false;
  }

  auto merged = toml_merge(&base, &overlay);
  if (!merged.ok) {
    fprintf(stderr, "FAIL: merge and equiv: merge failed: %s\n", merged.errmsg);
    ok = false;
  }

  if (ok && !toml_equiv(&merged, &expected)) {
    fail("merged equivalence", "merged result does not match expected");
    ok = false;
  }
  if (ok && toml_equiv(&merged, &reordered)) {
    fail("equivalence order sensitivity", "unexpected equivalence");
    ok = false;
  }
  if (ok) {
    ok &= expect_string("merged title", toml_get(merged.toptab, "title"),
                        "override");
    ok &= expect_string("merged host", toml_seek(merged.toptab, "server.host"),
                        "127.0.0.1");
    ok &=
        expect_int("merged port", toml_seek(merged.toptab, "server.port"), 443);

    auto clients = toml_get(merged.toptab, "clients");
    if (clients.type != TOML_ARRAY) {
      fail("merged clients", "expected array");
      ok = false;
    } else if (clients.u.arr.size != 2) {
      fail("merged clients", "unexpected array size");
      ok = false;
    } else {
      ok &= expect_string("merged clients[0].name",
                          toml_get(clients.u.arr.elem[0], "name"), "alpha");
      ok &= expect_string("merged clients[1].name",
                          toml_get(clients.u.arr.elem[1], "name"), "beta");
    }
  }

  toml_free(base);
  toml_free(overlay);
  toml_free(expected);
  toml_free(reordered);
  toml_free(merged);
  return ok;
}

[[nodiscard]] static bool test_lookup_validation() {
  static constexpr char kDoc[] = "a = 1\n[t]\nb = 2\n";

  auto result = toml_parse(kDoc, (int)sizeof(kDoc) - 1);
  if (!result.ok) {
    fprintf(stderr, "FAIL: lookup validation: %s\n", result.errmsg);
    toml_free(result);
    return false;
  }

  bool ok = true;
  char nonterminated_key[2] = {'a', 'x'};
  char long_key[127];
  memset(long_key, 'a', sizeof(long_key));

  ok &= expect_int("lookup alias", toml_table_find(result.toptab, "a"), 1);
  ok &= expect_unknown("null lookup key", toml_get(result.toptab, nullptr));
  ok &= expect_unknown("nonterminated lookup key",
                       toml_get(result.toptab, nonterminated_key));
  ok &= expect_unknown("null multipart key", toml_seek(result.toptab, nullptr));
  ok &= expect_unknown("oversized multipart key",
                       toml_seek(result.toptab, long_key));
  ok &= expect_unknown("seek through scalar", toml_seek(result.toptab, "a.b"));
  ok &= expect_unknown("missing nested key", toml_seek(result.toptab, "t.c"));

  toml_free(result);
  return ok;
}

int main() {
  constexpr int kExitOk = 0;
  constexpr int kExitFail = 1;

  bool ok = true;
  ok &= test_parse_and_lookup();
  ok &= test_argument_validation();
  ok &= test_parse_file_roundtrip();
  ok &= test_merge_and_equiv();
  ok &= test_lookup_validation();

  return ok ? kExitOk : kExitFail;
}
