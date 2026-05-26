#pragma once

#include "security/security_internal.h"

bool sc_sandbox_executable_available(const char *path);
sc_status sc_sandbox_decision_begin(sc_sandbox_decision *decision,
                                    sc_allocator *alloc,
                                    sc_sandbox_backend backend,
                                    const char *reason);
sc_status sc_sandbox_decision_add_arg(sc_sandbox_decision *decision, sc_allocator *alloc, sc_str value);
sc_status sc_sandbox_decision_add_cstr(sc_sandbox_decision *decision, sc_allocator *alloc, const char *value);
sc_status sc_sandbox_decision_add_joined(sc_sandbox_decision *decision,
                                         sc_allocator *alloc,
                                         const char *prefix,
                                         sc_str value);
sc_status sc_sandbox_decision_add_i64_suffix(sc_sandbox_decision *decision,
                                             sc_allocator *alloc,
                                             int64_t value,
                                             const char *suffix);
sc_status sc_sandbox_decision_add_original_command(sc_sandbox_decision *decision,
                                                   sc_allocator *alloc,
                                                   const sc_sandbox_request *request);
