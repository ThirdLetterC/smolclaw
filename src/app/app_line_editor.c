#include "app/app_line_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAVE_ISOCLINE
#include <isocline.h>
#endif

static void strip_trailing_newline(char *line)
{
    size_t len = 0;

    if (line == nullptr) {
        return;
    }
    len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        len -= 1;
    }
}

void sc_app_line_editor_init(sc_str history_path)
{
#ifdef SC_HAVE_ISOCLINE
    if (history_path.ptr != nullptr && history_path.len > 0) {
        char path[4096] = {0};
        if (history_path.len < sizeof(path)) {
            memcpy(path, history_path.ptr, history_path.len);
            path[history_path.len] = '\0';
            ic_set_history(path, 200);
        }
    } else {
        ic_set_history(nullptr, 200);
    }
    ic_set_prompt_marker("", "  ");
    (void)ic_enable_multiline(true);
#else
    (void)history_path;
#endif
}

sc_status sc_app_line_editor_read(sc_allocator *alloc, const char *prompt, sc_string *out, bool *eof)
{
    if (out == nullptr || eof == nullptr) {
        return sc_status_invalid_argument("sc.cli.line_editor.invalid_argument");
    }
    *eof = false;
    *out = (sc_string){0};
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

#ifdef SC_HAVE_ISOCLINE
    sc_status status;
    char *line = ic_readline(prompt == nullptr ? "" : prompt);
    if (line == nullptr) {
        *eof = true;
        return sc_status_ok();
    }
    strip_trailing_newline(line);
    status = sc_string_from_cstr(alloc, line, out);
    free(line);
    return status;
#else
    char line[65'536] = {0};
    if (prompt != nullptr && fputs(prompt, stdout) < 0) {
        return sc_status_io("sc.cli.line_editor.prompt_write_failed");
    }
    (void)fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == nullptr) {
        *eof = true;
        return sc_status_ok();
    }
    strip_trailing_newline(line);
    return sc_string_from_cstr(alloc, line, out);
#endif
}

sc_status sc_app_line_editor_read_plain(sc_allocator *alloc, const char *prompt, sc_string *out, bool *eof)
{
    char line[4'096] = {0};

    if (out == nullptr || eof == nullptr) {
        return sc_status_invalid_argument("sc.cli.line_editor.invalid_argument");
    }
    *eof = false;
    *out = (sc_string){0};
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (prompt != nullptr && fputs(prompt, stdout) < 0) {
        return sc_status_io("sc.cli.line_editor.prompt_write_failed");
    }
    (void)fflush(stdout);
    if (fgets(line, sizeof(line), stdin) == nullptr) {
        *eof = true;
        return sc_status_ok();
    }
    strip_trailing_newline(line);
    return sc_string_from_cstr(alloc, line, out);
}
