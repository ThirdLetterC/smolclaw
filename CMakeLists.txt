cmake_minimum_required(VERSION 3.25)

project(SmolClawC
    VERSION 0.1.0
    DESCRIPTION "SmolClaw runtime bootstrap"
    LANGUAGES C CXX
)

if(NOT UNIX)
    message(FATAL_ERROR "SmolClaw requires a Unix-like host.")
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(options)
include(warnings)
include(sanitizers)
include(hardening)
include(deps)
include(CheckCSourceCompiles)

check_c_source_compiles("
    #ifndef _GNU_SOURCE
    #define _GNU_SOURCE
    #endif
    #include <linux/landlock.h>
    #include <sys/syscall.h>
    int main(void) {
        return (int)(SYS_landlock_create_ruleset + SYS_landlock_add_rule + SYS_landlock_restrict_self);
    }
" SC_HAVE_LINUX_LANDLOCK)
find_program(SC_BUBBLEWRAP_EXECUTABLE bwrap)
find_program(SC_FIREJAIL_EXECUTABLE firejail)
find_program(SC_DOCKER_EXECUTABLE docker)
find_program(SC_PODMAN_EXECUTABLE podman)

set(SC_ABI_VERSION_MAJOR 0)
set(SC_ABI_VERSION_MINOR 1)
set(SC_ABI_VERSION_PATCH 0)

sc_bool_to_int(SC_BUILD_FULL SC_BUILD_FULL_VALUE)
sc_bool_to_int(SC_ENABLE_RUNTIME SC_ENABLE_RUNTIME_VALUE)
sc_bool_to_int(SC_ENABLE_GATEWAY SC_ENABLE_GATEWAY_VALUE)
sc_bool_to_int(SC_ENABLE_PLUGINS SC_ENABLE_PLUGINS_VALUE)
sc_bool_to_int(SC_ENABLE_WASM_PLUGINS SC_ENABLE_WASM_PLUGINS_VALUE)
sc_bool_to_int(SC_ENABLE_PYTHON_PLUGINS SC_ENABLE_PYTHON_PLUGINS_VALUE)
sc_bool_to_int(SC_ENABLE_HARDWARE SC_ENABLE_HARDWARE_VALUE)
sc_bool_to_int(SC_ENABLE_POSTGRES SC_ENABLE_POSTGRES_VALUE)
sc_bool_to_int(SC_ENABLE_VOICE SC_ENABLE_VOICE_VALUE)
sc_bool_to_int(SC_ENABLE_AUDIO_PREPROCESSING SC_ENABLE_AUDIO_PREPROCESSING_VALUE)
sc_bool_to_int(SC_ENABLE_CURL_HTTP2 SC_ENABLE_CURL_HTTP2_VALUE)
sc_bool_to_int(SC_ENABLE_MIMALLOC SC_ENABLE_MIMALLOC_VALUE)
sc_bool_to_int(SC_ENABLE_JEMALLOC SC_ENABLE_JEMALLOC_VALUE)
sc_bool_to_int(SC_PROVIDER_OPENAI SC_PROVIDER_OPENAI_VALUE)
sc_bool_to_int(SC_PROVIDER_ANTHROPIC SC_PROVIDER_ANTHROPIC_VALUE)
sc_bool_to_int(SC_PROVIDER_GEMINI SC_PROVIDER_GEMINI_VALUE)
sc_bool_to_int(SC_PROVIDER_OLLAMA SC_PROVIDER_OLLAMA_VALUE)
sc_bool_to_int(SC_PROVIDER_BEDROCK SC_PROVIDER_BEDROCK_VALUE)
sc_bool_to_int(SC_PROVIDER_OPENAI_COMPATIBLE SC_PROVIDER_OPENAI_COMPATIBLE_VALUE)
sc_bool_to_int(SC_PROVIDER_PROCESS_ADAPTERS SC_PROVIDER_PROCESS_ADAPTERS_VALUE)
sc_bool_to_int(SC_CHANNEL_TELEGRAM SC_CHANNEL_TELEGRAM_VALUE)
sc_bool_to_int(SC_CHANNEL_DISCORD SC_CHANNEL_DISCORD_VALUE)
sc_bool_to_int(SC_CHANNEL_WEBHOOK SC_CHANNEL_WEBHOOK_VALUE)
sc_bool_to_int(SC_CHANNEL_RABBITMQ SC_CHANNEL_RABBITMQ_VALUE)
sc_bool_to_int(SC_CHANNEL_MAIL SC_CHANNEL_MAIL_VALUE)
sc_bool_to_int(SC_SANITIZERS SC_SANITIZERS_VALUE)
sc_bool_to_int(SC_HARDENING SC_HARDENING_VALUE)
sc_bool_to_int(SC_ENABLE_THIRD_PARTY_DEPS SC_ENABLE_THIRD_PARTY_DEPS_VALUE)
sc_bool_to_int(SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT_VALUE)
sc_bool_to_int(SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER_VALUE)
sc_bool_to_int(SC_REQUIRE_SQLITE_FTS5 SC_REQUIRE_SQLITE_FTS5_VALUE)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/include/sc/version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/sc/version.h"
    @ONLY
)

