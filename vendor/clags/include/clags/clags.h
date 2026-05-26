/*
  clags.h - A simple declarative command line arguments parser for C

  Version: 1.0.0

  MIT License

  Copyright (c) 2026 Constantijn de Meer

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#pragma once

#include <float.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#if defined(__has_include)
#if __has_include(<stdckdint.h>)
#include <stdckdint.h>
#define CLAGS_HAS_STDCKDINT 1
#endif
#endif

#ifndef CLAGS_HAS_STDCKDINT
#define CLAGS_HAS_STDCKDINT 0
#endif

// memory allocation functions; can be overridden
#ifndef CLAGS_FREE
#define CLAGS_FREE free // default free function
#endif                  // CLAGS_FREE

#ifndef CLAGS_CALLOC
#define CLAGS_CALLOC calloc // default calloc function
#endif                      // CLAGS_CALLOC

#ifndef CLAGS_REALLOC
#define CLAGS_REALLOC realloc // default realloc function
#endif                        // CLAGS_REALLOC

// the initial capacity of lists
#ifndef CLAGS_LIST_INIT_CAPACITY
#define CLAGS_LIST_INIT_CAPACITY 8
#endif // CLAGS_LIST_INIT_CAPACITY

// the character column at which ':' appears in `clags_usage` output
// you can adjust this value to control the alignment of argument descriptions
#ifndef CLAGS_USAGE_ALIGNMENT
#define CLAGS_USAGE_ALIGNMENT 36
#endif // CLAGS_USAGE_ALIGNMENT

#define CLAGS__USAGE_PRINTF_ALIGNMENT -(CLAGS_USAGE_ALIGNMENT - 4)
#define CLAGS__USAGE_TEMP_BUFFER_SIZE (CLAGS_USAGE_ALIGNMENT - 3)

// macro for enabling printf-like format checks in `clags_sb_appendf`
#if defined(__GNUC__) || defined(__clang__)
#ifdef __MINGW_PRINTF_FORMAT
#define CLAGS__PRINTF_FORMAT(STRING_INDEX, FIRST_TO_CHECK)                     \
  __attribute__((format(__MINGW_PRINTF_FORMAT, STRING_INDEX, FIRST_TO_CHECK)))
#else
#define CLAGS__PRINTF_FORMAT(STRING_INDEX, FIRST_TO_CHECK)                     \
  __attribute__((format(printf, STRING_INDEX, FIRST_TO_CHECK)))
#endif // __MINGW_PRINTF_FORMAT
#else
#define CLAGS__PRINTF_FORMAT(STRING_INDEX, FIRST_TO_CHECK)
#endif

typedef enum {
  Clags_Info,
  Clags_Warning,
  Clags_Error,
  Clags_ConfigWarning,
  Clags_ConfigError,
  Clags_NoLogs, // disable all logs
} clags_log_level_t;

typedef struct clags_config_t clags_config_t;
typedef bool (*clags_custom_verify_func_t)(
    clags_config_t *config, const char *arg_name, const char *arg,
    void *variable); // the function type for custom verifiers
typedef bool(clags_verify_func_t)(clags_config_t *config, const char *arg_name,
                                  const char *arg, void *variable,
                                  void *verify);
typedef clags_verify_func_t *clags_verify_func_ptr_t;
typedef void (*clags_log_handler_t)(
    clags_log_level_t level, const char *format,
    va_list args); // the function type of custom log handlers
typedef void (*clags_callback_func_t)(
    clags_config_t *config); // the function type of callback functions
typedef uint64_t clags_fsize_t;
typedef uint64_t clags_time_t;

// all available value verifiers
clags_verify_func_t clags__verify_string;
clags_verify_func_t clags__verify_custom;
clags_verify_func_t clags__verify_subcmd;
clags_verify_func_t clags__verify_bool;
clags_verify_func_t clags__verify_int8;
clags_verify_func_t clags__verify_uint8;
clags_verify_func_t clags__verify_int32;
clags_verify_func_t clags__verify_uint32;
clags_verify_func_t clags__verify_int64;
clags_verify_func_t clags__verify_uint64;
clags_verify_func_t clags__verify_double;
clags_verify_func_t clags__verify_choice;
clags_verify_func_t clags__verify_path;
clags_verify_func_t clags__verify_file;
clags_verify_func_t clags__verify_dir;
clags_verify_func_t clags__verify_size;
clags_verify_func_t clags__verify_time_s;
clags_verify_func_t clags__verify_time_ns;

// the defintion of all supported value types. Format: (enum value, verification
// function, type name) if an argument’s `value_type` is not explicitly set,
// `Clags_String` is used by default.
#define clags__types                                                           \
  X(Clags_String, clags__verify_string,                                        \
    "string") /* string value; variable type: char* */                         \
  X(Clags_Custom, clags__verify_custom,                                        \
    "custom") /* custom verification function; variable type: depends on       \
                 verify function                        */                     \
  X(Clags_Bool, clags__verify_bool,                                            \
    "bool") /* boolean value; variable type: bool */                           \
  X(Clags_Int8, clags__verify_int8,                                            \
    "int8") /* signed 8-bit integer; variable type: int8_t */                  \
  X(Clags_UInt8, clags__verify_uint8,                                          \
    "uint8") /* unsigned 8-bit integer; variable type: uint8_t */              \
  X(Clags_Int32, clags__verify_int32,                                          \
    "int32") /* signed 32-bit integer; variable type: int32_t */               \
  X(Clags_UInt32, clags__verify_uint32,                                        \
    "uint32") /* unsigned 32-bit integer; variable type: uint32_t */           \
  X(Clags_Int64, clags__verify_int64,                                          \
    "int64") /* signed 64-bit integer; variable type: int64_t */               \
  X(Clags_UInt64, clags__verify_uint64,                                        \
    "uint64") /* unsigned 64-bit integer; variable type: uint64_t */           \
  X(Clags_Double, clags__verify_double,                                        \
    "double") /* floating-point value; variable type: double */                \
  X(Clags_Choice, clags__verify_choice,                                        \
    "choice") /* selects one value from a set of choices; variable type:       \
                 clags_choice_t*                        */                     \
  X(Clags_Path, clags__verify_path,                                            \
    "path") /* valid filesystem path; variable type: char* */                  \
  X(Clags_File, clags__verify_file,                                            \
    "file") /* path to a regular file; variable type: char* */                 \
  X(Clags_Dir, clags__verify_dir,                                              \
    "dir") /* path to a directory; variable type: char* */                     \
  X(Clags_Size, clags__verify_size,                                            \
    "size") /* size in bytes (supports suffixes like KiB/MB); variable type:   \
               clags_fsize_t                    */                             \
  X(Clags_TimeS, clags__verify_time_s,                                         \
    "time_s") /* time duration in seconds (supports suffixes s/m/h/d);         \
                 variable type: clags_time_t              */                   \
  X(Clags_TimeNS, clags__verify_time_ns,                                       \
    "time_ns") /* time duration in nanoseconds (supports suffixes              \
                  ns/us/ms/s/m/h/d); variable type: clags_time_t */            \
  X(Clags_Subcmd, clags__verify_subcmd,                                        \
    "subcmd") /* subcommand; variable type: clags_subcmd_t* */

