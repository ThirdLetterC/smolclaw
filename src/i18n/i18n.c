#include "sc/i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

typedef struct sc_i18n_entry {
    sc_string key;
    sc_string message;
} sc_i18n_entry;

static const char default_en_ftl_cli[] =
    "cli.usage = Usage: { $program } [--help] [--version] [--features]\n"
    "cli.commands = Commands:\n"
    "cli.options = Options:\n"
    "cli.option.help = Print help text.\n"
    "cli.option.version = Print version and ABI information.\n"
    "cli.option.features = Print compile-time feature flags.\n"
    "cli.option.hard_shutdown = Skip graceful shutdown drain.\n"
    "cli.option.bind = Bind address for the gateway listener.\n"
    "cli.version = smolclaw-c { $version }\n"
    "cli.abi = abi { $abi }\n"
    "cli.error = smolclaw: { $error }\n"
    "cli.error.with_arg = smolclaw: { $error }: { $arg }\n"
    "cli.error.parse = parse error\n"
    "cli.error.unknown_argument = unknown argument\n"
    "cli.error.unknown_command = unknown command\n"
    "cli.command.registered = smolclaw: command '{ $command }' is registered but not implemented in phase 3\n"
    "cli.chat.env_missing = smolclaw: chat cannot start because required environment variables are not set: { $vars }\n"
    "cli.root.summary = SmolClaw bootstrap CLI.\n"
    "cli.acp.summary = Run the ACP JSON-RPC stdio server.\n"
    "cli.chat.summary = Open an interactive model chat.\n"
    "cli.daemon.summary = Run the configured runtime as a background daemon.\n"
    "cli.init_config.summary = Write a starter configuration file.\n"
    "cli.init_config.created = Wrote starter config to { $path }\n"
    "cli.init_config.exists = Config already exists at { $path }\n"
    "cli.init_config.failed = Failed to write starter config to { $path }\n"
    "cli.onboard.summary = Configure SmolClaw interactively.\n"
    "cli.config.summary = Inspect or edit configuration.\n"
    "cli.config.show.summary = Print effective configuration.\n"
    "cli.config.set.summary = Set a configuration value.\n"
    "cli.provider.summary = Manage providers.\n"
    "cli.memory.summary = Inspect memory backends.\n"
    "cli.cron.summary = Manage scheduled cron jobs.\n"
    "cli.cron.add.summary = Add or replace a cron job.\n"
    "cli.cron.list.summary = List cron jobs.\n"
    "cli.cron.remove.summary = Remove a cron job.\n"
    "cli.gateway.summary = Run or inspect the gateway.\n"
    "cli.gateway.serve.summary = Run the local gateway.\n"
    "cli.service.summary = Manage local service descriptors.\n"
    "cli.service.status.summary = Show service descriptor status.\n"
    "cli.service.dry_run.summary = Preview service installation.\n"
    "cli.service.install.summary = Install the service descriptor.\n"
    "cli.service.uninstall.summary = Remove the service descriptor.\n"
    "cli.service.start.summary = Print the platform start command.\n"
    "cli.service.stop.summary = Print the platform stop command.\n"
    "cli.service.restart.summary = Print the platform restart command.\n"
    "cli.estop.summary = Manage emergency stop state.\n"
    "cli.estop.status.summary = Show emergency stop state.\n"
    "cli.estop.trip.summary = Trip the emergency stop.\n"
    "cli.estop.reset.summary = Reset the emergency stop.\n"
    "cli.doctor.summary = Run diagnostics.\n";