sc_add_third_party_dependencies()

add_library(sc_core STATIC
    src/app/cli/cli.c
    src/autonomy/autonomy.c
    src/channels/acp.c
    src/channels/orchestrator.c
    src/channels/telegram.c
    src/channels/webhooks.c
    src/config/config.c
    src/config/sc_config_schema.c
    src/config/config_secret_store.c
    src/config/config_source.c
    src/contracts/contracts.c
    src/contracts/contracts_handle.c
    src/core/allocator.c
    src/core/build_info.c
    src/core/buffer.c
    src/core/map.c
    src/core/result.c
    src/core/string.c
    src/core/time.c
    src/core/vector.c
    src/gateway/gateway.c
    src/hardware/hardware.c
    src/i18n/i18n.c
    src/json/json.c
    src/json/json_schema.c
    src/media/media.c
    src/media/ogg_opus_encoder.c
    src/media/piper_tts.c
    src/media/whisper_transcriber.c
    src/memory/markdown_memory.c
    src/memory/memory_hydrate.c
    src/memory/memory_common.c
    src/memory/none_memory.c
    src/memory/sqlite_memory.c
    src/net/async.c
    src/net/curl_global.c
    src/net/http_client.c
    src/net/url.c
    src/observability/log.c
    src/observability/observer_impls.c
    src/plugins/plugin.c
    src/plugins/plugin_python.c
    src/providers/compatible.c
    src/providers/claude_code.c
    src/providers/gemini_cli.c
    src/providers/http_providers.c
    src/providers/mock_provider.c
    src/providers/provider_common.c
    src/providers/reliable.c
    src/providers/router.c
    src/providers/unsupported.c
    src/registry/registry.c
    src/runtime/agent.c
    src/runtime/bootstrap.c
    src/runtime/channel_policy.c
    src/runtime/loop.c
    src/runtime/turn_queue.c
    src/security/approval.c
    src/security/audit.c
    src/security/domain_matcher.c
    src/security/estop.c
    src/security/policy.c
    src/security/receipts.c
    src/security/sandbox.c
    src/security/sandbox_bubblewrap.c
    src/security/sandbox_container.c
    src/security/sandbox_firejail.c
    src/security/sandbox_landlock.c
    src/security/store.c
    src/security/workspace_boundary.c
    src/tools/autonomy_tools.c
    src/tools/browser_tool.c
    src/tools/diagnostic_tools.c
    src/tools/file_read.c
    src/tools/file_tools.c
    src/tools/http_tool.c
    src/tools/http_tool_async.c
    src/tools/mcp.c
    src/tools/memory_tools.c
    src/tools/process_runner.c
    src/tools/process_tools.c
    src/tools/search_tools.c
    src/tools/shell.c
    src/tools/time_tool.c
    src/tools/tool_common.c
    src/tools/tool_wrappers.c
)
target_include_directories(sc_core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/include/compat"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
)
target_compile_definitions(sc_core PUBLIC _POSIX_C_SOURCE=200809L)
if(APPLE)
    target_compile_definitions(sc_core PUBLIC _DARWIN_C_SOURCE)
endif()
if(SC_HAVE_LINUX_LANDLOCK)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_LINUX_LANDLOCK=1)
endif()
if(SC_BUBBLEWRAP_EXECUTABLE)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_BUBBLEWRAP=1)
endif()
if(SC_FIREJAIL_EXECUTABLE)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_FIREJAIL=1)
endif()
if(SC_DOCKER_EXECUTABLE)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_DOCKER=1)
endif()
if(SC_PODMAN_EXECUTABLE)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_PODMAN=1)
endif()
if(SC_DOCKER_EXECUTABLE OR SC_PODMAN_EXECUTABLE)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_CONTAINER_RUNTIME=1)
endif()
sc_apply_warnings(sc_core)
sc_apply_sanitizers(sc_core)
sc_apply_hardening(sc_core)