// the definition of all error types and their respective descriptions
#define clags__errors                                                          \
  X(Clags_Error_Ok, "no error")                                                \
  X(Clags_Error_InvalidConfig, "configuration is invalid")                     \
  X(Clags_Error_InvalidValue,                                                  \
    "argument value does not match expected type or criteria")                 \
  X(Clags_Error_InvalidOption, "unrecognized option or flag syntax")           \
  X(Clags_Error_TooManyArguments, "too many positional arguments provided")    \
  X(Clags_Error_TooFewArguments, "required positional arguments missing")

// an auto-generated enum of all supported value types
#define X(type, func, name) type,
typedef enum { clags__types } clags_value_type_t;
#undef X

// an auto-generated enum of all error types
#define X(type, desc) type,
typedef enum { clags__errors } clags_error_t;
#undef X

// all available flag types
// if a flag's `type` is not explicitly set, `Clags_BoolFlag` is used by default
typedef enum {
  Clags_BoolFlag = 0, // standard boolean flag; set to `true` when the flag
                      // occurs; variable type: bool
  Clags_ConfigFlag, // stores a pointer to the config in which the flag was set;
                    // variable type: clags_config_t*
  Clags_CountFlag,  // tracks how many times the flag was encountered; variable
                    // type: size_t
  Clags_CallbackFlag, // invokes a user-provided callback function each time the
                      // flag occurs; callback type: clags_callback_func_t
} clags_flag_type_t;

