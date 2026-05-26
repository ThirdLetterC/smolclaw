#pragma once

#include "sc/allocator.h"
#include "sc/contract.h"
#include "sc/result.h"
#include "sc/string.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: requests borrow operation and subject strings. Decisions
 * are caller-owned. The handle owns impl and calls destroy exactly once. The
 * wrapper does not synchronize checks; sandbox backends document their own
 * thread-safety.
 */
typedef struct sc_sandbox sc_sandbox;

typedef enum sc_sandbox_backend {
    SC_SANDBOX_BACKEND_AUTO = 0,
    SC_SANDBOX_BACKEND_LANDLOCK,
    SC_SANDBOX_BACKEND_BUBBLEWRAP,
    SC_SANDBOX_BACKEND_FIREJAIL,
    SC_SANDBOX_BACKEND_DOCKER,
    SC_SANDBOX_BACKEND_SEATBELT,
    SC_SANDBOX_BACKEND_APPCONTAINER,
    SC_SANDBOX_BACKEND_PODMAN,
    SC_SANDBOX_BACKEND_CONTAINER,
    SC_SANDBOX_BACKEND_NOOP,
    SC_SANDBOX_BACKEND_UNKNOWN
} sc_sandbox_backend;

typedef enum sc_sandbox_network_policy {
    SC_SANDBOX_NETWORK_FULL = 0,
    SC_SANDBOX_NETWORK_ALLOWED_DOMAINS,
    SC_SANDBOX_NETWORK_NONE
} sc_sandbox_network_policy;

typedef struct sc_sandbox_request {
    size_t struct_size;
    sc_str operation;
    sc_str subject;
    sc_str executable;
    const sc_str *args;
    size_t arg_count;
    sc_str cwd;
    sc_sandbox_network_policy network;
    int64_t memory_limit_mb;
    int64_t max_subprocesses;
    const sc_str *allowed_devices;
    size_t allowed_device_count;
    const sc_str *env_passthrough;
    size_t env_passthrough_count;
    sc_str container_runtime;
    sc_str image_name;
} sc_sandbox_request;

typedef struct sc_sandbox_decision {
    size_t struct_size;
    bool allowed;
    sc_string reason;
    sc_sandbox_backend resolved_backend;
    sc_vec argv;
    bool apply_landlock;
    sc_string fallback_reason;
} sc_sandbox_decision;

typedef enum sc_landlock_fs_access {
    SC_LANDLOCK_FS_EXECUTE = 1u << 0,
    SC_LANDLOCK_FS_WRITE_FILE = 1u << 1,
    SC_LANDLOCK_FS_READ_FILE = 1u << 2,
    SC_LANDLOCK_FS_READ_DIR = 1u << 3,
    SC_LANDLOCK_FS_REMOVE_DIR = 1u << 4,
    SC_LANDLOCK_FS_REMOVE_FILE = 1u << 5,
    SC_LANDLOCK_FS_MAKE_CHAR = 1u << 6,
    SC_LANDLOCK_FS_MAKE_DIR = 1u << 7,
    SC_LANDLOCK_FS_MAKE_REG = 1u << 8,
    SC_LANDLOCK_FS_MAKE_SOCK = 1u << 9,
    SC_LANDLOCK_FS_MAKE_FIFO = 1u << 10,
    SC_LANDLOCK_FS_MAKE_BLOCK = 1u << 11,
    SC_LANDLOCK_FS_MAKE_SYM = 1u << 12,
    SC_LANDLOCK_FS_REFER = 1u << 13,
    SC_LANDLOCK_FS_TRUNCATE = 1u << 14
} sc_landlock_fs_access;

typedef struct sc_landlock_path_rule {
    size_t struct_size;
    sc_str path;
    uint64_t allowed_access;
} sc_landlock_path_rule;

typedef struct sc_landlock_ruleset {
    size_t struct_size;
    uint64_t handled_access;
    const sc_landlock_path_rule *rules;
    size_t rule_count;
} sc_landlock_ruleset;

typedef struct sc_sandbox_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*check)(void *impl,
                       const sc_sandbox_request *request,
                       sc_allocator *alloc,
                       sc_sandbox_decision *out);
    void (*destroy)(void *impl);
} sc_sandbox_vtab;

static inline bool sc_sandbox_handle_is_null(const sc_sandbox *sandbox)
{
    return sandbox == nullptr;
}

bool sc_sandbox_vtab_valid(const sc_sandbox_vtab *vtab);
sc_status sc_sandbox_new(sc_allocator *alloc, const sc_sandbox_vtab *vtab, void *impl, sc_sandbox **out);
sc_status sc_sandbox_check(sc_sandbox *sandbox,
                           const sc_sandbox_request *request,
                           sc_allocator *alloc,
                           sc_sandbox_decision *out);
const sc_sandbox_vtab *sc_sandbox_vtab_of(const sc_sandbox *sandbox);
void sc_sandbox_destroy(sc_sandbox *sandbox);
void sc_sandbox_decision_clear(sc_sandbox_decision *decision);

uint32_t sc_landlock_abi_version();
uint64_t sc_landlock_supported_fs_access();
bool sc_landlock_available();
uint64_t sc_landlock_fs_read_access();
uint64_t sc_landlock_fs_read_write_access();
sc_status sc_landlock_restrict_self(const sc_landlock_ruleset *ruleset);
sc_status sc_sandbox_noop_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_landlock_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_bubblewrap_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_firejail_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_docker_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_podman_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_container_new(sc_allocator *alloc, sc_str runtime, sc_sandbox **out);
sc_status sc_sandbox_seatbelt_new(sc_allocator *alloc, sc_sandbox **out);
sc_status sc_sandbox_appcontainer_new(sc_allocator *alloc, sc_sandbox **out);

SC_END_DECLS