if(SC_FUZZING)
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SC_FUZZING requires Clang with libFuzzer support")
    endif()
    target_compile_options(sc_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
    add_executable(sc_fuzz_url tests/fuzz/fuzz_url.c)
    target_link_libraries(sc_fuzz_url PRIVATE sc_core)
    target_compile_options(sc_fuzz_url PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
    target_link_options(sc_fuzz_url PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
if(SC_ENABLE_THIRD_PARTY_DEPS AND TARGET sc_external_deps)
    add_dependencies(sc_core sc_external_deps)
endif()
target_link_libraries(sc_core PRIVATE ${CMAKE_DL_LIBS})
if(TARGET sc_tp_ulog)
    target_link_libraries(sc_core PRIVATE sc_tp_ulog)
    add_dependencies(sc_core sc_tp_ulog)
endif()
if(TARGET sc_tp_toml)
    target_link_libraries(sc_core PRIVATE sc_tp_toml)
    add_dependencies(sc_core sc_tp_toml)
endif()
if(TARGET sc_tp_clags)
    target_link_libraries(sc_core PRIVATE sc_tp_clags)
    add_dependencies(sc_core sc_tp_clags)
endif()
if(TARGET sc_tp_nanocron)
    target_link_libraries(sc_core PRIVATE sc_tp_nanocron)
    add_dependencies(sc_core sc_tp_nanocron)
endif()
if(TARGET sc_tp_microjson)
    target_link_libraries(sc_core PRIVATE sc_tp_microjson)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_MICROJSON=1)
    add_dependencies(sc_core sc_tp_microjson)
endif()
if(TARGET sc_tp_parson)
    target_link_libraries(sc_core PRIVATE sc_tp_parson)
    add_dependencies(sc_core sc_tp_parson)
endif()
if(TARGET sc_tp_jsonrpc)
    target_link_libraries(sc_core PRIVATE sc_tp_jsonrpc)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_JSONRPC=1)
    add_dependencies(sc_core sc_tp_jsonrpc)
endif()
if(TARGET sc_tp_websocket_client)
    target_link_libraries(sc_core PRIVATE sc_tp_websocket_client)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_TP_WEBSOCKET_CLIENT=1)
    add_dependencies(sc_core sc_tp_websocket_client)
endif()
if(TARGET sc_tp_rabbitmq)
    target_link_libraries(sc_core PRIVATE sc_tp_rabbitmq)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_RABBITMQ=1)
    add_dependencies(sc_core sc_tp_rabbitmq)
endif()
if(TARGET SC::libcurl)
    if(SC_LIBCURL_KNOWN_AWS_SIGV4)
        set(SC_HAVE_CURL_AWS_SIGV4 TRUE)
    else()
        set(CMAKE_REQUIRED_LIBRARIES SC::libcurl)
        check_c_source_compiles("
            #include <curl/curl.h>
            int main() {
                CURL *curl = curl_easy_init();
                (void)curl_easy_setopt(curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_AWS_SIGV4);
                (void)curl_easy_setopt(curl, CURLOPT_AWS_SIGV4, \"aws:amz:us-east-1:bedrock\");
                return 0;
            }
        " SC_HAVE_CURL_AWS_SIGV4)
        unset(CMAKE_REQUIRED_LIBRARIES)
    endif()
    target_link_libraries(sc_core PRIVATE SC::libcurl)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_LIBCURL=1)
    if(SC_LIBCURL_TLS_BACKEND STREQUAL "mbedtls")
        target_compile_definitions(sc_core PUBLIC SC_HAVE_CURL_MBEDTLS=1)
    elseif(SC_LIBCURL_TLS_BACKEND STREQUAL "wolfssl")
        target_compile_definitions(sc_core PUBLIC SC_HAVE_CURL_WOLFSSL=1)
    endif()
    if(SC_HAVE_CURL_AWS_SIGV4)
        target_compile_definitions(sc_core PUBLIC SC_HAVE_CURL_AWS_SIGV4=1)
    endif()
endif()
if(TARGET SC::nghttp2)
    target_link_libraries(sc_core PRIVATE SC::nghttp2)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_NGHTTP2=1)
    if(SC_LIBCURL_KNOWN_HTTP2)
        target_compile_definitions(sc_core PUBLIC SC_HAVE_CURL_HTTP2=1)
    endif()
endif()
if(TARGET SC::cmark)
    target_link_libraries(sc_core PRIVATE SC::cmark)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_LIBCMARK=1)
endif()
if(TARGET SC::opus)
    target_link_libraries(sc_core PRIVATE SC::opus)
    target_compile_definitions(sc_core PRIVATE SC_HAVE_OPUS=1)
