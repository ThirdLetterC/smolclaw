#include <stdio.h>

#include "toml/toml.h"

static void error(const char *msg, const char *msg1) {
  fprintf(stderr, "ERROR: %s%s\n", msg, msg1 ? msg1 : "");
}

[[nodiscard]] static FILE *setup() {
  const char *text =
      "[default]\n"
      "\n"
      "[wayland_displays.\"$WAYLAND_DISPLAY\"]\n"
      "seats = [ \"$XDG_SEAT\" ] \n"
      "[[clipboards.Default.mime_type_groups]]\n"
      "group = [ \"TEXT\", \"STRING\", \"UTF8_STRING\", \"text/plain\" ]\n"
      "xxxx xx xx\n";
  FILE *fp = tmpfile();
  if (!fp) {
    error("tmpfile failed", nullptr);
    return nullptr;
  }
  if (fputs(text, fp) == EOF) {
    error("failed to write fixture", nullptr);
    fclose(fp);
    return nullptr;
  }
  if (fflush(fp) != 0) {
    error("failed to flush fixture", nullptr);
    fclose(fp);
    return nullptr;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    error("failed to rewind fixture", nullptr);
    fclose(fp);
    return nullptr;
  }
  return fp;
}

[[nodiscard]] static bool run(FILE *fp) {
  auto root = toml_parse_file(fp);

  if (!root.ok) {
    fprintf(stderr, "toml_parse_file: %s\n", root.errmsg);
    toml_free(root);
    return false;
  }

  auto wayland_displays = toml_seek(root.toptab, "main.wayland_displays");
  auto clipboards = toml_seek(root.toptab, "main.clipboards");

  printf("wayland_displays: %d\n", wayland_displays.type);
  printf("clipboards: %d\n", clipboards.type);

  toml_free(root);
  return true;
}

int main() {
  constexpr int kExitOk = 0;
  constexpr int kExitFail = 1;
  int rc = kExitFail;

  FILE *fp = setup();
  if (!fp) {
    return rc;
  }
  if (!run(fp)) {
    goto cleanup;
  }
  rc = kExitOk;

cleanup:
  fclose(fp);
  return rc;
}