// the definition of clags's string builder
typedef struct {
  char *items;
  size_t count;
  size_t capacity;
} clags_sb_t;

// the definition of a "generic" list
typedef struct {
  void *items;
  size_t item_size; // set by the appropiate `clags_list_<type>` macro
  size_t count;
  size_t capacity;
} clags_list_t;

// the definition of a choice
typedef struct {
  const char *value;
  const char *description;
} clags_choice_t;

// a wrapper for choice definitions, construct with `clags_choices`
typedef struct {
  clags_choice_t *items;
  size_t count;
  bool print_no_details; // do not print the full choice descriptions in
                         // `clags_usage`, if possible
  bool case_insensitive; // match choices regardless of case
} clags_choices_t;

// the definition of a subcommand
typedef struct {
  const char *name;        // the name and identifier of a subcommand
  const char *description; // help text describing the subcommand
  clags_config_t *config;  // the config that should be used to parse the
                           // subcommand's arguments
} clags_subcmd_t;

// a wrapper for subcommand definitions, construct with `clags_subcmd`
typedef struct {
  clags_subcmd_t *items;
  size_t count;
} clags_subcmds_t;

// the definition of a positional argument, construct with `clags_positional`
typedef struct {
  void *variable; // pointer to store the parsed value at; type must match
                  // `value_type`, or `clags_list_t` of that type if `is_list`
                  // is set
  const char *arg_name;    // name shown in usage for the positional argument
  const char *description; // help text describing the positional argument
  // options
  clags_value_type_t value_type; // type of the positional value. See
                                 // `clags__types` for a list of all types
  bool is_list;  // if true, this positional consumes multiple values into a
                 // `clags_list_t` of the `value_type`
  bool optional; // if true, the positional argument is optional, meaning it may
                 // be omitted
  union {        // only one of these should be set
    clags_custom_verify_func_t
        verify; // a custom verification function pointer, only if `value_type`
                // == `Clags_Custom`
    clags_choices_t *choices; // pointer to the choice wrapper, only if
                              // `value_type` == `Clags_Choice`
    clags_subcmds_t *subcmds; // pointer to subcommand definitions, only if
                              // `value_type` == `Clags_Subcmd`
    void *_data;              // internal, do not touch
  };
} clags_positional_t;

// the definition of an option argument, construct with `clags_option`
typedef struct {
  char short_flag; // single-character flag (e.g. 'o' for -o), '\0' if none
  const char *long_flag; // full-length flag (e.g. "output" for --output),
                         // nullptr if none
  void *variable; // pointer to store the parsed value at; type must match
                  // `value_type`, or `clags_list_t` of that type if `is_list`
                  // is set
  const char
      *arg_name; // name shown in usage for the option's value (e.g. "FILE")
  const char *description; // help text describing the option
  // options
  clags_value_type_t value_type; // type of the option's value. See
                                 // `clags__types` for a list of all types
  bool is_list; // if true, each occurrence appends one value to a clags_list_t
  union {       // only one of these should be set
    clags_custom_verify_func_t
        verify; // a custom verification function pointer, only if `value_type`
                // == `Clags_Custom`
    clags_choices_t *choices; // pointer to the choice wrapper, only if
                              // `value_type` == `Clags_Choice`
    void *_data;              // internal, do not touch
  };
} clags_option_t;

// the definition of a flag argument, construct with `clags_flag`
typedef struct {
  char short_flag; // single-character flag (e.g. 'h' for -h), '\0' if none
  const char
      *long_flag; // full-length flag (e.g. "help" for --help), nullptr if none
  void *variable; // pointer to store the flag value for Bool/Config/Count flags
  const char *description; // help text describing the flag
  // options
  bool exit; // true if parsing should exit immediately when the flag occurs
  clags_flag_type_t type; // behavior of the flag; see `clags_flag_type_t`
                          // (BoolFlag, CountFlag, ConfigFlag, CallbackFlag)
  clags_callback_func_t callback; // callback to invoke when `.type` is
                                  // `Clags_CallbackFlag`
} clags_flag_t;

// entirely internal
typedef struct {
  clags_positional_t *positional;
  size_t positional_count;
  size_t required_count;
  clags_option_t *option;
  size_t option_count;
  clags_flag_t *flags;
  size_t flag_count;
} clags_args_t;

typedef enum {
  Clags_Positional,
  Clags_Option,
  Clags_Flag,
} clags_arg_type_t;