endif()
if(NOT TARGET SC::libuv)
    message(FATAL_ERROR "libuv is required but no SC::libuv target was configured.")
endif()
target_link_libraries(sc_core PRIVATE SC::libuv)
if(TARGET SC::libcurl)
    target_compile_definitions(sc_core PUBLIC SC_HAVE_ASYNC_HTTP=1)
endif()
if(TARGET SC::libsodium)
    target_link_libraries(sc_core PRIVATE SC::libsodium)
    target_compile_definitions(sc_core PRIVATE SC_HAVE_LIBSODIUM=1)
endif()
if(TARGET SC::sqlite3)
    target_link_libraries(sc_core PRIVATE SC::sqlite3)
    target_compile_definitions(sc_core PRIVATE SC_HAVE_SQLITE=1)
endif()
if(TARGET SC::wamr)
    target_link_libraries(sc_core PRIVATE SC::wamr)
    target_compile_definitions(sc_core PRIVATE SC_HAVE_WAMR=1)
endif()
if(TARGET SC::cpython)
    target_link_libraries(sc_core PRIVATE SC::cpython)
    target_compile_definitions(sc_core PRIVATE SC_HAVE_CPYTHON=1)
endif()
if(SC_ENABLE_MIMALLOC AND SC_MIMALLOC_TARGET)
    target_link_libraries(sc_core PRIVATE "${SC_MIMALLOC_TARGET}")
    target_compile_definitions(sc_core PRIVATE SC_HAVE_MIMALLOC=1)
endif()
if(SC_ENABLE_JEMALLOC AND SC_JEMALLOC_TARGET)
    target_link_libraries(sc_core PRIVATE "${SC_JEMALLOC_TARGET}")
    target_compile_definitions(sc_core PRIVATE SC_HAVE_JEMALLOC=1)
endif()

add_library(sc_test_support STATIC
    tests/support/test_helpers.c
)
target_include_directories(sc_test_support
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
)
target_link_libraries(sc_test_support PUBLIC sc_core)
sc_apply_warnings(sc_test_support)
sc_apply_sanitizers(sc_test_support)
sc_apply_hardening(sc_test_support)

add_executable(smolclaw
    src/app/app_chat.c
    src/app/app_commands.c
    src/app/app_cron.c
    src/app/app_estop.c
    src/app/app_init_config.c
    src/app/app_line_editor.c
    src/app/app_onboard.c
    src/app/app_onboard_auth.c
    src/app/app_provider_command.c
    src/app/app_runtime_commands.c
    src/app/app_service.c
    src/app/main.c
)
target_link_libraries(smolclaw PRIVATE sc_core)
if(TARGET SC::libcurl)
    target_link_libraries(smolclaw PRIVATE SC::libcurl)
endif()
if(TARGET SC::isocline)
    target_link_libraries(smolclaw PRIVATE SC::isocline)
    target_compile_definitions(smolclaw PRIVATE SC_HAVE_ISOCLINE=1)
endif()
if(SC_ENABLE_THIRD_PARTY_DEPS AND TARGET sc_external_deps)
    add_dependencies(smolclaw sc_external_deps)
endif()
sc_apply_warnings(smolclaw)
sc_apply_sanitizers(smolclaw)
sc_apply_hardening(smolclaw)

add_executable(example_custom_provider
    examples/custom_provider.c
)
target_link_libraries(example_custom_provider PRIVATE sc_core)
sc_apply_warnings(example_custom_provider)
sc_apply_sanitizers(example_custom_provider)
sc_apply_hardening(example_custom_provider)

enable_testing()
find_package(Python3 COMPONENTS Interpreter)

add_executable(test_build_info
    tests/unit/test_build_info.c
)
target_link_libraries(test_build_info PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_build_info)
sc_apply_sanitizers(test_build_info)
sc_apply_hardening(test_build_info)

add_executable(test_core
    tests/unit/test_core.c
)
target_link_libraries(test_core PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_core)
sc_apply_sanitizers(test_core)
sc_apply_hardening(test_core)

add_executable(test_http_client
    tests/unit/test_http_client.c
)
target_link_libraries(test_http_client PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_http_client)
sc_apply_sanitizers(test_http_client)
sc_apply_hardening(test_http_client)

add_executable(test_public_headers
    tests/unit/test_public_headers.c
)
target_link_libraries(test_public_headers PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_public_headers)
sc_apply_sanitizers(test_public_headers)
sc_apply_hardening(test_public_headers)

add_executable(test_public_headers_cpp
    tests/unit/test_public_headers_cpp.cpp
)
target_link_libraries(test_public_headers_cpp PRIVATE sc_core sc_test_support)
sc_apply_sanitizers(test_public_headers_cpp)
sc_apply_hardening(test_public_headers_cpp)

