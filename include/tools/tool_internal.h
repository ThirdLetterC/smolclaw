#pragma once

#include "sc/tool.h"

typedef struct sc_tool_impl_context {
    sc_allocator *alloc;
    sc_tool_context context;
    sc_json_value *schema;
    sc_json_value *output_schema;
} sc_tool_impl_context;

sc_status sc_tool_schema_string_required(sc_allocator *alloc,
                                         sc_str name,
                                         sc_str description,
                                         sc_json_value **out);
sc_status sc_tool_schema_string(sc_allocator *alloc,
                                sc_str name,
                                bool required,
                                sc_json_value **out);
sc_status sc_tool_output_schema_text(sc_allocator *alloc, sc_json_value **out);
sc_status sc_tool_schema_two_strings(sc_allocator *alloc,
                                     sc_str first,
                                     bool first_required,
                                     sc_str second,
                                     bool second_required,
                                     sc_json_value **out);
sc_status sc_tool_schema_three_strings(sc_allocator *alloc,
                                       sc_str first,
                                       bool first_required,
                                       sc_str second,
                                       bool second_required,
                                       sc_str third,
                                       bool third_required,
                                       sc_json_value **out);
sc_status sc_tool_schema_four_strings(sc_allocator *alloc,
                                      sc_str first,
                                      bool first_required,
                                      sc_str second,
                                      bool second_required,
                                      sc_str third,
                                      bool third_required,
                                      sc_str fourth,
                                      bool fourth_required,
                                      sc_json_value **out);
sc_status sc_tool_context_copy(sc_allocator *alloc,
                               const sc_tool_context *context,
                               sc_tool_impl_context *out);
void sc_tool_impl_context_clear(sc_tool_impl_context *context);
sc_status sc_tool_get_string_arg(const sc_tool_call *call, sc_str name, sc_str *out);
sc_status sc_tool_get_optional_string_arg(const sc_tool_call *call, sc_str name, sc_str *out);
sc_status sc_tool_check_cancelled(const sc_tool_impl_context *context, const sc_tool_call *call);
sc_status sc_tool_security_check(const sc_tool_impl_context *context,
                                 sc_str tool_name,
                                 sc_tool_risk risk,
                                 sc_str path,
                                 bool path_must_exist,
                                 sc_str url,
                                 sc_str shell);
sc_status sc_tool_security_check_ex(const sc_tool_impl_context *context,
                                    sc_str tool_name,
                                    sc_tool_risk risk,
                                    sc_str path,
                                    bool path_must_exist,
                                    sc_str url,
                                    sc_str shell,
                                    sc_str device,
                                    sc_str otp);
void sc_tool_approval_override_set(sc_str tool_name);
void sc_tool_approval_override_clear();
void sc_tool_log_failure(sc_str tool_name, sc_status status);
sc_status sc_tool_set_output(sc_allocator *alloc,
                             const sc_tool_impl_context *context,
                             sc_tool_result *out,
                             sc_str text,
                             bool success);
sc_status sc_tool_record_receipt(const sc_tool_impl_context *context,
                                 sc_str tool_name,
                                 sc_str args_summary,
                                 sc_str output_summary,
                                 bool success);
sc_status sc_tool_record_receipt_status(const sc_tool_impl_context *context,
                                        sc_str tool_name,
                                        sc_str args_summary,
                                        sc_str output_summary,
                                        bool success,
                                        sc_status tool_status);
