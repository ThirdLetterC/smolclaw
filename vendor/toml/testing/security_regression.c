#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "toml/toml.h"

static void fail(const char *name, const char *detail) {
  fprintf(stderr, "FAIL: %s: %s\n", name, detail);
}

[[nodiscard]] static bool expect_parse_failure(const char *name,
                                               const char *src, int len,
                                               const char *needle) {
  auto result = toml_parse(src, len);
  if (result.ok) {
    fail(name, "unexpected parse success");
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

[[nodiscard]] static bool test_utf8_validation() {
  auto opt = toml_default_option();
  opt.check_utf8 = true;
  toml_set_option(opt);

  bool ok = true;
  static const char kOverlong[] = {'k',        ' ',        '=', ' ',  '"',
                                   (char)0xC0, (char)0x80, '"', '\n', '\0'};
  static const char kSurrogate[] = {'k', ' ',        '=',        ' ',
                                    '"', (char)0xED, (char)0xA0, (char)0x80,
                                    '"', '\n',       '\0'};
  static const char kTooHigh[] = {
      'k',        ' ',        '=',        ' ', '"',  (char)0xF4,
      (char)0x90, (char)0x80, (char)0x80, '"', '\n', '\0'};

  ok &= expect_parse_failure("overlong utf8", kOverlong,
                             (int)sizeof(kOverlong) - 1, "invalid UTF8 char");
  ok &= expect_parse_failure("surrogate utf8", kSurrogate,
                             (int)sizeof(kSurrogate) - 1, "invalid UTF8 char");
  ok &= expect_parse_failure("out of range utf8", kTooHigh,
                             (int)sizeof(kTooHigh) - 1, "invalid UTF8 char");

  toml_set_option(toml_default_option());
  return ok;
}

[[nodiscard]] static bool test_escape_validation() {
  static constexpr char kTooHighEscape[] = "k = \"\\U00110000\"\n";
  static constexpr char kSurrogateEscape[] = "k = \"\\uD800\"\n";

  bool ok = true;
  ok &= expect_parse_failure("out of range escape", kTooHighEscape,
                             (int)sizeof(kTooHighEscape) - 1,
                             "invalid UTF8 char");
  ok &= expect_parse_failure("surrogate escape", kSurrogateEscape,
                             (int)sizeof(kSurrogateEscape) - 1,
                             "invalid UTF8 char");
  return ok;
}

[[nodiscard]] static bool test_lookup_bounds() {
  static constexpr char kDoc[] = "a = 1\n[t]\nb = 2\n";
  auto result = toml_parse(kDoc, (int)sizeof(kDoc) - 1);
  if (!result.ok) {
    fprintf(stderr, "FAIL: parse fixture: %s\n", result.errmsg);
    toml_free(result);
    return false;
  }

  bool ok = true;
  auto value = toml_get(result.toptab, "a");
  if (value.type != TOML_INT64 || value.u.int64 != 1) {
    fail("lookup valid key", "unexpected value");
    ok = false;
  }

  auto nested = toml_seek(result.toptab, "t.b");
  if (nested.type != TOML_INT64 || nested.u.int64 != 2) {
    fail("seek valid key", "unexpected value");
    ok = false;
  }

  char nonterminated_key[2] = {'a', 'x'};
  if (toml_get(result.toptab, nonterminated_key).type != TOML_UNKNOWN) {
    fail("lookup nonterminated key", "expected TOML_UNKNOWN");
    ok = false;
  }

  char long_key[127];
  memset(long_key, 'a', sizeof(long_key));
  if (toml_seek(result.toptab, long_key).type != TOML_UNKNOWN) {
    fail("seek oversized key", "expected TOML_UNKNOWN");
    ok = false;
  }

  toml_free(result);
  return ok;
}

int main() {
  constexpr int kExitOk = 0;
  constexpr int kExitFail = 1;

  bool ok = true;
  ok &= test_utf8_validation();
  ok &= test_escape_validation();
  ok &= test_lookup_bounds();

  return ok ? kExitOk : kExitFail;
}