if(SC_ENABLE_THIRD_PARTY_DEPS AND EXISTS "${SC_THIRD_PARTY_ROOT}" AND
   TARGET sc_tp_hiredis AND TARGET sc_tp_rabbitmq)
    add_executable(test_third_party_deps
        tests/unit/test_third_party_deps.c
    )
    target_link_libraries(test_third_party_deps PRIVATE sc_third_party sc_test_support)
    add_dependencies(test_third_party_deps sc_third_party_build)
    sc_apply_warnings(test_third_party_deps)
    sc_apply_sanitizers(test_third_party_deps)
    sc_apply_hardening(test_third_party_deps)
endif()


add_executable(test_foundation_parsers
    tests/unit/test_foundation_parsers.c
)
target_link_libraries(test_foundation_parsers PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_foundation_parsers)
sc_apply_sanitizers(test_foundation_parsers)
sc_apply_hardening(test_foundation_parsers)

add_executable(test_config
    tests/unit/test_config.c
)
target_link_libraries(test_config PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_config)
sc_apply_sanitizers(test_config)
sc_apply_hardening(test_config)

add_executable(test_contracts_registry
    tests/support/mock_contracts.c
    tests/unit/test_contracts_registry.c
)
target_link_libraries(test_contracts_registry PRIVATE sc_core sc_test_support)
target_include_directories(test_contracts_registry PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/support"
)
sc_apply_warnings(test_contracts_registry)
sc_apply_sanitizers(test_contracts_registry)
sc_apply_hardening(test_contracts_registry)

add_executable(test_security
    tests/unit/test_security.c
)
target_link_libraries(test_security PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_security)
sc_apply_sanitizers(test_security)
sc_apply_hardening(test_security)

add_executable(test_memory
    tests/unit/test_memory.c
)
target_link_libraries(test_memory PRIVATE sc_core sc_test_support)
if(TARGET SC::sqlite3)
    target_link_libraries(test_memory PRIVATE SC::sqlite3)
    target_compile_definitions(test_memory PRIVATE SC_HAVE_SQLITE=1)
endif()
sc_apply_warnings(test_memory)
sc_apply_sanitizers(test_memory)
sc_apply_hardening(test_memory)

add_executable(test_providers
    tests/unit/test_providers.c
)
target_link_libraries(test_providers PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_providers)
sc_apply_sanitizers(test_providers)
sc_apply_hardening(test_providers)

add_executable(test_tools
    tests/unit/test_tools.c
)
target_link_libraries(test_tools PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_tools)
sc_apply_sanitizers(test_tools)
sc_apply_hardening(test_tools)

add_executable(test_runtime
    tests/unit/test_runtime.c
)
target_link_libraries(test_runtime PRIVATE sc_core sc_test_support SC::libuv)
sc_apply_warnings(test_runtime)
sc_apply_sanitizers(test_runtime)
sc_apply_hardening(test_runtime)

add_executable(test_automation_bootstrap
    tests/unit/test_automation_bootstrap.c
)
target_link_libraries(test_automation_bootstrap PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_automation_bootstrap)
sc_apply_sanitizers(test_automation_bootstrap)
sc_apply_hardening(test_automation_bootstrap)

add_executable(test_gateway
    tests/unit/test_gateway.c
)
target_link_libraries(test_gateway PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_gateway)
sc_apply_sanitizers(test_gateway)
sc_apply_hardening(test_gateway)

add_executable(test_channels
    tests/unit/test_channels.c
)
target_link_libraries(test_channels PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_channels)
sc_apply_sanitizers(test_channels)
sc_apply_hardening(test_channels)

add_executable(test_channels_alignment
    tests/unit/test_channels_alignment.c
)
target_link_libraries(test_channels_alignment PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_channels_alignment)
sc_apply_sanitizers(test_channels_alignment)
sc_apply_hardening(test_channels_alignment)

add_executable(test_philosophy_alignment
    tests/unit/test_philosophy_alignment.c
)
target_link_libraries(test_philosophy_alignment PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_philosophy_alignment)
sc_apply_sanitizers(test_philosophy_alignment)
sc_apply_hardening(test_philosophy_alignment)

add_executable(test_media
    tests/unit/test_media.c
)
target_link_libraries(test_media PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_media)
sc_apply_sanitizers(test_media)
sc_apply_hardening(test_media)

add_executable(test_hardware
    tests/unit/test_hardware.c
)
target_link_libraries(test_hardware PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_hardware)
sc_apply_sanitizers(test_hardware)
sc_apply_hardening(test_hardware)

