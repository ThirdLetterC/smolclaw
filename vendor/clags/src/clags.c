#include <ctype.h>
#include <errno.h>
#include <math.h>

#include "clags/clags.h"

#define X(type, func, name) [type] = func,
static clags_verify_func_ptr_t clags__verify_funcs[] = {clags__types};
#undef X

[[nodiscard]] static inline bool
clags__is_valid_value_type(clags_value_type_t value_type) {
  return (size_t)value_type <
         (sizeof(clags__verify_funcs) / sizeof(*clags__verify_funcs));
}

#define X(type, func, name) [type] = name,
static const char *clags__type_names[] = {clags__types};
#undef X

#ifndef CLAGS_MAX_PARSE_DEPTH
#define CLAGS_MAX_PARSE_DEPTH 64
#endif // CLAGS_MAX_PARSE_DEPTH

[[nodiscard]] static inline bool
clags__has_config_cycle(const clags_config_t *parent,
                        const clags_config_t *candidate) {
  for (const clags_config_t *current = parent; current != nullptr;
       current = current->parent) {
    if (current == candidate) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] static inline bool
clags__checked_add_size(size_t *result, size_t lhs, size_t rhs) {
#if CLAGS_HAS_STDCKDINT
  return ckd_add(result, lhs, rhs);
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_add_overflow(lhs, rhs, result);
#else
  if (SIZE_MAX - lhs < rhs)
    return true;
  *result = lhs + rhs;
  return false;
#endif
}

[[nodiscard]] static inline bool
clags__checked_mul_size(size_t *result, size_t lhs, size_t rhs) {
#if CLAGS_HAS_STDCKDINT
  return ckd_mul(result, lhs, rhs);
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_mul_overflow(lhs, rhs, result);
#else
  if (lhs != 0 && rhs > SIZE_MAX / lhs)
    return true;
  *result = lhs * rhs;
  return false;
#endif
}

static inline const char *clags__skip_space(const char *value) {
  if (value == nullptr) {
    return nullptr;
  }
  while (*value != '\0' && isspace((unsigned char)*value)) {
    value++;
  }
  return value;
}

[[nodiscard]] static inline bool clags__has_sign_prefix(const char *value) {
  const char *trimmed = clags__skip_space(value);
  return trimmed != nullptr && (*trimmed == '-' || *trimmed == '+');
}

[[nodiscard]] static inline bool clags__is_empty_string(const char *value) {
  return value != nullptr && *value == '\0';
}

[[nodiscard]] static inline bool
clags__list_expected_item_size(clags_value_type_t value_type,
                               size_t *expected_item_size) {
  if (expected_item_size == nullptr) {
    return false;
  }
  switch (value_type) {
  case Clags_String:
  case Clags_Path:
  case Clags_File:
  case Clags_Dir:
    *expected_item_size = sizeof(char *);
    return true;
  case Clags_Bool:
    *expected_item_size = sizeof(bool);
    return true;
  case Clags_Int8:
    *expected_item_size = sizeof(int8_t);
    return true;
  case Clags_UInt8:
    *expected_item_size = sizeof(uint8_t);
    return true;
  case Clags_Int32:
    *expected_item_size = sizeof(int32_t);
    return true;
  case Clags_UInt32:
    *expected_item_size = sizeof(uint32_t);
    return true;
  case Clags_Int64:
    *expected_item_size = sizeof(int64_t);
    return true;
  case Clags_UInt64:
    *expected_item_size = sizeof(uint64_t);
    return true;
  case Clags_Double:
    *expected_item_size = sizeof(double);
    return true;
  case Clags_Choice:
    *expected_item_size = sizeof(clags_choice_t *);
    return true;
  case Clags_Size:
    *expected_item_size = sizeof(clags_fsize_t);
    return true;
  case Clags_TimeS:
  case Clags_TimeNS:
    *expected_item_size = sizeof(clags_time_t);
    return true;
  case Clags_Subcmd:
    *expected_item_size = sizeof(clags_subcmd_t *);
    return true;
  case Clags_Custom:
    return false;
  default:
    return false;
  }
}

[[nodiscard]] static inline char *clags__strdup(const char *string) {
  if (string == nullptr)
    return nullptr;
  size_t length = strlen(string);
  size_t alloc_size = 0;
  clags_assert(!clags__checked_add_size(&alloc_size, length, (size_t)1),
               "String duplication size overflow!");
  char *new_string = CLAGS_CALLOC(alloc_size, sizeof(char));
  clags_assert(new_string != nullptr, "Out of memory!");
  memcpy(new_string, string, alloc_size);
  return new_string;
}

static inline const char *clags__strchrnull(const char *string, char c) {
  if (string == nullptr)
    return nullptr;
  const char *s = string;
  while (*s != '\0') {
    if (*s == c)
      return s;
    s++;
  }
  return s;
}

[[nodiscard]] static inline bool
clags__next_capacity(size_t current, size_t required, size_t *next) {
  size_t capacity = current;
  if (capacity == 0)
    capacity = CLAGS_LIST_INIT_CAPACITY;
  if (capacity == 0)
    return false;
  while (capacity < required) {
    size_t doubled = 0;
    if (clags__checked_mul_size(&doubled, capacity, (size_t)2))
      return false;
    capacity = doubled;
  }
  *next = capacity;
  return true;
}

static inline void clags__sb_reserve(clags_sb_t *sb, size_t capacity) {
  if (sb->capacity >= capacity)
    return;
  size_t new_capacity = 0;
  clags_assert(clags__next_capacity(sb->capacity, capacity, &new_capacity),
               "String builder capacity overflow!");
  size_t alloc_size = 0;
  clags_assert(
      !clags__checked_mul_size(&alloc_size, new_capacity, sizeof(*sb->items)),
      "String builder allocation overflow!");
  sb->items = CLAGS_REALLOC(sb->items, alloc_size);
  clags_assert(sb->items != nullptr, "Out of memory!");
  sb->capacity = new_capacity;
}

void clags_sb_appendf(clags_sb_t *sb, const char *format, ...) {
  va_list args, args_copy;

  va_start(args, format);
  va_copy(args_copy, args);

  int n = vsnprintf(nullptr, 0, format, args_copy);
  va_end(args_copy);
  clags_assert(n >= 0, "Failed to format string!");

  size_t formatted_len = (size_t)n;
  size_t capacity = 0;
  clags_assert(!clags__checked_add_size(&capacity, sb->count, formatted_len),
               "String builder length overflow!");
  clags_assert(!clags__checked_add_size(&capacity, capacity, (size_t)1),
               "String builder length overflow!");
  clags__sb_reserve(sb, capacity);
  char *start = sb->items + sb->count;

  vsnprintf(start, formatted_len + 1, format, args);
  va_end(args);

  sb->count = capacity - 1;
}

void clags_sb_append_null(clags_sb_t *sb) {
  size_t capacity = 0;
  clags_assert(!clags__checked_add_size(&capacity, sb->count, (size_t)1),
               "String builder length overflow!");
  clags__sb_reserve(sb, capacity);
  sb->items[sb->count++] = '\0';
}

void clags_sb_free(clags_sb_t *sb) {
  if (!sb)
    return;
  CLAGS_FREE(sb->items);
  sb->items = nullptr;
  sb->count = sb->capacity = 0;
}

void clags__default_log_handler(clags_log_level_t level, const char *format,
                                va_list args) {
  switch (level) {
  case Clags_Info: {
    fprintf(stdout, "[INFO] ");
    vfprintf(stdout, format, args);
    fprintf(stdout, "\n");
    return;
  } break;
  case Clags_Warning: {
    fprintf(stderr, "[WARNING] ");
  } break;
  case Clags_Error: {
    fprintf(stderr, "[ERROR] ");
  } break;
  case Clags_ConfigWarning: {
    fprintf(stderr, "[CONFIG_WARNING] ");
  } break;
  case Clags_ConfigError: {
    fprintf(stderr, "[CONFIG_ERROR] ");
  } break;
  case Clags_NoLogs:
    return;
  default: {
    clags_unreachable("Invalid clags_log_level_t!");
  }
  }
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
}

void clags_log(clags_config_t *config, clags_log_level_t level,
               const char *format, ...) {
  if (config && config->options.min_log_level > level)
    return;
  va_list args;
  va_start(args, format);
  clags_log_handler_t handler = (config && config->options.log_handler)
                                    ? config->options.log_handler
                                    : clags__default_log_handler;
  handler(level, format, args);
  va_end(args);
}

void clags_log_sb(clags_config_t *config, clags_log_level_t level,
                  clags_sb_t *sb) {
  clags_log(config, level, "%s", sb->items);
}

[[nodiscard]] char *clags_config_duplicate_string(clags_config_t *config,
                                                  const char *string) {
  if (config == nullptr)
    return (char *)string;
  char *duplicate;
  if (config->options.duplicate_strings) {
    duplicate = clags__strdup(string);
    clags_assert(duplicate != nullptr, "Out of memory!");

    clags_list_t *allocs = &config->allocs;
    if (allocs->item_size == 0)
      allocs->item_size = sizeof(char *);
    if (allocs->count >= allocs->capacity) {
      size_t required_capacity = 0;
      clags_assert(!clags__checked_add_size(&required_capacity, allocs->count,
                                            (size_t)1),
                   "Allocation tracking capacity overflow!");
      size_t new_capacity = 0;
      clags_assert(clags__next_capacity(allocs->capacity, required_capacity,
                                        &new_capacity),
                   "Allocation tracking capacity overflow!");
      size_t alloc_size = 0;
      clags_assert(!clags__checked_mul_size(&alloc_size, allocs->item_size,
                                            new_capacity),
                   "Allocation tracking size overflow!");
      allocs->items = CLAGS_REALLOC(allocs->items, alloc_size);
      clags_assert(allocs->items != nullptr, "Out of memory!");
      allocs->capacity = new_capacity;
    }
    ((char **)allocs->items)[allocs->count++] = duplicate;
  } else {
    duplicate = (char *)string;
  }
  return duplicate;
}

bool clags__verify_string(clags_config_t *config,
                          [[maybe_unused]] const char *arg_name,
                          const char *arg, void *pvalue,
                          [[maybe_unused]] void *data) {
  if (pvalue)
    *(char **)pvalue = clags_config_duplicate_string(config, arg);
  return true;
}

bool clags__verify_bool(clags_config_t *config, const char *arg_name,
                        const char *arg, void *pvalue,
                        [[maybe_unused]] void *data) {
  if (strcasecmp(arg, "true") == 0 || strcasecmp(arg, "yes") == 0 ||
      strcasecmp(arg, "y") == 0) {
    if (pvalue)
      *(bool *)pvalue = true;
    return true;
  } else if (strcasecmp(arg, "false") == 0 || strcasecmp(arg, "no") == 0 ||
             strcasecmp(arg, "n") == 0) {
    if (pvalue)
      *(bool *)pvalue = false;
    return true;
  }
  clags_log(config, Clags_Error,
            "Invalid boolean value for argument '%s': '%s'!", arg_name, arg);
  return false;
}

bool clags__verify_int8(clags_config_t *config, const char *arg_name,
                        const char *arg, void *pvalue,
                        [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  long value = strtol(arg, &endptr, 0);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid int8 value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || value < INT8_MIN || value > INT8_MAX) {
    clags_log(config, Clags_Error,
              "int8 value out of range (%" PRId8 " to %" PRId8
              ") for argument '%s': '%s'!",
              INT8_MIN, INT8_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(int8_t *)pvalue = (int8_t)value;
  return true;
}

bool clags__verify_uint8(clags_config_t *config, const char *arg_name,
                         const char *arg, void *pvalue,
                         [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  unsigned long value = strtoul(arg, &endptr, 0);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid uint8 value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || value > UINT8_MAX || clags__has_sign_prefix(arg)) {
    clags_log(config, Clags_Error,
              "uint8 value out of range (0 to %" PRIu8
              ") for argument '%s': '%s'!",
              UINT8_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(uint8_t *)pvalue = (uint8_t)value;
  return true;
}

bool clags__verify_int32(clags_config_t *config, const char *arg_name,
                         const char *arg, void *pvalue,
                         [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  long value = strtol(arg, &endptr, 0);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid int32 value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || value < INT32_MIN || value > INT32_MAX) {
    clags_log(config, Clags_Error,
              "int32 value out of range (%" PRId32 " to %" PRId32
              ") for argument '%s': '%s'!",
              INT32_MIN, INT32_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(int32_t *)pvalue = (int32_t)value;
  return true;
}

bool clags__verify_uint32(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue,
                          [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  unsigned long value = strtoul(arg, &endptr, 0);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid uint32 value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || value > UINT32_MAX || clags__has_sign_prefix(arg)) {
    clags_log(config, Clags_Error,
              "uint32 value out of range (0 to %" PRIu32
              ") for argument '%s': '%s'!",
              UINT32_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(uint32_t *)pvalue = (uint32_t)value;
  return true;
}

bool clags__verify_int64(clags_config_t *config, const char *arg_name,
                         const char *arg, void *pvalue,
                         [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  long long value = strtoll(arg, &endptr, 0);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid int64 value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || value < INT64_MIN || value > INT64_MAX) {
    clags_log(config, Clags_Error,
              "int64 value out of range (%" PRId64 " to %" PRId64
              ") for argument '%s': '%s'!",
              INT64_MIN, INT64_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(int64_t *)pvalue = (int64_t)value;
  return true;
}

bool clags__verify_uint64(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue,
                          [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  unsigned long long value = strtoull(arg, &endptr, 0);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid uint64 value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || value > UINT64_MAX || clags__has_sign_prefix(arg)) {
    clags_log(config, Clags_Error,
              "uint64 value out of range (0 to %" PRIu64
              ") for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(uint64_t *)pvalue = (uint64_t)value;
  return true;
}

bool clags__verify_double(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue,
                          [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  double value = strtod(arg, &endptr);

  if (endptr == arg || *endptr != '\0') {
    clags_log(config, Clags_Error,
              "Invalid double value for argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  if (errno == ERANGE || !isfinite(value) || value > DBL_MAX ||
      value < -DBL_MAX) {
    clags_log(config, Clags_Error,
              "double value out of range (%lf to %lf) for argument '%s': '%s'!",
              DBL_MAX, -DBL_MAX, arg_name, arg);
    return false;
  }

  if (pvalue)
    *(double *)pvalue = value;
  return true;
}

bool clags__verify_choice(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue, void *data) {
  clags_choice_t **pchoice = (clags_choice_t **)pvalue;
  clags_choices_t *choices = (clags_choices_t *)data;
  for (size_t i = 0; i < choices->count; ++i) {
    clags_choice_t *choice = choices->items + i;
    if ((choices->case_insensitive && strcasecmp(choice->value, arg) == 0) ||
        (!choices->case_insensitive && strcmp(choice->value, arg) == 0)) {
      if (pchoice)
        *pchoice = choice;
      return true;
    }
  }
  clags_log(config, Clags_Error, "Invalid choice for argument '%s': '%s'!",
            arg_name, arg);
  return false;
}

bool clags__verify_path(clags_config_t *config, const char *arg_name,
                        const char *arg, void *pvalue,
                        [[maybe_unused]] void *data) {
  struct stat attr;
  if (stat(arg, &attr) == -1) {
    clags_log(config, Clags_Error, "Invalid path for argument '%s': '%s' : %s!",
              arg_name, arg, strerror(errno));
    return false;
  }
  if (pvalue)
    *(char **)pvalue = clags_config_duplicate_string(config, arg);
  return true;
}

bool clags__verify_file(clags_config_t *config, const char *arg_name,
                        const char *arg, void *pvalue,
                        [[maybe_unused]] void *data) {
  struct stat attr;
  if (stat(arg, &attr) == -1) {
    clags_log(config, Clags_Error, "Invalid path for argument '%s': '%s' : %s!",
              arg_name, arg, strerror(errno));
    return false;
  }
  if (!S_ISREG(attr.st_mode)) {
    clags_log(config, Clags_Error,
              "Path for arguments '%s' is not a file: '%s'!", arg_name, arg);
    return false;
  }
  if (pvalue)
    *(char **)pvalue = clags_config_duplicate_string(config, arg);
  return true;
}

bool clags__verify_dir(clags_config_t *config, const char *arg_name,
                       const char *arg, void *pvalue,
                       [[maybe_unused]] void *data) {
  struct stat attr;
  if (stat(arg, &attr) == -1) {
    clags_log(config, Clags_Error, "Invalid path for argument '%s': '%s' : %s!",
              arg_name, arg, strerror(errno));
    return false;
  }
  if (!S_ISDIR(attr.st_mode)) {
    clags_log(config, Clags_Error,
              "Path for arguments '%s' is not a dir: '%s'!", arg_name, arg);
    return false;
  }
  if (pvalue)
    *(char **)pvalue = clags_config_duplicate_string(config, arg);
  return true;
}

bool clags__verify_size(clags_config_t *config, const char *arg_name,
                        const char *arg, void *pvalue,
                        [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;
  unsigned long long value = strtoull(arg, &endptr, 10);

  if (endptr == arg) {
    clags_log(config, Clags_Error,
              "No leading number in size argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  clags_fsize_t factor;
  if (*endptr == '\0' || strcmp(endptr, "B") == 0)
    factor = 1;
  else if (strcasecmp(endptr, "KiB") == 0)
    factor = 1ULL << 10;
  else if (strcasecmp(endptr, "KB") == 0)
    factor = 1000;
  else if (strcasecmp(endptr, "MiB") == 0)
    factor = 1ULL << 20;
  else if (strcasecmp(endptr, "MB") == 0)
    factor = 1000000;
  else if (strcasecmp(endptr, "GiB") == 0)
    factor = 1ULL << 30;
  else if (strcasecmp(endptr, "GB") == 0)
    factor = 1000000000;
  else if (strcasecmp(endptr, "TiB") == 0)
    factor = 1ULL << 40;
  else if (strcasecmp(endptr, "TB") == 0)
    factor = 1000000000000;
  else {
    clags_log(config, Clags_Error, "Invalid size unit for argument '%s': '%s'!",
              arg_name, endptr);
    return false;
  }

  if (errno == ERANGE || value > UINT64_MAX / factor ||
      clags__has_sign_prefix(arg)) {
    clags_log(config, Clags_Error,
              "clags_fsize_t value out of range (0 to %" PRIu64
              ") for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }
  if (pvalue)
    *(clags_fsize_t *)pvalue = (clags_fsize_t)value * factor;
  return true;
}

bool clags__verify_time_s(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue,
                          [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;

  double value = strtod(arg, &endptr);
  if (endptr == arg) {
    clags_log(config, Clags_Error,
              "No leading number in time argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  clags_time_t factor;
  if (*endptr == '\0' || strcasecmp(endptr, "s") == 0)
    factor = 1;
  else if (strcasecmp(endptr, "m") == 0)
    factor = 60;
  else if (strcasecmp(endptr, "h") == 0)
    factor = 3600;
  else if (strcasecmp(endptr, "d") == 0)
    factor = 24 * 3600;
  else {
    clags_log(config, Clags_Error, "Invalid time unit for argument '%s': '%s'!",
              arg_name, endptr);
    return false;
  }
  if (errno == ERANGE || !isfinite(value) || value < 0) {
    clags_log(config, Clags_Error,
              "clags_time_t value out of range (0s to %" PRIu64
              "s) for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }
  long double scaled = (long double)value * (long double)factor;
  if (!isfinite(scaled) || scaled > (long double)UINT64_MAX) {
    clags_log(config, Clags_Error,
              "clags_time_t value out of range (0s to %" PRIu64
              "s) for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }
  if (pvalue)
    *(clags_time_t *)pvalue = (clags_time_t)scaled;
  return true;
}

bool clags__verify_time_ns(clags_config_t *config, const char *arg_name,
                           const char *arg, void *pvalue,
                           [[maybe_unused]] void *data) {
  char *endptr;
  errno = 0;

  double value = strtod(arg, &endptr);
  if (endptr == arg) {
    clags_log(config, Clags_Error,
              "No leading number in time argument '%s': '%s'!", arg_name, arg);
    return false;
  }
  clags_time_t factor;
  if (*endptr == '\0' || strcasecmp(endptr, "ns") == 0)
    factor = 1;
  else if (strcasecmp(endptr, "us") == 0)
    factor = 1000ULL;
  else if (strcasecmp(endptr, "ms") == 0)
    factor = 1000000ULL;
  else if (strcasecmp(endptr, "s") == 0)
    factor = 1000000000ULL;
  else if (strcasecmp(endptr, "m") == 0)
    factor = 60ULL * 1000000000ULL;
  else if (strcasecmp(endptr, "h") == 0)
    factor = 3600ULL * 1000000000ULL;
  else if (strcasecmp(endptr, "d") == 0)
    factor = 24ULL * 3600ULL * 1000000000ULL;
  else {
    clags_log(config, Clags_Error, "Invalid time unit for argument '%s': '%s'!",
              arg_name, endptr);
    return false;
  }
  if (errno == ERANGE || !isfinite(value) || value < 0) {
    clags_log(config, Clags_Error,
              "clags_time_t value out of range (0ns to %" PRIu64
              "ns) for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }
  long double scaled = (long double)value * (long double)factor;
  if (!isfinite(scaled) || scaled > (long double)UINT64_MAX) {
    clags_log(config, Clags_Error,
              "clags_time_t value out of range (0ns to %" PRIu64
              "ns) for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }
  long double rounded = scaled + 0.5L;
  if (rounded > (long double)UINT64_MAX) {
    clags_log(config, Clags_Error,
              "clags_time_t value out of range (0ns to %" PRIu64
              "ns) for argument '%s': '%s'!",
              UINT64_MAX, arg_name, arg);
    return false;
  }
  if (pvalue)
    *(clags_time_t *)pvalue = (clags_time_t)rounded;
  return true;
}

bool clags__verify_subcmd(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue, void *data) {
  clags_subcmds_t *subcmds = (clags_subcmds_t *)data;
  for (size_t i = 0; i < subcmds->count; ++i) {
    clags_subcmd_t subcmd = subcmds->items[i];
    if (strcmp(subcmd.name, arg) == 0) {
      if (pvalue)
        *(clags_subcmd_t **)pvalue = &subcmds->items[i];
      return true;
    }
  }
  clags_log(config, Clags_Error, "unknown subcommand '%s' for argument '%s'!",
            arg, arg_name);
  return false;
}

bool clags__verify_custom(clags_config_t *config, const char *arg_name,
                          const char *arg, void *pvalue, void *data) {
  if (data == nullptr) {
    clags_log(config, Clags_ConfigError,
              "Missing custom verifier for argument '%s'!", arg_name);
    return false;
  }
  clags_custom_verify_func_t custom_verify =
      *(clags_custom_verify_func_t *)data;
  if (custom_verify == nullptr) {
    clags_log(config, Clags_ConfigError,
              "Invalid custom verifier for argument '%s'!", arg_name);
    return false;
  }
  if (!custom_verify(config, arg_name, arg, pvalue)) {
    clags_log(config, Clags_Error,
              "Value for argument '%s' does not match custom criteria: '%s'!",
              arg_name, arg);
    return false;
  }
  return true;
}

static inline bool clags__append_to_list(clags_config_t *config,
                                         clags_value_type_t value_type,
                                         const char *arg_name, const char *arg,
                                         void *variable, void *data,
                                         clags_custom_verify_func_t verify) {
  if (!clags__is_valid_value_type(value_type)) {
    clags_log(config, Clags_Error, "Invalid value type %d for argument '%s'!",
              (int)value_type, arg_name);
    return false;
  }
  clags_list_t *list = (clags_list_t *)variable;
  size_t item_size = list->item_size;
  if (item_size == 0) {
    clags_log(config, Clags_Error,
              "List item size for argument '%s' may not be 0!", arg_name);
    return false;
  }
  size_t expected_item_size = 0;
  if (value_type != Clags_Custom &&
      clags__list_expected_item_size(value_type, &expected_item_size) &&
      item_size != expected_item_size) {
    clags_log(
        config, Clags_Error,
        "List item size mismatch for argument '%s': expected %zu, got %zu!",
        arg_name, expected_item_size, item_size);
    return false;
  }
  if (list->count >= list->capacity) {
    size_t required_capacity = 0;
    clags_assert(
        !clags__checked_add_size(&required_capacity, list->count, (size_t)1),
        "List capacity overflow!");
    size_t new_capacity = 0;
    clags_assert(
        clags__next_capacity(list->capacity, required_capacity, &new_capacity),
        "List capacity overflow!");
    size_t alloc_size = 0;
    clags_assert(!clags__checked_mul_size(&alloc_size, new_capacity, item_size),
                 "List allocation size overflow!");
    list->items = CLAGS_REALLOC(list->items, alloc_size);
    clags_assert(list->items != nullptr, "Out of memory!");
    list->capacity = new_capacity;
  }
  size_t offset = 0;
  clags_assert(!clags__checked_mul_size(&offset, item_size, list->count),
               "List offset overflow!");
  char *ptr = (char *)list->items;
  void *verify_data = data;
  clags_custom_verify_func_t custom_verify = verify;
  if (value_type == Clags_Custom) {
    verify_data = &custom_verify;
  }
  if (clags__verify_funcs[value_type](config, arg_name, arg, ptr + offset,
                                      verify_data)) {
    list->count++;
    return true;
  }
  return false;
}

static inline bool
clags__set_arg(clags_config_t *config, clags_value_type_t value_type,
               const char *arg_name, const char *arg, void *variable,
               void *data, clags_custom_verify_func_t verify, bool is_list) {
  if (!clags__is_valid_value_type(value_type)) {
    clags_log(config, Clags_Error, "Invalid value type %d for argument '%s'!",
              (int)value_type, arg_name);
    config->error = Clags_Error_InvalidValue;
    return false;
  }
  bool result;
  if (is_list) {
    result = clags__append_to_list(config, value_type, arg_name, arg, variable,
                                   data, verify);
  } else {
    void *verify_data = data;
    clags_custom_verify_func_t custom_verify = verify;
    if (value_type == Clags_Custom) {
      verify_data = &custom_verify;
    }
    result = clags__verify_funcs[value_type](config, arg_name, arg, variable,
                                             verify_data);
  }
  if (!result)
    config->error = Clags_Error_InvalidValue;
  return result;
}

static inline void clags__set_flag(clags_config_t *config, clags_flag_t *flag) {
  switch (flag->type) {
  case Clags_BoolFlag: {
    if (flag->variable == nullptr)
      return;
    *(bool *)flag->variable = true;
  } break;
  case Clags_ConfigFlag: {
    if (flag->variable == nullptr)
      return;
    *(clags_config_t **)flag->variable = config;
  } break;
  case Clags_CountFlag: {
    if (flag->variable == nullptr)
      return;
    size_t count = *(size_t *)flag->variable;
    clags_assert(!clags__checked_add_size(&count, count, (size_t)1),
                 "Count flag overflow!");
    *(size_t *)flag->variable = count;
  } break;
  case Clags_CallbackFlag: {
    if (flag->callback != nullptr)
      flag->callback(config);
  } break;
  default: {
    clags_unreachable("Invalid clags_flag_type_t");
  }
  }
}

bool clags__validate_positional(clags_config_t *config,
                                clags_positional_t pos) {
  if (!clags__is_valid_value_type(pos.value_type)) {
    clags_log(config, Clags_ConfigError,
              "invalid value type for positional argument '%s': %d!",
              pos.arg_name, (int)pos.value_type);
    return false;
  }
  switch (pos.value_type) {
  case Clags_Subcmd: {
    if (pos.subcmds == nullptr) {
      clags_log(config, Clags_ConfigError,
                "incomplete subcommand definition for argument '%s'! Define "
                "`.subcmds` for subcommand verification!",
                pos.arg_name);
      return false;
    }
  } break;
  case Clags_Choice: {
    if (pos.choices == nullptr) {
      clags_log(config, Clags_ConfigError,
                "incomplete choice definition for argument '%s'! Define "
                "`.choices` for choice verification!",
                pos.arg_name);
      return false;
    }
  } break;
  case Clags_Custom: {
    if (pos.verify == nullptr) {
      clags_log(config, Clags_ConfigError,
                "incomplete custom verifier definition for argument '%s'! "
                "Define `.verify` for custom verification!",
                pos.arg_name);
      return false;
    }
  } break;
  default:
    break;
  }
  return true;
}

bool clags__validate_option(clags_config_t *config, clags_option_t opt) {
  if (!clags__is_valid_value_type(opt.value_type)) {
    char short_flag_name[2] = {(char)opt.short_flag, '\0'};
    clags_log(config, Clags_ConfigError,
              "invalid value type for option argument '%s': %d!",
              opt.long_flag ? opt.long_flag
                            : (opt.short_flag ? short_flag_name : "unknown"),
              (int)opt.value_type);
    return false;
  }
  char buf[3] = {'-', '\0', '\0'};
  const char *name =
      opt.long_flag
          ? opt.long_flag
          : (opt.short_flag ? (buf[1] = opt.short_flag, buf) : "(unnamed)");
  if (opt.short_flag == '\0' && opt.long_flag == nullptr) {
    clags_log(config, Clags_ConfigWarning,
              "option argument is unreachable. Define at least one of "
              "`short_flag` and `long_flag`.");
  }
  if (opt.long_flag && strncmp(opt.long_flag, "--", 2) == 0) {
    clags_log(config, Clags_ConfigWarning,
              "option long flag '%s' should not start with '--'. "
              "The parser automatically handles leading '--' for long flags, "
              "so including it in the config may cause incorrect parsing.",
              opt.long_flag);
  }
  switch (opt.value_type) {
  case Clags_Subcmd: {
    clags_log(config, Clags_ConfigError,
              "option argument '%s' may not be a subcommand!", name);
    return false;
  } break;
  case Clags_Choice: {
    if (opt.choices == nullptr) {
      clags_log(config, Clags_ConfigError,
                "incomplete choice definition for argument '%s'! Define "
                "`.choices` for choice verification!",
                name);
      return false;
    }
  } break;
  case Clags_Custom: {
    if (opt.verify == nullptr) {
      clags_log(config, Clags_ConfigError,
                "incomplete custom verifier definition for argument '%s'! "
                "Define `.verify` for custom verification!",
                name);
      return false;
    }
  } break;
  default:
    break;
  }
  return true;
}

bool clags__validate_flag(clags_config_t *config, clags_flag_t flag) {
  if (flag.short_flag == '\0' && flag.long_flag == nullptr) {
    clags_log(config, Clags_ConfigWarning,
              "flag argument is unreachable. Define at least one of "
              "`short_flag` and `long_flag`.");
  }
  if (flag.long_flag && strncmp(flag.long_flag, "--", 2) == 0) {
    clags_log(config, Clags_ConfigWarning,
              "long flag '%s' should not start with '--'. "
              "The parser automatically handles leading '--' for long flags, "
              "so including it in the config may cause incorrect parsing.",
              flag.long_flag);
  }
  switch (flag.type) {
  case Clags_BoolFlag:
  case Clags_ConfigFlag:
  case Clags_CountFlag:
    break;
  case Clags_CallbackFlag: {
    if (flag.callback == nullptr) {
      clags_log(config, Clags_ConfigError,
                "callback flag requires `.callback` to be set.");
      return false;
    }
  } break;
  default: {
    clags_log(config, Clags_ConfigError, "invalid flag type: %d!", flag.type);
    return false;
  }
  }
  return true;
}

bool clags__validate_config(clags_config_t *config) {
  bool result = true;
  // validate options
  if (clags__is_empty_string(config->options.list_terminator)) {
    clags_log(config, Clags_ConfigError,
              "'.list_terminator' may not be empty.");
    clags_return_defer(false);
  }
  if (clags__is_empty_string(config->options.ignore_prefix)) {
    clags_log(config, Clags_ConfigError, "'.ignore_prefix' may not be empty.");
    clags_return_defer(false);
  }
  if (config->options.list_terminator &&
      strcmp(config->options.list_terminator, "--") == 0) {
    clags_log(config, Clags_ConfigError,
              "'.list_terminator' may not be '--' because '--' is reserved for "
              "toggling option and flag parsing!");
    clags_return_defer(false);
  }
  if (config->options.ignore_prefix &&
      strcmp(config->options.ignore_prefix, "--") == 0) {
    clags_log(config, Clags_ConfigError,
              "'.ignore_prefix' may not be '--' since this conflicts with the "
              "long option and flag prefix!");
    clags_return_defer(false);
  }
  if (config->options.list_terminator != nullptr &&
      config->options.ignore_prefix != nullptr &&
      strcmp(config->options.list_terminator, config->options.ignore_prefix) ==
          0) {
    clags_log(config, Clags_ConfigError,
              "'.list_terminator' and '.ignore_prefix' may not be identical.");
    clags_return_defer(false);
  }

  // validate args

  bool last_was_list = false;
  bool subcmd_found = false;
  bool optional_found = false;
  const char *last_pos_name = nullptr;
  for (size_t i = 0; i < config->args_count; ++i) {
    switch (config->args[i].type) {
    case Clags_Positional: {
      clags_positional_t pos = config->args[i].pos;
      if (!clags__validate_positional(config, pos))
        clags_return_defer(false);
      if (optional_found && !pos.optional) {
        clags_log(config, Clags_ConfigError,
                  "invalid positional argument order: required argument '%s' "
                  "appears after optional argument '%s'",
                  pos.arg_name, last_pos_name);
        clags_return_defer(false);
      }
      optional_found = pos.optional;
      if (pos.value_type == Clags_Subcmd) {
        subcmd_found = true;
        if (last_pos_name != nullptr) {
          clags_log(config, Clags_ConfigError,
                    "subcommand '%s' must be the only positional argument in "
                    "its config!",
                    pos.arg_name);
          clags_return_defer(false);
        }
      } else if (subcmd_found) {
        clags_log(config, Clags_ConfigError,
                  "trailing positional argument after subcommand: '%s'!",
                  pos.arg_name);
        clags_return_defer(false);
      }
      if (last_was_list && config->options.list_terminator == nullptr) {
        clags_sb_t sb = {0};
        clags_sb_appendf(
            &sb,
            "positional argument '%s' is unreachable after list '%s'! Define "
            "'.list_terminator' in 'clags_config' to separate them",
            pos.arg_name, last_pos_name);
        if (!pos.is_list) {
          clags_sb_appendf(&sb, " or make '%s' option", pos.arg_name);
        }
        clags_sb_appendf(&sb, ".");
        clags_log_sb(config, Clags_ConfigError, &sb);
        clags_sb_free(&sb);
        clags_return_defer(false);
      }
      last_was_list = pos.is_list;
      last_pos_name = pos.arg_name;
    } break;
    case Clags_Option: {
      last_was_list = false;
      if (!clags__validate_option(config, config->args[i].opt))
        clags_return_defer(false);
    } break;
    case Clags_Flag: {
      last_was_list = false;
      if (!clags__validate_flag(config, config->args[i].flag))
        clags_return_defer(false);
    } break;
    }
  }
defer:
  if (!result)
    config->error = Clags_Error_InvalidConfig;
  return result;
}

void clags__sort_args(clags_args_t *args, clags_config_t *config) {
  for (size_t i = 0; i < config->args_count; ++i) {
    switch (config->args[i].type) {
    case Clags_Positional: {
      clags_positional_t pos = config->args[i].pos;
      args->positional[args->positional_count++] = pos;
      if (!pos.optional)
        args->required_count++;
    } break;
    case Clags_Option: {
      args->option[args->option_count++] = config->args[i].opt;
    } break;
    case Clags_Flag: {
      args->flags[args->flag_count++] = config->args[i].flag;
    } break;
    default: {
      clags_unreachable("Invalid clags_arg_type_t");
    }
    }
  }
}

void clags__choice_usage(clags_choices_t *choices, bool is_list) {
  if (!choices->print_no_details || choices->count >= 6) {
    printf(" (%s%s)\n        Choices%s:\n", clags__type_names[Clags_Choice],
           is_list ? "[]" : "",
           choices->case_insensitive ? " (case-insensitive)" : "");
    for (size_t j = 0; j < choices->count; ++j) {
      clags_choice_t choice = choices->items[j];
      printf("          - %*s : %s\n", CLAGS__USAGE_PRINTF_ALIGNMENT + 8,
             choice.value, choice.description);
    }
  } else {
    printf(" (%s%s:", clags__type_names[Clags_Choice], is_list ? "[]" : "");
    for (size_t j = 0; j < choices->count; ++j) {
      printf("%s%s", j > 0 ? " | " : " ", choices->items[j].value);
    }
    printf(")");
  }
}

void clags__subcmd_usage(clags_subcmds_t *subcmds) {
  printf(" (%s)\n      Subcommands:\n", clags__type_names[Clags_Subcmd]);
  for (size_t i = 0; i < subcmds->count; ++i) {
    clags_subcmd_t subcmd = subcmds->items[i];
    printf("        - %*s : %s\n", CLAGS__USAGE_PRINTF_ALIGNMENT + 6,
           subcmd.name, subcmd.description);
  }
}

void clags__type_usage(clags_value_type_t type, void *data, bool is_list) {
  switch (type) {
  case Clags_Choice: {
    clags__choice_usage((clags_choices_t *)data, is_list);
  } break;
  case Clags_Subcmd: {
    clags__subcmd_usage((clags_subcmds_t *)data);
  } break;
  case Clags_String: {
    if (is_list)
      printf(" ([])");
  } break;
  default: {
    printf(" (%s%s)", clags__type_names[type], is_list ? "[]" : "");
  }
  }
  printf("\n");
}

void clags__subcommand_path_usage(const char *program_name,
                                  clags_config_t *config) {
  if (config->parent) {
    clags__subcommand_path_usage(program_name, config->parent);
    printf(" %s", config->name ? config->name : "(subcommand)");
  } else {
    printf("Usage: %s", program_name);
  }
}

[[nodiscard]] static clags_config_t *
clags__parse_internal(size_t argc, char **argv, clags_config_t *config,
                      size_t depth) {
  if (config == nullptr || config->args == nullptr || config->invalid)
    return nullptr;
  if (argv == nullptr || argc == 0) {
    config->error = Clags_Error_InvalidOption;
    return config;
  }
  if (argv[0] == nullptr) {
    clags_log(config, Clags_Error, "Missing program name in parser input");
    config->error = Clags_Error_InvalidOption;
    return config;
  }
  if (depth > CLAGS_MAX_PARSE_DEPTH) {
    clags_log(
        config, Clags_Error,
        "Subcommand nesting too deep: exceeded maximum parser depth of %zu",
        (size_t)CLAGS_MAX_PARSE_DEPTH);
    config->error = Clags_Error_InvalidOption;
    return config;
  }
  // validate the configuration, exit and mark config as invalid on fatal error
  if (!clags__validate_config(config)) {
    config->invalid = true;
    return config;
  }

  config->name = clags_config_duplicate_string(config, argv[0]);
  config->error = Clags_Error_Ok;

  clags_config_t *result = nullptr;

  // sort arguments by type
  clags_positional_t *positional =
      CLAGS_CALLOC(config->args_count, sizeof(*positional));
  clags_option_t *option = CLAGS_CALLOC(config->args_count, sizeof(*option));
  clags_flag_t *flags = CLAGS_CALLOC(config->args_count, sizeof(*flags));
  clags_assert(positional && option && flags, "Out of memory!");

  clags_args_t args = {
      .positional = positional, .option = option, .flags = flags};
  clags__sort_args(&args, config);

  const char *ignore_prefix = config->options.ignore_prefix;
  size_t ignore_prefix_len = ignore_prefix ? strlen(ignore_prefix) : 0;
  const char *list_term = config->options.list_terminator;

  // parse arguments
  bool arguments_ignored = false;
  bool in_list = false;
  bool parsing_optionals = false;
  bool accept_options = true;
  size_t positional_count = 0;
  size_t required_count = 0;
  for (size_t index = 1; index < (size_t)argc; ++index) {
    char *arg = argv[index];
    if (arg == nullptr) {
      clags_log(config, Clags_Error, "Invalid null argument at position %zu!",
                index);
      config->error = Clags_Error_InvalidOption;
      clags_return_defer(config);
    }

    // toggle option and flag parsing based on '--'
    if (strcmp(arg, "--") == 0) {
      if (accept_options || config->options.allow_option_parsing_toggle) {
        accept_options = !accept_options;
        continue;
      }
    }

    // ignore arguments prefixed with `ignore_prefix`
    if (ignore_prefix && strncmp(arg, ignore_prefix, ignore_prefix_len) == 0) {
      arguments_ignored = true;
      continue;
    }

    // detect list terminator
    if (list_term && strcmp(arg, list_term) == 0) {
      if (in_list) {
        in_list = false;
        positional_count += 1;
        if (!parsing_optionals)
          required_count += 1;
      }
      continue;
    }
    if (accept_options && strncmp(arg, "--", 2) == 0) {
      // parse long flag or option
      arg += 2;
      if (*arg == '\0') {
        clags_log(config, Clags_Error, "Missing flag or option name: '--%s'!",
                  arg);
        config->error = Clags_Error_InvalidOption;
        clags_return_defer(config);
      }

      // parse long option
      for (size_t i = 0; i < args.option_count; ++i) {
        clags_option_t opt = args.option[i];
        if (opt.long_flag == nullptr)
          continue;
        size_t long_flag_len = strlen(opt.long_flag);
        if (strncmp(arg, opt.long_flag, long_flag_len) == 0) {
          char *value = arg + long_flag_len;
          if (*value == '\0') {
            // get value from the next not-ignored argument
            while (true) {
              if (argc - index <= 1) {
                clags_log(config, Clags_Error,
                          "Option flag %s requires argument!", arg);
                config->error = Clags_Error_InvalidOption;
                clags_return_defer(config);
              }
              value = argv[++index];
              if (value == nullptr) {
                clags_log(config, Clags_Error,
                          "Invalid null argument at position %zu!", index);
                config->error = Clags_Error_InvalidOption;
                clags_return_defer(config);
              }
              if (!ignore_prefix ||
                  strncmp(value, ignore_prefix, ignore_prefix_len) != 0)
                break;
              arguments_ignored = true;
            }
          } else if (*value++ == '=') {
            if (*value == '\0') {
              clags_log(config, Clags_Error,
                        "Designated option assignment may not have an empty "
                        "value: '%s'!",
                        arg);
              config->error = Clags_Error_InvalidOption;
              clags_return_defer(config);
            }
          } else {
            continue;
          }
          clags_custom_verify_func_t verify =
              opt.value_type == Clags_Custom ? opt.verify : nullptr;
          if (!clags__set_arg(config, opt.value_type, arg, value, opt.variable,
                              opt._data, verify, opt.is_list))
            clags_return_defer(config);
          goto next;
        }
      }
      // parse long flags
      for (size_t i = 0; i < args.flag_count; ++i) {
        clags_flag_t flag = args.flags[i];
        if (flag.long_flag && strcmp(arg, flag.long_flag) == 0) {
          clags__set_flag(config, &flag);
          if (flag.exit)
            clags_return_defer(nullptr);
          goto next;
        }
      }
      clags_log(config, Clags_Error, "Unknown long flag or option: '--%s'!",
                arg);
      config->error = Clags_Error_InvalidOption;
      clags_return_defer(config);
    } else if (accept_options && *arg == '-' &&
               !isdigit((unsigned char)arg[1])) {
      // parse short flag or option
      arg += 1;
      size_t flag_len = strlen(arg);
      if (flag_len == 0) {
        clags_log(config, Clags_Error, "Missing flag or option name: '-'!");
        config->error = Clags_Error_InvalidOption;
        clags_return_defer(config);
      }
      for (char *c = arg; c < arg + flag_len; ++c) {
        // check for short option
        for (size_t i = 0; i < args.option_count; ++i) {
          clags_option_t opt = args.option[i];
          if (*c == opt.short_flag) {
            char *value = c + 1;
            if (*value == '\0') {
              while (true) {
                if (argc - index <= 1) {
                  clags_log(config, Clags_Error,
                            "Option flag %s requires argument!", arg);
                  config->error = Clags_Error_InvalidOption;
                  clags_return_defer(config);
                }
                value = argv[++index];
                if (value == nullptr) {
                  clags_log(config, Clags_Error,
                            "Invalid null argument at position %zu!", index);
                  config->error = Clags_Error_InvalidOption;
                  clags_return_defer(config);
                }
                if (!ignore_prefix ||
                    strncmp(value, ignore_prefix, ignore_prefix_len) != 0)
                  break;
                arguments_ignored = true;
              }
            }
            clags_custom_verify_func_t verify =
                opt.value_type == Clags_Custom ? opt.verify : nullptr;
            if (!clags__set_arg(config, opt.value_type, arg, value,
                                opt.variable, opt._data, verify, opt.is_list))
              clags_return_defer(config);
            goto next;
          }
        }
        bool matched = false;
        for (size_t i = 0; i < args.flag_count; ++i) {
          clags_flag_t flag = args.flags[i];
          if (*c == flag.short_flag) {
            clags__set_flag(config, &flag);
            if (flag.exit)
              clags_return_defer(nullptr);
            matched = true;
          }
        }
        if (!matched) {
          if (flag_len > 1) {
            clags_log(config, Clags_Error,
                      "Unknown short flag '-%c' in combination '-%s'!", *c,
                      arg);
          } else {
            clags_log(config, Clags_Error, "Unknown short flag '-%c'!", *c);
          }
          config->error = Clags_Error_InvalidOption;
          clags_return_defer(config);
        }
      }
    } else {
      // parse positional argument
      if (positional_count >= args.positional_count) {
        clags_log(config, Clags_Error,
                  "Unknown additional argument (%zu/%zu): '%s'!",
                  positional_count + 1, args.positional_count, arg);
        config->error = Clags_Error_TooManyArguments;
        clags_return_defer(config);
      }

      // verify and write argument
      clags_positional_t pos = args.positional[positional_count];

      // parse subcommands
      if (pos.value_type == Clags_Subcmd) {
        clags_subcmd_t **subcmd = pos.variable;
        if (!clags__verify_funcs[pos.value_type](config, pos.arg_name, arg,
                                                 subcmd, pos.subcmds))
          clags_return_defer(config);
        if (subcmd == nullptr)
          clags_return_defer(nullptr);
        clags_config_t *child_config = (*subcmd)->config;
        if (child_config != nullptr) {
          if (clags__has_config_cycle(config, child_config)) {
            clags_log(config, Clags_Error,
                      "Cycle detected while selecting subcommand '%s'!", arg);
            config->error = Clags_Error_InvalidOption;
            clags_return_defer(config);
          }
          child_config->parent = config;
        }
        clags_return_defer(clags__parse_internal(
            (size_t)argc - index, argv + index, child_config, depth + 1));
      }
      if (pos.is_list) {
        in_list = true;
      } else {
        positional_count += 1;
        if (!pos.optional)
          required_count += 1;
      }
      parsing_optionals = pos.optional;
      clags_custom_verify_func_t verify =
          pos.value_type == Clags_Custom ? pos.verify : nullptr;
      if (!clags__set_arg(config, pos.value_type, pos.arg_name, arg,
                          pos.variable, pos._data, verify, pos.is_list))
        clags_return_defer(config);
    }
  next:
    continue;
  }
  if (in_list) {
    positional_count += 1;
    if (!parsing_optionals)
      required_count += 1;
  }
  if (arguments_ignored)
    clags_log(config, Clags_Warning,
              "Arguments were ignored because they were prefixed with '%s'",
              ignore_prefix);

  // report missing positional arguments
  if (required_count < args.required_count) {
    clags_sb_t sb = {0};
    clags_sb_appendf(&sb,
                     "Missing required arguments (%zu/%zu):", required_count,
                     args.required_count);
    for (size_t i = positional_count; i < args.required_count; ++i) {
      clags_sb_appendf(&sb, " <%s>", args.positional[i].arg_name);
    }
    clags_sb_appendf(&sb, "!");
    clags_log_sb(config, Clags_Error, &sb);
    clags_sb_free(&sb);

    config->error = Clags_Error_TooFewArguments;
    clags_return_defer(config);
  }

  clags_return_defer(nullptr);

defer:
  // cleanup memory of sorted args
  CLAGS_FREE(positional);
  CLAGS_FREE(option);
  CLAGS_FREE(flags);
  return result;
}

[[nodiscard]] clags_config_t *clags_parse(int argc, char **argv,
                                          clags_config_t *config) {
  if (argc <= 0 || config == nullptr || argv == nullptr) {
    if (config != nullptr) {
      config->error = Clags_Error_InvalidOption;
    }
    return config;
  }
  return clags__parse_internal((size_t)argc, argv, config, 1);
}

static void clags__format_lhs(char *buffer, size_t buf_size, char short_flag,
                              const char *long_flag, const char *arg_name,
                              bool *lines_cut_off) {
  if (!buffer || buf_size == 0)
    return;
  buffer[0] = '\0';

  char temp[512] = {0};
  size_t needed = 0;

  if (short_flag && long_flag) {
    if (arg_name)
      snprintf(temp, sizeof(temp), "-%c, --%s(=)%s", short_flag, long_flag,
               arg_name);
    else
      snprintf(temp, sizeof(temp), "-%c, --%s", short_flag, long_flag);
  } else if (short_flag) {
    if (arg_name)
      snprintf(temp, sizeof(temp), "-%c %s", short_flag, arg_name);
    else
      snprintf(temp, sizeof(temp), "-%c", short_flag);
  } else if (long_flag) {
    if (arg_name)
      snprintf(temp, sizeof(temp), "--%s(=)%s", long_flag, arg_name);
    else
      snprintf(temp, sizeof(temp), "--%s", long_flag);
  }

  needed = strlen(temp);

  if (needed < buf_size) {
    strncpy(buffer, temp, buf_size);
    buffer[buf_size - 1] = '\0';
    return;
  }

  if (!long_flag) {
    strncpy(buffer, temp, buf_size - 1);
    buffer[buf_size - 1] = '\0';
    if (lines_cut_off)
      *lines_cut_off = true;
    return;
  }

  const char *suffix = arg_name ? arg_name : "";
  size_t suffix_len = strlen(suffix) + 3;
  size_t remaining = buf_size > 1 ? buf_size - 1 : 0;

  char prefix[8] = {'-', '-', '\0', '\0', '\0', '\0', '\0', '\0'};
  if (short_flag && long_flag) {
    prefix[1] = short_flag;
    prefix[2] = ',';
    prefix[3] = ' ';
    prefix[4] = '-';
    prefix[5] = '-';
    prefix[6] = '\0';
  }

  size_t prefix_len = strlen(prefix);
  size_t max_long = 0;
  if (remaining > prefix_len + suffix_len)
    max_long = remaining - prefix_len - suffix_len;
  else
    max_long = 0;

  if (max_long > 0) {
    const size_t max_trimmed_len = 128 - 1;
    if (max_long > max_trimmed_len) {
      max_long = max_trimmed_len;
    }

    char trimmed_long[128] = {0};
    strncpy(trimmed_long, long_flag, max_long);
    if (max_long >= 2) {
      trimmed_long[max_long - 2] = '.';
      trimmed_long[max_long - 1] = '.';
    } else if (max_long == 1) {
      trimmed_long[0] = '.';
    }

    snprintf(buffer, buf_size, "%s%s%s%s", prefix, trimmed_long,
             arg_name ? "(=)" : "", suffix);
  } else {
    snprintf(buffer, buf_size, "%s%s", prefix, arg_name ? arg_name : "");
  }

  if (lines_cut_off)
    *lines_cut_off = true;
}

void clags_usage(const char *program_name, clags_config_t *config) {
  if (!config || !config->args || config->invalid)
    return;

  clags_positional_t *positional =
      CLAGS_CALLOC(config->args_count, sizeof(*positional));
  clags_option_t *option = CLAGS_CALLOC(config->args_count, sizeof(*option));
  clags_flag_t *flags = CLAGS_CALLOC(config->args_count, sizeof(*flags));
  clags_assert(positional && option && flags, "Out of memory!");

  clags_args_t args = {
      .positional = positional, .option = option, .flags = flags};
  clags__sort_args(&args, config);

  char *temp_buffer =
      CLAGS_CALLOC(CLAGS__USAGE_TEMP_BUFFER_SIZE, sizeof(*temp_buffer));
  clags_assert(temp_buffer, "Out of memory!");

  bool lines_cut_off = false;

  clags__subcommand_path_usage(program_name, config);

  if (args.option_count)
    printf(" [OPTIONS]");
  if (args.flag_count)
    printf(" [FLAGS]");

  bool last_was_list = false;
  for (size_t i = 0; i < args.positional_count; ++i) {
    if (last_was_list) {
      if (config->options.list_terminator) {
        printf(" %s", config->options.list_terminator);
      }
      last_was_list = false;
    }
    clags_positional_t pos = args.positional[i];
    const char *pos_arg_name = pos.arg_name ? pos.arg_name : "(unnamed)";
    printf(" ");
    printf("%c", pos.optional ? '[' : '<');
    if (pos.is_list) {
      printf("%s..", pos_arg_name);
      last_was_list = true;
    } else {
      printf("%s", pos_arg_name);
    }
    printf("%c", pos.optional ? ']' : '>');
  }
  printf("\n");

  if (config->options.description) {
    const char *line = config->options.description;
    while (line && *line) {
      const char *line_end = clags__strchrnull(line, '\n');
      int len = (int)(line_end - line);
      printf("%.*s\n", len, line);
      if (*line_end == '\0')
        break;
      line = line_end + 1;
    }
    printf("\n");
  }

  if (args.positional_count) {
    printf("  Arguments:\n");
    for (size_t i = 0; i < args.positional_count; ++i) {
      clags_positional_t pos = args.positional[i];
      const char *pos_arg_name = pos.arg_name ? pos.arg_name : "(unnamed)";
      const char *pos_description = pos.description ? pos.description : "";
      const char *optional_hint = pos.optional ? " (optional)" : "";
      printf("    %*s%s : %s", CLAGS__USAGE_PRINTF_ALIGNMENT, pos_arg_name,
             optional_hint, pos_description);
      clags__type_usage(pos.value_type, pos._data, pos.is_list);
    }
  }

  if (args.option_count) {
    printf("  Options:\n");
    for (size_t i = 0; i < args.option_count; ++i) {
      clags_option_t opt = args.option[i];
      const char *opt_description = opt.description ? opt.description : "";
      clags__format_lhs(temp_buffer, CLAGS__USAGE_TEMP_BUFFER_SIZE,
                        opt.short_flag, opt.long_flag, opt.arg_name,
                        &lines_cut_off);
      printf("    %*s : %s", CLAGS__USAGE_PRINTF_ALIGNMENT, temp_buffer,
             opt_description);
      clags__type_usage(opt.value_type, opt._data, opt.is_list);
    }
  }

  if (args.flag_count) {
    printf("  Flags:\n");
    for (size_t i = 0; i < args.flag_count; ++i) {
      clags_flag_t flag = args.flags[i];
      const char *flag_description = flag.description ? flag.description : "";
      clags__format_lhs(temp_buffer, CLAGS__USAGE_TEMP_BUFFER_SIZE,
                        flag.short_flag, flag.long_flag, nullptr,
                        &lines_cut_off);
      printf("    %*s : %s%s\n", CLAGS__USAGE_PRINTF_ALIGNMENT, temp_buffer,
             flag_description, flag.exit ? " and exit" : "");
    }
  }

  if (!config->options.print_no_notes &&
      (config->options.list_terminator || config->options.ignore_prefix ||
       config->options.allow_option_parsing_toggle)) {
    printf("\n  Notes:\n");
    if (config->options.allow_option_parsing_toggle) {
      printf("    '--' toggles option and flag parsing and can re-enable "
             "parsing when provided again.\n");
    }
    if (config->options.list_terminator) {
      printf("    '%s' terminates a list argument.\n",
             config->options.list_terminator);
    }
    if (config->options.ignore_prefix) {
      printf("    Arguments prefixed with '%s' are ignored.\n",
             config->options.ignore_prefix);
    }
  }

  if (lines_cut_off) {
    clags_log(config, Clags_ConfigWarning,
              "Some flag names were too long and were cut off! Increase "
              "`CLAGS_USAGE_ALIGNMENT` to give them more space.");
  }

  CLAGS_FREE(positional);
  CLAGS_FREE(option);
  CLAGS_FREE(flags);
  CLAGS_FREE(temp_buffer);
}

[[nodiscard]] int clags_subcmd_index(clags_subcmds_t *subcmds,
                                     clags_subcmd_t *subcmd) {
  if (!subcmds || !subcmd)
    return -1;
  for (size_t i = 0; i < subcmds->count; ++i) {
    if (&subcmds->items[i] == subcmd)
      return (int)i;
  }
  return -1;
}

[[nodiscard]] int clags_choice_index(clags_choices_t *choices,
                                     clags_choice_t *choice) {
  if (!choices || !choice)
    return -1;
  for (size_t i = 0; i < choices->count; ++i) {
    if (&choices->items[i] == choice)
      return (int)i;
  }
  return -1;
}

void clags_list_free(clags_list_t *list) {
  if (list == nullptr)
    return;
  CLAGS_FREE(list->items);
  list->items = nullptr;
  list->count = list->capacity = 0;
}

void clags_config_free_allocs(clags_config_t *config) {
  if (config == nullptr)
    return;
  clags_list_t *allocs = &config->allocs;
  for (size_t i = 0; i < allocs->count; ++i) {
    CLAGS_FREE(((char **)allocs->items)[i]);
  }
  CLAGS_FREE(allocs->items);
  allocs->items = nullptr;
  allocs->count = allocs->capacity = 0;
  if (config->options.duplicate_strings) {
    config->name = nullptr;
  }
}

void clags_config_free(clags_config_t *config) {
  if (config == nullptr)
    return;
  for (size_t i = 0; i < config->args_count; ++i) {
    clags_arg_t arg = config->args[i];
    if (arg.type == Clags_Positional && arg.pos.is_list) {
      clags_list_free(arg.pos.variable);
    } else if (arg.type == Clags_Option && arg.opt.is_list) {
      clags_list_free(arg.opt.variable);
    }
  }
  clags_config_free_allocs(config);
}

[[nodiscard]] const char *clags_error_description(clags_error_t error) {
  switch (error) {
#define X(type, desc)                                                          \
  case type:                                                                   \
    return desc;
    clags__errors
#undef X
        default : return "unknown error";
  }
}