static const char default_en_ftl_tools[] =
    "tool.file_read.description = Read a file inside the configured workspace.\n"
    "tool.file_read.catalog = Filesystem read tool.\n"
    "tool.file_write.description = Write a file inside the configured workspace when policy allows side effects.\n"
    "tool.file_write.catalog = Filesystem write tool.\n"
    "tool.file_list.description = List directory entries inside the configured workspace.\n"
    "tool.file_list.catalog = Filesystem listing tool.\n"
    "tool.memory.description = Manage memory records in the configured backend.\n"
    "tool.memory.catalog = Memory backend tool.\n"
    "tool.shell.description = Run a shell command when shell access is explicitly enabled.\n"
    "tool.shell.catalog = Shell process tool.\n"
    "tool.http.description = Make a bounded HTTP request through the shared network policy.\n"
    "tool.http.catalog = HTTP network tool.\n"
    "tool.web_search.description = Search the web through a configured search provider.\n"
    "tool.web_search.catalog = Web search tool.\n"
    "tool.browser.description = Drive Lightpanda CDP browser automation.\n"
    "tool.browser.catalog = Browser automation tool.\n"
    "tool.browser_screenshot.description = Capture a PNG screenshot through a local CDP browser.\n"
    "tool.browser_screenshot.catalog = Browser screenshot tool.\n"
    "tool.pdf_extract.description = Extract text from a PDF inside the configured workspace.\n"
    "tool.pdf_extract.catalog = PDF text extraction tool.\n"
    "tool.time.description = Read the current time for a timezone.\n"
    "tool.time.catalog = Time utility tool.\n"
    "tool.content_search.description = Search file contents inside the configured workspace.\n"
    "tool.content_search.catalog = Content search tool.\n"
    "tool.glob_search.description = Find files by glob pattern inside the configured workspace.\n"
    "tool.glob_search.catalog = Glob search tool.\n"
    "tool.mcp_server.description = Call a deferred MCP server tool through its configured transport.\n"
    "tool.mcp_server.catalog = MCP server bridge tool.\n"
    "tool.sop_inspect.description = Inspect a SOP markdown file inside the configured workspace.\n"
    "tool.sop_inspect.catalog = SOP inspection tool.\n"
    "tool.sop_advance.description = Advance a SOP run without mutating the source document.\n"
    "tool.sop_advance.catalog = SOP advancement tool.\n"
    "tool.cron_list.description = List configured cron jobs.\n"
    "tool.cron_list.catalog = Cron inspection tool.\n"
    "tool.cron_upsert.description = Create or update a cron job.\n"
    "tool.cron_upsert.catalog = Cron mutation tool.\n"
    "tool.cron_remove.description = Remove a cron job.\n"
    "tool.cron_remove.catalog = Cron mutation tool.\n"
    "tool.resource_usage.description = Report current agent process resource usage and tool execution limits.\n"
    "tool.resource_usage.catalog = Agent resource usage diagnostic.\n"
    "tool.hardware.gpio_read.description = Read a safe GPIO pin alias from a configured hardware device.\n"
    "tool.hardware.gpio_write.description = Write a safe GPIO pin alias on a configured hardware device.\n"
    "gateway.health.ok = Gateway is healthy.\n"
    "onboarding.welcome = Welcome to SmolClaw.\n"
    "error.missing_key = Missing localized message: { $key }\n";

static sc_i18n_entry *find_entry(sc_i18n_catalog *catalog, sc_str key);
static const sc_i18n_entry *find_entry_const(const sc_i18n_catalog *catalog, sc_str key);
static sc_str trim(sc_str text);
static sc_status append_formatted(sc_string_builder *builder,
                                  sc_str message,
                                  const sc_i18n_arg *args,
                                  size_t arg_count);
static const sc_i18n_arg *find_arg(const sc_i18n_arg *args, size_t arg_count, sc_str name);
static bool line_has_bare_user_string(sc_str line);
static bool contains_call_with_string(sc_str line, const char *call);
static bool contains_str(sc_str haystack, const char *needle);
static bool has_prefix(sc_str text, const char *prefix);
static void i18n_entry_clear(sc_i18n_entry *entry);

void sc_i18n_catalog_init(sc_i18n_catalog *catalog, sc_allocator *alloc, sc_str locale)
{
    if (catalog == nullptr) {
        return;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *catalog = (sc_i18n_catalog){.alloc = alloc};
    sc_vec_init(&catalog->entries, alloc, sizeof(sc_i18n_entry));
    (void)sc_string_from_str(alloc, locale.len == 0 ? sc_str_from_cstr("en") : locale, &catalog->locale);
}

sc_status sc_i18n_catalog_add(sc_i18n_catalog *catalog, sc_str key, sc_str message)
{
    sc_i18n_entry entry = {0};
    sc_i18n_entry *existing = nullptr;
    sc_status status;

    if (catalog == nullptr || key.ptr == nullptr || key.len == 0) {
        return sc_status_invalid_argument("sc.i18n.invalid_argument");
    }
    existing = find_entry(catalog, key);
    if (existing != nullptr) {
        sc_string_clear(&existing->message);
        return sc_string_from_str(catalog->alloc, message, &existing->message);
    }
    status = sc_string_from_str(catalog->alloc, key, &entry.key);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(catalog->alloc, message, &entry.message);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&catalog->entries, &entry);
    }
    if (!sc_status_is_ok(status)) {
        i18n_entry_clear(&entry);
    }
    return status;
}

