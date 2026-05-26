#include "core/build_info.h"

#include <string.h>

#include "sc/api.h"
#include "sc/version.h"

#ifndef SC_HAVE_CLAGS
#define SC_HAVE_CLAGS 0
#endif
#ifndef SC_HAVE_JSONRPC
#define SC_HAVE_JSONRPC 0
#endif
#ifndef SC_HAVE_LIBCMARK
#define SC_HAVE_LIBCMARK 0
#endif
#ifndef SC_HAVE_LIBCURL
#define SC_HAVE_LIBCURL 0
#endif
#ifndef SC_HAVE_CURL_HTTP2
#define SC_HAVE_CURL_HTTP2 0
#endif
#ifndef SC_HAVE_CURL_MBEDTLS
#define SC_HAVE_CURL_MBEDTLS 0
#endif
#ifndef SC_HAVE_CURL_WOLFSSL
#define SC_HAVE_CURL_WOLFSSL 0
#endif
#ifndef SC_HAVE_LIBSODIUM
#define SC_HAVE_LIBSODIUM 0
#endif
#ifndef SC_HAVE_ASYNC_HTTP
#define SC_HAVE_ASYNC_HTTP 0
#endif
#ifndef SC_HAVE_NGHTTP2
#define SC_HAVE_NGHTTP2 0
#endif
#ifndef SC_HAVE_MIMALLOC
#define SC_HAVE_MIMALLOC 0
#endif
#ifndef SC_HAVE_JEMALLOC
#define SC_HAVE_JEMALLOC 0
#endif
#ifndef SC_HAVE_NANOCRON
#define SC_HAVE_NANOCRON 0
#endif
#ifndef SC_HAVE_PARSON
#define SC_HAVE_PARSON 0
#endif
#ifndef SC_HAVE_RABBITMQ
#define SC_HAVE_RABBITMQ 0
#endif
#ifndef SC_HAVE_TOML
#define SC_HAVE_TOML 0
#endif
#ifndef SC_HAVE_TP_WEBSOCKET_CLIENT
#define SC_HAVE_TP_WEBSOCKET_CLIENT 0
#endif
#ifndef SC_HAVE_TP_WEBSOCKET_SERVER
#define SC_HAVE_TP_WEBSOCKET_SERVER 0
#endif
#ifndef SC_HAVE_ULOG
#define SC_HAVE_ULOG 0
#endif
#ifndef SC_HAVE_WAMR
#define SC_HAVE_WAMR 0
#endif
#ifndef SC_HAVE_CPYTHON
#define SC_HAVE_CPYTHON 0
#endif
#ifndef SC_HAVE_LINUX_LANDLOCK
#define SC_HAVE_LINUX_LANDLOCK 0
#endif
#ifndef SC_HAVE_BUBBLEWRAP
#define SC_HAVE_BUBBLEWRAP 0
#endif
#ifndef SC_HAVE_FIREJAIL
#define SC_HAVE_FIREJAIL 0
#endif
#ifndef SC_HAVE_DOCKER
#define SC_HAVE_DOCKER 0
#endif
#ifndef SC_HAVE_PODMAN
#define SC_HAVE_PODMAN 0
#endif
#ifndef SC_HAVE_CONTAINER_RUNTIME
#define SC_HAVE_CONTAINER_RUNTIME 0
#endif

typedef struct sc_build_flag {
    const char *name;
    int enabled;
} sc_build_flag;

static const sc_build_flag feature_flags[] = {
    {.name = "SC_BUILD_FULL", .enabled = SC_BUILD_FULL},
    {.name = "SC_ENABLE_RUNTIME", .enabled = SC_ENABLE_RUNTIME},
    {.name = "SC_ENABLE_GATEWAY", .enabled = SC_ENABLE_GATEWAY},
    {.name = "SC_ENABLE_PLUGINS", .enabled = SC_ENABLE_PLUGINS},
    {.name = "SC_ENABLE_WASM_PLUGINS", .enabled = SC_ENABLE_WASM_PLUGINS},
    {.name = "SC_ENABLE_PYTHON_PLUGINS", .enabled = SC_ENABLE_PYTHON_PLUGINS},
    {.name = "SC_ENABLE_HARDWARE", .enabled = SC_ENABLE_HARDWARE},
    {.name = "SC_ENABLE_POSTGRES", .enabled = SC_ENABLE_POSTGRES},
    {.name = "SC_ENABLE_VOICE", .enabled = SC_ENABLE_VOICE},
    {.name = "SC_ENABLE_AUDIO_PREPROCESSING", .enabled = SC_ENABLE_AUDIO_PREPROCESSING},
    {.name = "SC_ENABLE_CURL_HTTP2", .enabled = SC_ENABLE_CURL_HTTP2},
    {.name = "SC_ENABLE_MIMALLOC", .enabled = SC_ENABLE_MIMALLOC},
    {.name = "SC_ENABLE_JEMALLOC", .enabled = SC_ENABLE_JEMALLOC},
    {.name = "SC_PROVIDER_OPENAI", .enabled = SC_PROVIDER_OPENAI},
    {.name = "SC_PROVIDER_ANTHROPIC", .enabled = SC_PROVIDER_ANTHROPIC},
    {.name = "SC_PROVIDER_GEMINI", .enabled = SC_PROVIDER_GEMINI},
    {.name = "SC_PROVIDER_OLLAMA", .enabled = SC_PROVIDER_OLLAMA},
    {.name = "SC_PROVIDER_BEDROCK", .enabled = SC_PROVIDER_BEDROCK},
    {.name = "SC_PROVIDER_OPENAI_COMPATIBLE", .enabled = SC_PROVIDER_OPENAI_COMPATIBLE},
    {.name = "SC_PROVIDER_PROCESS_ADAPTERS", .enabled = SC_PROVIDER_PROCESS_ADAPTERS},
    {.name = "SC_CHANNEL_TELEGRAM", .enabled = SC_CHANNEL_TELEGRAM},
    {.name = "SC_CHANNEL_DISCORD", .enabled = SC_CHANNEL_DISCORD},
    {.name = "SC_CHANNEL_WEBHOOK", .enabled = SC_CHANNEL_WEBHOOK},
    {.name = "SC_CHANNEL_RABBITMQ", .enabled = SC_CHANNEL_RABBITMQ},
    {.name = "SC_CHANNEL_MAIL", .enabled = SC_CHANNEL_MAIL},
    {.name = "SC_SANITIZERS", .enabled = SC_SANITIZERS},
    {.name = "SC_HARDENING", .enabled = SC_HARDENING},
    {.name = "SC_ENABLE_THIRD_PARTY_DEPS", .enabled = SC_ENABLE_THIRD_PARTY_DEPS},
    {.name = "SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT", .enabled = SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT},
    {.name = "SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER", .enabled = SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER},
    {.name = "SC_REQUIRE_SQLITE_FTS5", .enabled = SC_REQUIRE_SQLITE_FTS5},
};