// a wrapper for all arg types
// automatically construct with `clags_positional`, `clags_option` and
// `clags_flag` macros a `clags_arg_t` array can then be passed to
// `clags_config` to define a (sub)command's arguments
typedef struct {
  clags_arg_type_t type;
  union {
    clags_positional_t pos;
    clags_option_t opt;
    clags_flag_t flag;
  };
} clags_arg_t;

// the available config options
typedef struct {
  const char *ignore_prefix;   // a custom prefix that instructs the parser to
                               // ignore the current argument
  const char *list_terminator; // a custom list terminator that tells the parser
                               // that following positional arguments do no
                               // longer belong to the current list
  bool print_no_notes;         // do not print the `Notes` section in the usage
  bool allow_option_parsing_toggle; // allow "--" to be used to toggle option
                                    // and flag parsing, one-time disabling is
                                    // always enabled
  bool duplicate_strings; // duplicate all strings instead of setting variables
                          // to the content of argv, free the allocated memory
                          // via `clags_config_free_allocs`
  clags_log_handler_t log_handler; // a custom log handler
  clags_log_level_t
      min_log_level;       // the minimal log level for which to print logs
  const char *description; // a description of the current (sub)command
} clags_options_t;

// a config for a single (sub)command
// construct with the `clags_config` macro
struct clags_config_t {
  clags_arg_t *args;       // the array of argument definitions
  size_t args_count;       // the amount of argument definitions
  clags_options_t options; // additional settings

  // internal, set automatically
  const char *name; // the program name or the name of the current subcommand
  clags_config_t *parent; // pointer to the parent (sub)command's config
  bool invalid;           // argument definitions are invalid
  clags_list_t
      allocs; // all duplicated strings allocated in this config's context, only
              // if `options.duplicate_strings` is enabled
  clags_error_t error; // the last error detected while parsing this config
};

// helper macros
#define clags_arr_len(arr) (sizeof(arr) / sizeof((arr)[0]))
#define clags_return_defer(value)                                              \
  do {                                                                         \
    result = (value);                                                          \
    goto defer;                                                                \
  } while (0)
#define clags_assert(expr, msg)                                                \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "%s:%d in %s: [FATAL] Assertion failed [%s] : %s\n",     \
              __FILE__, __LINE__, __func__, #expr, (msg));                     \
      fflush(stderr);                                                          \
      abort();                                                                 \
    }                                                                          \
  } while (0)
#define clags_unreachable(msg)                                                 \
  do {                                                                         \
    fprintf(stderr, "%s:%d in %s: [FATAL] Unreachable: %s\n", __FILE__,        \
            __LINE__, __func__, (msg));                                        \
    fflush(stderr);                                                            \
    abort();                                                                   \
  } while (0)

/* Custom Variable Types */

// predefined list constructors for different types
// returns an empty `clags_list_t` with item_size set appropriately
// use `clags_custom_list(size)` to define a list of custom element size
#define clags__sized_list(size)                                                \
  {.items = nullptr, .count = 0, .capacity = 0, .item_size = (size)}
#define clags_list() clags__sized_list(sizeof(char *))
#define clags_string_list() clags__sized_list(sizeof(char *))
#define clags_path_list() clags__sized_list(sizeof(char *))
#define clags_file_list() clags__sized_list(sizeof(char *))
#define clags_dir_list() clags__sized_list(sizeof(char *))
#define clags_custom_list(size) clags__sized_list(size)
#define clags_bool_list() clags__sized_list(sizeof(bool))
#define clags_int8_list() clags__sized_list(sizeof(int8_t))
#define clags_uint8_list() clags__sized_list(sizeof(uint8_t))
#define clags_int32_list() clags__sized_list(sizeof(int32_t))
#define clags_uint32_list() clags__sized_list(sizeof(uint32_t))
#define clags_int64_list() clags__sized_list(sizeof(int64_t))
#define clags_uint64_list() clags__sized_list(sizeof(uint64_t))
#define clags_double_list() clags__sized_list(sizeof(double))
#define clags_size_list() clags__sized_list(sizeof(clags_fsize_t))
#define clags_time_list() clags__sized_list(sizeof(clags_time_t))
#define clags_choice_list() clags__sized_list(sizeof(clags_choice_t *))