add_library(plugin_missing_descriptor MODULE
    tests/plugins/plugin_missing_descriptor.c
)
set_target_properties(plugin_missing_descriptor PROPERTIES
    PREFIX ""
)
sc_apply_warnings(plugin_missing_descriptor)
sc_apply_sanitizers(plugin_missing_descriptor)
sc_apply_hardening(plugin_missing_descriptor)

add_library(plugin_export_descriptor MODULE
    tests/plugins/plugin_export_descriptor.c
)
set_target_properties(plugin_export_descriptor PROPERTIES
    PREFIX ""
)
target_include_directories(plugin_export_descriptor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/compat"
    "${CMAKE_CURRENT_BINARY_DIR}/include"
)
target_compile_definitions(plugin_export_descriptor PRIVATE _POSIX_C_SOURCE=200809L)
sc_apply_warnings(plugin_export_descriptor)
sc_apply_sanitizers(plugin_export_descriptor)
sc_apply_hardening(plugin_export_descriptor)

add_executable(test_plugins
    tests/unit/test_plugins.c
)
target_link_libraries(test_plugins PRIVATE sc_core sc_test_support)
target_compile_definitions(test_plugins PRIVATE
    SC_PLUGIN_MISSING_DESCRIPTOR_PATH="$<TARGET_FILE:plugin_missing_descriptor>"
)
add_dependencies(test_plugins plugin_missing_descriptor)
sc_apply_warnings(test_plugins)
sc_apply_sanitizers(test_plugins)
sc_apply_hardening(test_plugins)

add_executable(test_observability_i18n
    tests/unit/test_observability_i18n.c
)
target_link_libraries(test_observability_i18n PRIVATE sc_core sc_test_support)
sc_apply_warnings(test_observability_i18n)
sc_apply_sanitizers(test_observability_i18n)
sc_apply_hardening(test_observability_i18n)

add_executable(test_validation_smoke
    tests/unit/test_validation_smoke.c
)
target_link_libraries(test_validation_smoke PRIVATE sc_core sc_test_support)
add_dependencies(test_validation_smoke plugin_export_descriptor)
sc_apply_warnings(test_validation_smoke)
sc_apply_sanitizers(test_validation_smoke)
sc_apply_hardening(test_validation_smoke)

add_test(NAME unit_build_info COMMAND test_build_info)
add_test(NAME unit_core COMMAND test_core)
add_test(NAME unit_http_client COMMAND test_http_client)
add_test(NAME example_custom_provider COMMAND example_custom_provider)
add_test(NAME public_headers COMMAND test_public_headers)
add_test(NAME public_headers_cpp COMMAND test_public_headers_cpp)
if(TARGET test_third_party_deps)
    add_test(NAME third_party_deps COMMAND test_third_party_deps)