sc_status sc_i18n_catalog_load_ftl(sc_i18n_catalog *catalog, sc_str text)
{
    size_t start = 0;

    if (catalog == nullptr || (text.len > 0 && text.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.i18n.invalid_argument");
    }
    while (start < text.len) {
        size_t end = start;
        sc_str line = {0};
        sc_str key = {0};
        sc_str value = {0};
        size_t eq = SIZE_MAX;
        sc_status status;

        while (end < text.len && text.ptr[end] != '\n') {
            end += 1;
        }
        line = trim(sc_str_from_parts(text.ptr + start, end - start));
        start = end < text.len ? end + 1 : end;
        if (line.len == 0 || line.ptr[0] == '#') {
            continue;
        }
        for (size_t i = 0; i < line.len; i += 1) {
            if (line.ptr[i] == '=') {
                eq = i;
                break;
            }
        }
        if (eq == SIZE_MAX) {
            return sc_status_parse("sc.i18n.ftl.missing_equals");
        }
        key = trim(sc_str_from_parts(line.ptr, eq));
        value = trim(sc_str_from_parts(line.ptr + eq + 1, line.len - eq - 1));
        status = sc_i18n_catalog_add(catalog, key, value);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    return sc_status_ok();
}

sc_status sc_i18n_catalog_load_default_en(sc_i18n_catalog *catalog)
{
    sc_status status;

    if (catalog == nullptr) {
        return sc_status_invalid_argument("sc.i18n.invalid_argument");
    }
    status = sc_i18n_catalog_load_ftl(catalog, sc_str_from_cstr(default_en_ftl_cli));
    if (sc_status_is_ok(status)) {
        status = sc_i18n_catalog_load_ftl(catalog, sc_str_from_cstr(default_en_ftl_tools));
    }
    return status;
}

bool sc_i18n_catalog_has(const sc_i18n_catalog *catalog, sc_str key)
{
    return find_entry_const(catalog, key) != nullptr;
}

sc_status sc_i18n_format(const sc_i18n_catalog *catalog,
                         sc_str key,
                         const sc_i18n_arg *args,
                         size_t arg_count,
                         sc_allocator *alloc,
                         sc_string *out)
{
    const sc_i18n_entry *entry = nullptr;
    sc_string_builder builder = {0};
    sc_status status;

    if (catalog == nullptr || out == nullptr || key.ptr == nullptr || key.len == 0 ||
        (arg_count > 0 && args == nullptr)) {
        return sc_status_invalid_argument("sc.i18n.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    entry = find_entry_const(catalog, key);
    sc_string_builder_init(&builder, alloc);
    if (entry == nullptr) {
        status = sc_string_builder_append_cstr(&builder, "!");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, key);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "!");
        }
    } else {
        status = append_formatted(&builder, sc_string_as_str(&entry->message), args, arg_count);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

sc_status sc_i18n_coverage_report(const sc_i18n_catalog *catalog,
                                  const sc_str *required_keys,
                                  size_t required_key_count,
                                  sc_allocator *alloc,
                                  sc_string *out)
{
    sc_string_builder builder = {0};
    size_t missing = 0;
    sc_status status;

    if (catalog == nullptr || out == nullptr || (required_key_count > 0 && required_keys == nullptr)) {
        return sc_status_invalid_argument("sc.i18n.invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"locale\":\"");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&catalog->locale));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\",\"missing\":[");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < required_key_count; i += 1) {
        if (!sc_i18n_catalog_has(catalog, required_keys[i])) {
            if (missing > 0) {
                status = sc_string_builder_append_cstr(&builder, ",");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\"");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, required_keys[i]);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\"");
            }
            missing += 1;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "],\"missing_count\":");
    }
    if (sc_status_is_ok(status)) {
        char count[32] = {0};
        (void)snprintf(count, sizeof(count), "%zu", missing);
        status = sc_string_builder_append_cstr(&builder, count);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

sc_status sc_i18n_scan_c_source(sc_str source, sc_i18n_scan_result *out)
{
    size_t start = 0;
    size_t count = 0;

    if (out == nullptr || (source.len > 0 && source.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.i18n_scan.invalid_argument");
    }
    while (start < source.len) {
        size_t end = start;
        sc_str line = {0};
        while (end < source.len && source.ptr[end] != '\n') {
            end += 1;
        }
        line = trim(sc_str_from_parts(source.ptr + start, end - start));
        start = end < source.len ? end + 1 : end;
        if (line.len == 0 || has_prefix(line, "//") || has_prefix(line, "/*") ||
            contains_str(line, "sc_log_") || contains_str(line, "sc_i18n_") ||
            contains_str(line, "tracing::")) {
            continue;
        }
        if (line_has_bare_user_string(line)) {
            count += 1;
        }
    }
    *out = (sc_i18n_scan_result){
        .struct_size = sizeof(*out),
        .ok = count == 0,
        .bare_string_count = count,
    };
    return sc_status_ok();
}

void sc_i18n_catalog_clear(sc_i18n_catalog *catalog)
{
    if (catalog == nullptr) {
        return;
    }
    for (size_t i = 0; i < catalog->entries.len; i += 1) {
        sc_i18n_entry *entry = sc_vec_at(&catalog->entries, i);
        i18n_entry_clear(entry);
    }
    sc_vec_clear(&catalog->entries);
    sc_string_clear(&catalog->locale);
    *catalog = (sc_i18n_catalog){0};
}

static sc_i18n_entry *find_entry(sc_i18n_catalog *catalog, sc_str key)
{
    if (catalog == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < catalog->entries.len; i += 1) {
        sc_i18n_entry *entry = sc_vec_at(&catalog->entries, i);
        if (entry != nullptr && sc_str_equal(sc_string_as_str(&entry->key), key)) {
            return entry;
        }
    }
    return nullptr;
}

static const sc_i18n_entry *find_entry_const(const sc_i18n_catalog *catalog, sc_str key)
{
    if (catalog == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < catalog->entries.len; i += 1) {
        const sc_i18n_entry *entry = sc_vec_at_const(&catalog->entries, i);
        if (entry != nullptr && sc_str_equal(sc_string_as_str(&entry->key), key)) {
            return entry;
        }
    }
    return nullptr;
}

static sc_str trim(sc_str text)
{
    size_t start = 0;
    size_t end = text.len;
    while (start < end && isspace((unsigned char)text.ptr[start])) {
        start += 1;
    }
    while (end > start && isspace((unsigned char)text.ptr[end - 1])) {
        end -= 1;
    }
    return sc_str_from_parts(text.ptr + start, end - start);
}

static sc_status append_formatted(sc_string_builder *builder,
                                  sc_str message,
                                  const sc_i18n_arg *args,
                                  size_t arg_count)
{
    size_t start = 0;
    sc_status status = sc_status_ok();

    for (size_t i = 0; sc_status_is_ok(status) && i < message.len; i += 1) {
        if (message.ptr[i] == '{') {
            size_t close = i + 1;
            sc_str token = {0};
            while (close < message.len && message.ptr[close] != '}') {
                close += 1;
            }
            if (close >= message.len) {
                break;
            }
            status = sc_string_builder_append(builder, sc_str_from_parts(message.ptr + start, i - start));
            token = trim(sc_str_from_parts(message.ptr + i + 1, close - i - 1));
            if (token.len > 0 && token.ptr[0] == '$') {
                sc_str name = trim(sc_str_from_parts(token.ptr + 1, token.len - 1));
                const sc_i18n_arg *arg = find_arg(args, arg_count, name);
                if (arg == nullptr) {
                    status = sc_string_builder_append_cstr(builder, "{?");
                    if (sc_status_is_ok(status)) {
                        status = sc_string_builder_append(builder, name);
                    }
                    if (sc_status_is_ok(status)) {
                        status = sc_string_builder_append_cstr(builder, "?}");
                    }
                } else if (arg->secret) {
                    status = sc_string_builder_append_cstr(builder, "[REDACTED]");
                } else {
                    status = sc_string_builder_append(builder, arg->value);
                }
                i = close;
                start = close + 1;
            }
        }
    }
    if (sc_status_is_ok(status) && start < message.len) {
        status = sc_string_builder_append(builder, sc_str_from_parts(message.ptr + start, message.len - start));
    }
    return status;
}

static const sc_i18n_arg *find_arg(const sc_i18n_arg *args, size_t arg_count, sc_str name)
{
    for (size_t i = 0; i < arg_count; i += 1) {
        if (sc_str_equal(args[i].name, name)) {
            return &args[i];
        }
    }
    return nullptr;
}

static bool line_has_bare_user_string(sc_str line)
{
    return contains_call_with_string(line, "printf") ||
           contains_call_with_string(line, "fprintf") ||
           contains_call_with_string(line, "puts") ||
           contains_call_with_string(line, "fputs");
}

static bool contains_call_with_string(sc_str line, const char *call)
{
    size_t call_len = strlen(call);

    if (line.ptr == nullptr || line.len < call_len + 2) {
        return false;
    }
    for (size_t i = 0; i + call_len < line.len; i += 1) {
        if (memcmp(line.ptr + i, call, call_len) == 0) {
            size_t j = i + call_len;
            while (j < line.len && isspace((unsigned char)line.ptr[j])) {
                j += 1;
            }
            if (j < line.len && line.ptr[j] == '(') {
                while (j < line.len && line.ptr[j] != '"') {
                    j += 1;
                }
                return j < line.len && line.ptr[j] == '"';
            }
        }
    }
    return false;
}

static bool contains_str(sc_str haystack, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (haystack.ptr == nullptr || needle_len == 0 || haystack.len < needle_len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= haystack.len; i += 1) {
        if (memcmp(haystack.ptr + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool has_prefix(sc_str text, const char *prefix)
{
    size_t prefix_len = strlen(prefix);
    return text.ptr != nullptr && text.len >= prefix_len && memcmp(text.ptr, prefix, prefix_len) == 0;
}

static void i18n_entry_clear(sc_i18n_entry *entry)
{
    if (entry == nullptr) {
        return;
    }
    sc_string_clear(&entry->key);
    sc_string_clear(&entry->message);
    *entry = (sc_i18n_entry){0};
}
