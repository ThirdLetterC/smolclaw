#pragma once

#include "sc/result.h"
#include "sc/string.h"

sc_status sc_app_line_editor_read(sc_allocator *alloc, const char *prompt, sc_string *out, bool *eof);
sc_status sc_app_line_editor_read_plain(sc_allocator *alloc, const char *prompt, sc_string *out, bool *eof);
void sc_app_line_editor_init(sc_str history_path);