endif()
add_test(NAME unit_contracts_registry COMMAND test_contracts_registry)
add_test(NAME unit_security COMMAND test_security)
add_test(NAME unit_memory COMMAND test_memory)
add_test(NAME unit_providers COMMAND test_providers)
add_test(NAME unit_tools COMMAND test_tools)
add_test(NAME unit_runtime COMMAND test_runtime)
add_test(NAME unit_automation_bootstrap COMMAND test_automation_bootstrap)
add_test(NAME unit_gateway COMMAND test_gateway)
add_test(NAME unit_channels COMMAND test_channels)
add_test(NAME channels_alignment COMMAND test_channels_alignment)
add_test(NAME philosophy_alignment COMMAND test_philosophy_alignment)
add_test(NAME unit_media COMMAND test_media)
add_test(NAME unit_hardware COMMAND test_hardware)
add_test(NAME unit_plugins COMMAND test_plugins)
add_test(NAME unit_observability_i18n COMMAND test_observability_i18n)
add_test(NAME validation_smoke COMMAND test_validation_smoke)
add_test(NAME unit_foundation_parsers COMMAND test_foundation_parsers)
add_test(NAME unit_config COMMAND test_config)
add_test(NAME cli_version COMMAND smolclaw --version)
set_tests_properties(cli_version PROPERTIES
    PASS_REGULAR_EXPRESSION "smolclaw-c ${PROJECT_VERSION}"
)
add_test(NAME cli_help COMMAND smolclaw --help)
set_tests_properties(cli_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage: smolclaw"
)
add_test(NAME cli_nested_help COMMAND smolclaw config --help)
set_tests_properties(cli_nested_help PROPERTIES
    PASS_REGULAR_EXPRESSION "Inspect or edit configuration"
)
add_test(NAME cli_nested_command
    COMMAND
        ${CMAKE_COMMAND} -E env
        SMOLCLAW_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.toml
        SMOLCLAW_ONCE=1
        SMOLCLAW_MAX_POLLS=1
        $<TARGET_FILE:smolclaw> gateway serve
)
add_test(NAME cli_daemon
    COMMAND
        ${CMAKE_COMMAND} -E env
        SMOLCLAW_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.toml
        SMOLCLAW_ONCE=1
        SMOLCLAW_MAX_POLLS=1
        $<TARGET_FILE:smolclaw> daemon
)
add_test(NAME cli_features COMMAND smolclaw --features)
set_tests_properties(cli_features PROPERTIES
    PASS_REGULAR_EXPRESSION "SC_ENABLE_RUNTIME="
)
add_test(NAME cli_doctor COMMAND smolclaw doctor)
set_tests_properties(cli_doctor PROPERTIES
    PASS_REGULAR_EXPRESSION "SmolClaw doctor"
)
add_test(NAME cli_provider COMMAND smolclaw provider)
set_tests_properties(cli_provider PROPERTIES
    PASS_REGULAR_EXPRESSION "status: ok"
)
add_test(NAME cli_provider_config
    COMMAND
        ${CMAKE_COMMAND} -E env
        SMOLCLAW_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/gateway.toml
        $<TARGET_FILE:smolclaw> provider
)
set_tests_properties(cli_provider_config PROPERTIES
    PASS_REGULAR_EXPRESSION "configured_provider: mock .*status: ok"
)
add_test(NAME cli_config COMMAND smolclaw config)
set_tests_properties(cli_config PROPERTIES
    PASS_REGULAR_EXPRESSION "SmolClaw config"
)
add_test(NAME cli_config_show COMMAND smolclaw config show)
set_tests_properties(cli_config_show PROPERTIES
    PASS_REGULAR_EXPRESSION "provider.api_key = \\[REDACTED\\]"
)
if(UNIX)
    add_test(NAME cli_cron_commands
        COMMAND
            /bin/sh -c "tmp=$(mktemp -d) && workspace=\"$tmp/workspace\" && mkdir -p \"$workspace\" && SMOLCLAW_CONFIG=\"$tmp/smolclaw.toml\" SMOLCLAW_WORKSPACE=\"$workspace\" '$<TARGET_FILE:smolclaw>' cron add daily-report '0 9 * * *' 'Send daily summary' && out=$(SMOLCLAW_CONFIG=\"$tmp/smolclaw.toml\" SMOLCLAW_WORKSPACE=\"$workspace\" '$<TARGET_FILE:smolclaw>' cron list) && printf '%s\n' \"$out\" && printf '%s\n' \"$out\" | grep daily-report && SMOLCLAW_CONFIG=\"$tmp/smolclaw.toml\" SMOLCLAW_WORKSPACE=\"$workspace\" '$<TARGET_FILE:smolclaw>' cron remove daily-report && out=$(SMOLCLAW_CONFIG=\"$tmp/smolclaw.toml\" SMOLCLAW_WORKSPACE=\"$workspace\" '$<TARGET_FILE:smolclaw>' cron list) && printf '%s\n' \"$out\" && ! printf '%s\n' \"$out\" | grep daily-report"
    )
endif()
if(UNIX)
    add_test(NAME cli_onboard_dry_run_provider
        COMMAND
            /bin/sh -c "printf '1\\n\\n\\nsk-test\\n1\\n' | '$<TARGET_FILE:smolclaw>' onboard --dry-run"
    )
    set_tests_properties(cli_onboard_dry_run_provider PROPERTIES
        PASS_REGULAR_EXPRESSION "status: dry-run"
        FAIL_REGULAR_EXPRESSION "sk-test"
    )
    add_test(NAME cli_onboard_dry_run_telegram
        COMMAND
            /bin/sh -c "printf '2\\n\\n\\ngemini-secret\\n2\\ntg-secret\\n[\"*\"]\\n' | '$<TARGET_FILE:smolclaw>' onboard --dry-run"
    )
    set_tests_properties(cli_onboard_dry_run_telegram PROPERTIES
        PASS_REGULAR_EXPRESSION "channels.telegram"
        FAIL_REGULAR_EXPRESSION "tg-secret|gemini-secret"
    )
    add_test(NAME cli_onboard_creates_workspace_markdown
        COMMAND
            /bin/sh -c "tmp=$(mktemp -d) && export HOME=\"$tmp\" && printf '7\\n\\n\\n1\\n' | '$<TARGET_FILE:smolclaw>' onboard && test -f \"$tmp/.smolclaw/workspace/AGENTS.md\" && test -f \"$tmp/.smolclaw/workspace/SOUL.md\" && test -f \"$tmp/.smolclaw/workspace/USER.md\" && test -f \"$tmp/.smolclaw/workspace/IDENTITY.md\" && test -f \"$tmp/.smolclaw/workspace/MEMORY.md\" && test -f \"$tmp/.smolclaw/workspace/TOOLS.md\" && test -f \"$tmp/.smolclaw/workspace/HEARTBEAT.md\""
    )