static const sc_build_flag dependency_capabilities[] = {
    {.name = "SC_HAVE_CLAGS", .enabled = SC_HAVE_CLAGS},
    {.name = "SC_HAVE_JSONRPC", .enabled = SC_HAVE_JSONRPC},
    {.name = "SC_HAVE_LIBCMARK", .enabled = SC_HAVE_LIBCMARK},
    {.name = "SC_HAVE_LIBCURL", .enabled = SC_HAVE_LIBCURL},
    {.name = "SC_HAVE_CURL_HTTP2", .enabled = SC_HAVE_CURL_HTTP2},
    {.name = "SC_HAVE_CURL_MBEDTLS", .enabled = SC_HAVE_CURL_MBEDTLS},
    {.name = "SC_HAVE_CURL_WOLFSSL", .enabled = SC_HAVE_CURL_WOLFSSL},
    {.name = "SC_HAVE_LIBSODIUM", .enabled = SC_HAVE_LIBSODIUM},
    {.name = "SC_HAVE_ASYNC_HTTP", .enabled = SC_HAVE_ASYNC_HTTP},
    {.name = "SC_HAVE_NGHTTP2", .enabled = SC_HAVE_NGHTTP2},
    {.name = "SC_HAVE_MIMALLOC", .enabled = SC_HAVE_MIMALLOC},
    {.name = "SC_HAVE_JEMALLOC", .enabled = SC_HAVE_JEMALLOC},
    {.name = "SC_HAVE_NANOCRON", .enabled = SC_HAVE_NANOCRON},
    {.name = "SC_HAVE_PARSON", .enabled = SC_HAVE_PARSON},
    {.name = "SC_HAVE_RABBITMQ", .enabled = SC_HAVE_RABBITMQ},
    {.name = "SC_HAVE_TOML", .enabled = SC_HAVE_TOML},
    {.name = "SC_HAVE_TP_WEBSOCKET_CLIENT", .enabled = SC_HAVE_TP_WEBSOCKET_CLIENT},
    {.name = "SC_HAVE_TP_WEBSOCKET_SERVER", .enabled = SC_HAVE_TP_WEBSOCKET_SERVER},
    {.name = "SC_HAVE_ULOG", .enabled = SC_HAVE_ULOG},
    {.name = "SC_HAVE_WAMR", .enabled = SC_HAVE_WAMR},
    {.name = "SC_HAVE_CPYTHON", .enabled = SC_HAVE_CPYTHON},
    {.name = "SC_HAVE_LINUX_LANDLOCK", .enabled = SC_HAVE_LINUX_LANDLOCK},
    {.name = "SC_HAVE_BUBBLEWRAP", .enabled = SC_HAVE_BUBBLEWRAP},
    {.name = "SC_HAVE_FIREJAIL", .enabled = SC_HAVE_FIREJAIL},
    {.name = "SC_HAVE_DOCKER", .enabled = SC_HAVE_DOCKER},
    {.name = "SC_HAVE_PODMAN", .enabled = SC_HAVE_PODMAN},
    {.name = "SC_HAVE_CONTAINER_RUNTIME", .enabled = SC_HAVE_CONTAINER_RUNTIME},
};

const char *sc_build_version(void)
{
    return SC_VERSION;
}

unsigned int sc_build_abi_version(void)
{
    return SC_ABI_VERSION;
}

int sc_build_feature_enabled(const char *name)
{
    if (name == nullptr) {
        return 0;
    }

    for (size_t i = 0; i < SC_ARRAY_LEN(feature_flags); i += 1) {
        if (strcmp(name, feature_flags[i].name) == 0) {
            return feature_flags[i].enabled;
        }
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(dependency_capabilities); i += 1) {
        if (strcmp(name, dependency_capabilities[i].name) == 0) {
            return dependency_capabilities[i].enabled;
        }
    }

    return 0;
}

void sc_build_features_write(FILE *stream)
{
    if (stream == nullptr) {
        return;
    }

    for (size_t i = 0; i < SC_ARRAY_LEN(feature_flags); i += 1) {
        (void)fprintf(stream, "%s=%d\n", feature_flags[i].name, feature_flags[i].enabled);
    }
}

void sc_build_capabilities_write(FILE *stream)
{
    if (stream == nullptr) {
        return;
    }

    for (size_t i = 0; i < SC_ARRAY_LEN(dependency_capabilities); i += 1) {
        (void)fprintf(stream, "  %s: %s\n", dependency_capabilities[i].name, dependency_capabilities[i].enabled != 0 ? "enabled" : "disabled");
    }
}