// macros for easy value extraction from lists
// `value_type` must match the type stored within the list
#define clags_list_element(list, value_type, index)                            \
  ((value_type *)(list).items)[index]

// wrapper for a `clags_choice_t` array
// use designated initializers in the variadic arguments to set additional
// options. see `clags_choices_t` for all available fields
#define clags_choices(arr, ...)                                                \
  {.items = (arr), .count = clags_arr_len(arr), __VA_ARGS__}
// macro for getting the pointer to the index-th choice
#define clags_choice_value(choices, index) (&(choices)[index])

// wrapper for a `clags_subcmd_t` array
#define clags_subcmds(subcmds)                                                 \
  {.items = (subcmds), .count = clags_arr_len(subcmds)}

/* Argument Constructors */

/*
  Define a positional argument.

  Parameters:
    var  : pointer to the variable that receives the parsed value, must be of a
  type matching the value type, or `clags_list_t` of that type if `is_list` is
  set, default: char* name : argument name shown in usage desc : description
  shown in help output

  Additional behavior (value type, lists, optionality, choices, custom
  verification, subcommands, etc.) is configured via designated initializers in
  the variadic arguments. See `clags_positional_t` for all available fields.
*/
#define clags_positional(var, name, desc, ...)                                 \
  {                                                                            \
    .type = Clags_Positional, .pos = {                                         \
      .variable = (var),                                                       \
      .arg_name = (name),                                                      \
      .description = (desc),                                                   \
      __VA_ARGS__                                                              \
    }                                                                          \
  }

/*
  Define an option argument.

  Parameters:
    sflag : short option character (e.g. 'o' for -o), or '\0' if unused
    lflag : long option name (e.g. "output"), or nullptr if unused
    var   : pointer to the variable that receives the parsed value, must be of a
  type matching the value type, or `clags_list_t` of that type if `is_list` is
  set, default: char* name  : value name shown in usage (e.g. "FILE") desc  :
  description shown in help output

  Additional behavior (value type, lists, choices, custom verification, etc.)
  is configured via designated initializers in the variadic arguments.
  See `clags_option_t` for all available fields.
*/
#define clags_option(sflag, lflag, var, name, desc, ...)                       \
  {                                                                            \
    .type = Clags_Option, .opt = {                                             \
      .short_flag = (sflag),                                                   \
      .long_flag = (lflag),                                                    \
      .variable = (var),                                                       \
      .arg_name = (name),                                                      \
      .description = (desc),                                                   \
      __VA_ARGS__                                                              \
    }                                                                          \
  }

/*
  Define a flag argument.

  Parameters:
    sflag : short flag character (e.g. 'h' for -h), or '\0' if unused
    lflag : long flag name (e.g. "help"), or nullptr if unused
    var   : pointer to the variable that receives the flag value; type depends
  on `.type`, default: bool desc  : description shown in help output

  Additional behavior (exit-on-set, count flags, config pointer storage,
  callbacks, etc.) is configured via designated initializers in the variadic
  arguments. See `clags_flag_t` for all available fields.
*/
#define clags_flag(sflag, lflag, var, desc, ...)                               \
  {                                                                            \
    .type = Clags_Flag, .flag = {                                              \
      .short_flag = (sflag),                                                   \
      .long_flag = (lflag),                                                    \
      .variable = (var),                                                       \
      .description = (desc),                                                   \
      __VA_ARGS__                                                              \
    }                                                                          \
  }

// simple helpers for the common help flags
#define clags_flag_help(val)                                                   \
  clags_flag('h', "help", val, "print this help dialog", .exit = true)
#define clags_flag_help_config(val)                                            \
  clags_flag('h', "help", val, "print this help dialog", .exit = true,         \
             .type = Clags_ConfigFlag)

/* Config Constructors */

/*
  Construct a `clags_config_t` from an array of arguments.

  Parameters:
    arguments : array of `clags_arg_t` defining the positionals, options, and
  flags
    ...       : optional designated initializers for `clags_options_t` fields
                (e.g. .ignore_prefix="!", .list_terminator="::",
  .duplicate_strings=true, etc.), see `clags_options_t` for all available
  fields.
*/
#define clags_config(arguments, ...)                                           \
  {                                                                            \
    .args = (arguments), .args_count = clags_arr_len(arguments),               \
    .allocs = {.item_size = sizeof(char *)}, .options = {                      \
      __VA_ARGS__                                                              \
    }                                                                          \
  }