endif()
add_test(NAME cli_config_preset_default COMMAND smolclaw config preset default-posture)
set_tests_properties(cli_config_preset_default PROPERTIES
    PASS_REGULAR_EXPRESSION "shell_enabled = false"
)
add_test(NAME cli_config_preset_yolo COMMAND smolclaw config preset yolo)
set_tests_properties(cli_config_preset_yolo PROPERTIES
    PASS_REGULAR_EXPRESSION "disabled_guardrails:"
)
add_test(NAME cli_estop_status COMMAND smolclaw estop status)
set_tests_properties(cli_estop_status PROPERTIES
    PASS_REGULAR_EXPRESSION "active: false"
)
add_test(NAME cli_service_status COMMAND smolclaw service status)
set_tests_properties(cli_service_status PROPERTIES
    PASS_REGULAR_EXPRESSION "SmolClaw service"
)
add_test(NAME cli_service_dry_run COMMAND smolclaw service dry-run)
set_tests_properties(cli_service_dry_run PROPERTIES
    PASS_REGULAR_EXPRESSION "dry_run: true"
)
add_test(NAME cli_service_install_dry_run
    COMMAND
        ${CMAKE_COMMAND} -E env
        SMOLCLAW_SERVICE_DRY_RUN=1
        $<TARGET_FILE:smolclaw> service install
)
set_tests_properties(cli_service_install_dry_run PROPERTIES
    PASS_REGULAR_EXPRESSION "would_write: true"
)
add_test(NAME cli_service_uninstall_dry_run
    COMMAND
        ${CMAKE_COMMAND} -E env
        SMOLCLAW_SERVICE_DRY_RUN=1
        $<TARGET_FILE:smolclaw> service uninstall
)
set_tests_properties(cli_service_uninstall_dry_run PROPERTIES
    PASS_REGULAR_EXPRESSION "would_remove:"
)
if(UNIX)
    add_test(NAME cli_acp_initialize
        COMMAND /bin/sh -c
                "printf '%s\\n' '{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}' | \"$1\" acp"
                sh
                $<TARGET_FILE:smolclaw>
    )
    set_tests_properties(cli_acp_initialize PROPERTIES
        PASS_REGULAR_EXPRESSION "\"protocolVersion\":\"0.1\""
    )
endif()
add_test(NAME cli_unknown_argument COMMAND smolclaw --unknown)
set_tests_properties(cli_unknown_argument PROPERTIES
    WILL_FAIL TRUE
)


set_tests_properties(unit_build_info unit_core public_headers public_headers_cpp unit_foundation_parsers unit_contracts_registry unit_config unit_security unit_memory PROPERTIES
    LABELS "unit"
)
set_tests_properties(example_custom_provider PROPERTIES
    LABELS "examples"
)
set_tests_properties(unit_providers unit_tools unit_runtime unit_automation_bootstrap unit_gateway unit_channels channels_alignment philosophy_alignment unit_media unit_hardware unit_plugins unit_observability_i18n PROPERTIES
    LABELS "component"
)
if(TARGET test_third_party_deps)
    set_tests_properties(third_party_deps PROPERTIES
        LABELS "component"
    )
endif()
set_tests_properties(cli_version cli_help cli_nested_help cli_nested_command cli_features cli_doctor cli_unknown_argument cli_config_preset_default cli_config_preset_yolo cli_estop_status cli_service_status cli_service_dry_run cli_service_install_dry_run cli_service_uninstall_dry_run PROPERTIES
    LABELS "integration"
)
if(UNIX)
    set_tests_properties(cli_acp_initialize PROPERTIES
        LABELS "integration"
    )
endif()
set_tests_properties(public_headers public_headers_cpp unit_plugins validation_smoke PROPERTIES
    LABELS "abi"
)
set_tests_properties(validation_smoke PROPERTIES
    LABELS "fuzz;docs;abi"
)
set_tests_properties(public_headers public_headers_cpp PROPERTIES
    LABELS "unit;abi"
)
set_tests_properties(unit_plugins PROPERTIES
    LABELS "component;abi"
)