/*
  Construct a `clags_config_t` from an array of arguments and a pre-defined
  options struct. Useful when sharing options over mutiple configs.

  Parameters:
    arguments : array of `clags_arg_t` defining positionals, options, and flags
    opts      : a fully initialized `clags_options_t` struct with custom config
  options
*/
#define clags_config_with_options(arguments, opts)                             \
  {.args = (arguments),                                                        \
   .args_count = clags_arr_len(arguments),                                     \
   .allocs = {.item_size = sizeof(char *)},                                    \
   .options = (opts)}

/* Core Functions */

/*
  Parse arguments based on the provided config.

  Arguments:
    - argc          : the number of arguments
    - argv          : the array of arguments
    - config        : pointer to a config with argument definitions and other
  options

  Returns:
    clags_config_t* : pointer to the failed config. If parsing fails, the
  `.error` field will be set to indicate the type of the encountered error.
*/
[[nodiscard]] clags_config_t *clags_parse(int argc, char **argv,
                                          clags_config_t *config);

/*
  Print a detailed usage based on the provided config.

  Arguments:
    - program_name  : the name of the program
    - config        : pointer to a config with argument definitions and other
  options
*/
void clags_usage(const char *program_name, clags_config_t *config);

/*
  Get the index of a selected subcommand in the provided subcommand array.

  Arguments:
    - subcmds       : pointer to a clags_subcmds_t containing the subcommand
  array
    - subcmd        : pointer to the selected clags_subcmd_t to find

  Returns:
    int             : the index of the selected subcommand in the array,
                      or -1 if the subcommand was not found or either argument
  is nullptr
*/
[[nodiscard]] int clags_subcmd_index(clags_subcmds_t *subcmds,
                                     clags_subcmd_t *subcmd);

/*
  Get the index of a selected choice in the provided choice array.

  Arguments:
    - choices       : pointer to clags_choices_t containing the choice array
    - choice        : pointer to the selected clags_choice_t to find

  Returns:
    int             : the index of the selected choice in the array,
                      or -1 if the choice was not found or either argument is
  nullptr
*/
[[nodiscard]] int clags_choice_index(clags_choices_t *choices,
                                     clags_choice_t *choice);

/*
  Duplicate a string if string duplication is enabled in the config,
  otherwise return the original string.

  Arguments:
    - config  : pointer to the clags configuration
    - string  : the string to duplicate

  Returns:
    char*     : pointer to the duplicated string if duplication is enabled,
                otherwise the original string. Memory allocated for duplicates
                is tracked internally within the config and will be freed when
  the config is cleaned up.
*/
[[nodiscard]] char *clags_config_duplicate_string(clags_config_t *config,
                                                  const char *string);

/*
  Free all memory allocated for strings duplicated during parsing.
  This only applies if `.duplicate_strings` was enabled in the config.

  Arguments:
    - config        : pointer to the clags_config_t whose duplicated strings
  should be freed
*/
void clags_config_free_allocs(clags_config_t *config);

/*
  Free all lists and allocated strings of a config.
  The function does not propagate to child configs.

  Arguments:
    - config        : pointer to the config of which to free all strings and
  lists
*/
void clags_config_free(clags_config_t *config);

/*
  Free all memory associated with a `clags_list_t` instance.

  Arguments:
    - list          : a pointer to the list to free
*/
void clags_list_free(clags_list_t *list);

/*
  Return a description of the provided error type.

  Arguments:
    - error         : the error type

  Returns:
    const char*     : a string description of the provided error type
*/
[[nodiscard]] const char *clags_error_description(clags_error_t error);

/* Logging */

/*
  Use a config's log handler to log a formatted message.

  Arguments:
    - config        : the config for which to log using its options
    - level         : the log level of the message, if less than the configs
  minimal log level, nothing will be logged
    - format        : the printf-style format of the message
    - ...           : the variadic format arguments
*/
void clags_log(clags_config_t *config, clags_log_level_t level,
               const char *format, ...) CLAGS__PRINTF_FORMAT(3, 4);
// log the content of a string builder instead of a formatted message
void clags_log_sb(clags_config_t *config, clags_log_level_t level,
                  clags_sb_t *sb);

/* String Builder Functionality */
void clags_sb_appendf(clags_sb_t *sb, const char *format, ...);
void clags_sb_append_null(clags_sb_t *sb);
void clags_sb_free(clags_sb_t *sb);
