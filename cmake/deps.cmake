option(SC_ENABLE_THIRD_PARTY_DEPS "Build vendored C dependency targets." ON)
option(SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT "Build wolfSSL-backed websocket-client target." ON)
option(SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER "Build libuv-backed websocket-server target." ON)
option(SC_REQUIRE_SQLITE_FTS5 "Require SQLite FTS5 support for session search indexes." ON)

set(SC_DEPS_PROVIDER
    "github"
    CACHE STRING
    "Dependency provider: github, system, or auto."
)
set_property(CACHE SC_DEPS_PROVIDER PROPERTY STRINGS github system auto)

set(SC_CURL_TLS_BACKEND
    "mbedtls"
    CACHE STRING
    "TLS backend for GitHub-built curl: wolfssl or mbedtls."
)
set_property(CACHE SC_CURL_TLS_BACKEND PROPERTY STRINGS wolfssl mbedtls)

set(SC_DEPS_PREFIX
    "${CMAKE_BINARY_DIR}/_sc_deps/install"
    CACHE PATH
    "Install prefix for dependencies built from GitHub."
)
set(SC_MBEDTLS_PYTHON_VENV
    "${CMAKE_BINARY_DIR}/_sc_deps/venv/mbedtls"
    CACHE PATH
    "Python virtual environment used for GitHub-built Mbed TLS code generation."
)
set(SC_UV_BOOTSTRAP_DIR
    "${CMAKE_BINARY_DIR}/_sc_deps/bin"
    CACHE PATH
    "Build-local directory for bootstrapped uv when uv is not installed."
)

set(SC_LIBUV_TAG "v1.52.1" CACHE STRING "Pinned libuv Git tag.")
set(SC_CURL_TAG "curl-8_20_0" CACHE STRING "Pinned curl Git tag.")
set(SC_WOLFSSL_TAG "v5.9.1-stable" CACHE STRING "Pinned wolfSSL Git tag.")
set(SC_MBEDTLS_TAG "mbedtls-4.1.0" CACHE STRING "Pinned Mbed TLS Git tag.")
set(SC_NGHTTP2_TAG "v1.69.0" CACHE STRING "Pinned nghttp2 Git tag.")
set(SC_LIBSODIUM_TAG "1.0.22-RELEASE" CACHE STRING "Pinned libsodium Git tag.")
set(SC_CMARK_TAG "0.31.2" CACHE STRING "Pinned cmark Git tag.")
set(SC_OPUS_TAG "v1.5.2" CACHE STRING "Pinned Opus Git tag.")
set(SC_SQLITE_TAG "version-3.53.0" CACHE STRING "Pinned SQLite Git tag.")
set(SC_WAMR_TAG "WAMR-2.4.4" CACHE STRING "Pinned wasm-micro-runtime Git tag.")
set(SC_CPYTHON_TAG "v3.14.4" CACHE STRING "Pinned CPython Git tag.")
set(SC_MIMALLOC_TAG "v3.3.2" CACHE STRING "Pinned mimalloc Git tag.")
set(SC_JEMALLOC_TAG "5.3.1" CACHE STRING "Pinned jemalloc release tag.")
set(SC_ISOCLINE_TAG "v1.1.0" CACHE STRING "Pinned isocline Git tag.")
set(SC_JEMALLOC_SHA256
    "3826bc80232f22ed5c4662f3034f799ca316e819103bdc7bb99018a421706f92"
    CACHE STRING
    "SHA256 for the pinned jemalloc GitHub release archive."
)

set(SC_THIRD_PARTY_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
    CACHE PATH
    "Path to vendored C dependency sources."
)

include(ExternalProject)
include(FindPkgConfig)
include(GNUInstallDirs)
include(CheckCSourceRuns)
find_package(Threads QUIET)
find_program(SC_MAKE_PROGRAM NAMES gmake make)
find_program(SC_UV_EXECUTABLE NAMES uv)
find_program(SC_CURL_EXECUTABLE NAMES curl)
find_program(SC_SH_EXECUTABLE NAMES sh)
if(SC_UV_EXECUTABLE)
    set(SC_UV_COMMAND "${SC_UV_EXECUTABLE}")
else()
    set(SC_UV_COMMAND "${SC_UV_BOOTSTRAP_DIR}/uv")
endif()
if(UNIX)
    find_library(SC_MATH_LIBRARY m)
    find_library(SC_RT_LIBRARY rt)
    find_library(SC_ATOMIC_LIBRARY atomic)
    find_library(SC_TINFO_LIBRARY tinfo)
    find_library(SC_UTIL_LIBRARY util)
endif()

function(sc_validate_deps_provider)
    if(NOT SC_DEPS_PROVIDER STREQUAL "github" AND
       NOT SC_DEPS_PROVIDER STREQUAL "system" AND
       NOT SC_DEPS_PROVIDER STREQUAL "auto")
        message(FATAL_ERROR "SC_DEPS_PROVIDER must be one of: github, system, auto.")
    endif()
    if(NOT SC_CURL_TLS_BACKEND STREQUAL "wolfssl" AND
       NOT SC_CURL_TLS_BACKEND STREQUAL "mbedtls")
        message(FATAL_ERROR "SC_CURL_TLS_BACKEND must be one of: wolfssl, mbedtls.")
    endif()
endfunction()

function(sc_provider_allows_system out)
    if(SC_DEPS_PROVIDER STREQUAL "system" OR SC_DEPS_PROVIDER STREQUAL "auto")
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_provider_allows_github out)
    if(SC_DEPS_PROVIDER STREQUAL "github" OR SC_DEPS_PROVIDER STREQUAL "auto")
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_ensure_dir path)
    file(MAKE_DIRECTORY "${path}")
endfunction()

function(sc_add_imported_static target location)
    cmake_parse_arguments(
        SC_IMPORT
        ""
        ""
        "INCLUDE_DIRS;LINK_LIBRARIES;COMPILE_DEFINITIONS"
        ${ARGN}
    )

    if(TARGET "${target}")
        return()
    endif()

    foreach(include_dir IN LISTS SC_IMPORT_INCLUDE_DIRS)
        sc_ensure_dir("${include_dir}")
    endforeach()

    add_library("${target}" STATIC IMPORTED GLOBAL)
    set_target_properties("${target}" PROPERTIES
        IMPORTED_LOCATION "${location}"
    )
    if(SC_IMPORT_INCLUDE_DIRS)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SC_IMPORT_INCLUDE_DIRS}"
        )
    endif()
    if(SC_IMPORT_LINK_LIBRARIES)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_LINK_LIBRARIES "${SC_IMPORT_LINK_LIBRARIES}"
        )
    endif()
    if(SC_IMPORT_COMPILE_DEFINITIONS)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${SC_IMPORT_COMPILE_DEFINITIONS}"
        )
    endif()
endfunction()

function(sc_add_imported_unknown target location)
    cmake_parse_arguments(
        SC_IMPORT
        ""
        ""
        "INCLUDE_DIRS;LINK_LIBRARIES;COMPILE_DEFINITIONS"
        ${ARGN}
    )

    if(TARGET "${target}")
        return()
    endif()

    foreach(include_dir IN LISTS SC_IMPORT_INCLUDE_DIRS)
        sc_ensure_dir("${include_dir}")
    endforeach()

    add_library("${target}" UNKNOWN IMPORTED GLOBAL)
    set_target_properties("${target}" PROPERTIES
        IMPORTED_LOCATION "${location}"
    )
    if(SC_IMPORT_INCLUDE_DIRS)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${SC_IMPORT_INCLUDE_DIRS}"
        )
    endif()
    if(SC_IMPORT_LINK_LIBRARIES)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_LINK_LIBRARIES "${SC_IMPORT_LINK_LIBRARIES}"
        )
    endif()
    if(SC_IMPORT_COMPILE_DEFINITIONS)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${SC_IMPORT_COMPILE_DEFINITIONS}"
        )
    endif()
endfunction()

function(sc_add_imported_interface target)
    cmake_parse_arguments(
        SC_IMPORT
        ""
        ""
        "LINK_LIBRARIES;COMPILE_DEFINITIONS"
        ${ARGN}
    )

    if(TARGET "${target}")
        return()
    endif()

    add_library("${target}" INTERFACE IMPORTED GLOBAL)
    if(SC_IMPORT_LINK_LIBRARIES)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_LINK_LIBRARIES "${SC_IMPORT_LINK_LIBRARIES}"
        )
    endif()
    if(SC_IMPORT_COMPILE_DEFINITIONS)
        set_target_properties("${target}" PROPERTIES
            INTERFACE_COMPILE_DEFINITIONS "${SC_IMPORT_COMPILE_DEFINITIONS}"
        )
    endif()
endfunction()

function(sc_try_system_libuv out)
    pkg_check_modules(SC_SYSTEM_LIBUV QUIET IMPORTED_TARGET libuv)
    if(SC_SYSTEM_LIBUV_FOUND AND TARGET PkgConfig::SC_SYSTEM_LIBUV)
        sc_add_imported_interface(SC::libuv LINK_LIBRARIES PkgConfig::SC_SYSTEM_LIBUV)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_try_system_curl out)
    find_package(CURL QUIET)
    if(CURL_FOUND AND TARGET CURL::libcurl)
        sc_add_imported_interface(SC::libcurl LINK_LIBRARIES CURL::libcurl)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_try_system_nghttp2 out)
    find_package(nghttp2 CONFIG QUIET)
    if(TARGET nghttp2::nghttp2_static)
        sc_add_imported_interface(SC::nghttp2 LINK_LIBRARIES nghttp2::nghttp2_static)
        set("${out}" TRUE PARENT_SCOPE)
    elseif(TARGET nghttp2::nghttp2)
        sc_add_imported_interface(SC::nghttp2 LINK_LIBRARIES nghttp2::nghttp2)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        pkg_check_modules(SC_SYSTEM_NGHTTP2 QUIET IMPORTED_TARGET libnghttp2)
        if(SC_SYSTEM_NGHTTP2_FOUND AND TARGET PkgConfig::SC_SYSTEM_NGHTTP2)
            sc_add_imported_interface(SC::nghttp2 LINK_LIBRARIES PkgConfig::SC_SYSTEM_NGHTTP2)
            set("${out}" TRUE PARENT_SCOPE)
        else()
            find_library(SC_SYSTEM_NGHTTP2_LIBRARY NAMES nghttp2)
            find_path(SC_SYSTEM_NGHTTP2_INCLUDE_DIR NAMES nghttp2/nghttp2.h)
            if(SC_SYSTEM_NGHTTP2_LIBRARY AND SC_SYSTEM_NGHTTP2_INCLUDE_DIR)
                sc_add_imported_unknown(
                    SC::nghttp2
                    "${SC_SYSTEM_NGHTTP2_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_NGHTTP2_INCLUDE_DIR}"
                    COMPILE_DEFINITIONS NGHTTP2_STATICLIB
                )
                set("${out}" TRUE PARENT_SCOPE)
            else()
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()

function(sc_try_system_wolfssl out)
    pkg_check_modules(SC_SYSTEM_WOLFSSL QUIET IMPORTED_TARGET wolfssl)
    if(SC_SYSTEM_WOLFSSL_FOUND AND TARGET PkgConfig::SC_SYSTEM_WOLFSSL)
        sc_add_imported_interface(SC::wolfssl LINK_LIBRARIES PkgConfig::SC_SYSTEM_WOLFSSL)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_try_system_mbedtls out)
    find_package(MbedTLS CONFIG QUIET)
    if(TARGET MbedTLS::mbedtls)
        sc_add_imported_interface(SC::mbedtls LINK_LIBRARIES MbedTLS::mbedtls)
        set("${out}" TRUE PARENT_SCOPE)
    elseif(TARGET mbedtls)
        sc_add_imported_interface(SC::mbedtls LINK_LIBRARIES mbedtls)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        pkg_check_modules(SC_SYSTEM_MBEDTLS QUIET IMPORTED_TARGET mbedtls)
        if(SC_SYSTEM_MBEDTLS_FOUND AND TARGET PkgConfig::SC_SYSTEM_MBEDTLS)
            sc_add_imported_interface(SC::mbedtls LINK_LIBRARIES PkgConfig::SC_SYSTEM_MBEDTLS)
            set("${out}" TRUE PARENT_SCOPE)
        else()
            find_library(SC_SYSTEM_MBEDTLS_LIBRARY NAMES mbedtls)
            find_library(SC_SYSTEM_MBEDX509_LIBRARY NAMES mbedx509)
            find_library(SC_SYSTEM_MBEDCRYPTO_LIBRARY NAMES mbedcrypto)
            find_path(SC_SYSTEM_MBEDTLS_INCLUDE_DIR NAMES mbedtls/ssl.h)
            if(SC_SYSTEM_MBEDTLS_LIBRARY AND
               SC_SYSTEM_MBEDX509_LIBRARY AND
               SC_SYSTEM_MBEDCRYPTO_LIBRARY AND
               SC_SYSTEM_MBEDTLS_INCLUDE_DIR)
                sc_add_imported_static(
                    SC::mbedcrypto
                    "${SC_SYSTEM_MBEDCRYPTO_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_MBEDTLS_INCLUDE_DIR}"
                )
                sc_add_imported_static(
                    SC::mbedx509
                    "${SC_SYSTEM_MBEDX509_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_MBEDTLS_INCLUDE_DIR}"
                    LINK_LIBRARIES SC::mbedcrypto
                )
                sc_add_imported_static(
                    SC::mbedtls
                    "${SC_SYSTEM_MBEDTLS_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_MBEDTLS_INCLUDE_DIR}"
                    LINK_LIBRARIES SC::mbedx509 SC::mbedcrypto
                )
                set("${out}" TRUE PARENT_SCOPE)
            else()
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()

function(sc_try_system_libsodium out)
    pkg_check_modules(SC_SYSTEM_LIBSODIUM QUIET IMPORTED_TARGET libsodium)
    if(SC_SYSTEM_LIBSODIUM_FOUND AND TARGET PkgConfig::SC_SYSTEM_LIBSODIUM)
        sc_add_imported_interface(SC::libsodium LINK_LIBRARIES PkgConfig::SC_SYSTEM_LIBSODIUM)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_try_system_cmark out)
    pkg_check_modules(SC_SYSTEM_CMARK QUIET IMPORTED_TARGET libcmark)
    if(NOT SC_SYSTEM_CMARK_FOUND)
        pkg_check_modules(SC_SYSTEM_CMARK QUIET IMPORTED_TARGET cmark)
    endif()
    if(SC_SYSTEM_CMARK_FOUND AND TARGET PkgConfig::SC_SYSTEM_CMARK)
        sc_add_imported_interface(SC::cmark LINK_LIBRARIES PkgConfig::SC_SYSTEM_CMARK)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        set("${out}" FALSE PARENT_SCOPE)
    endif()
endfunction()

function(sc_try_system_opus out)
    find_package(Opus CONFIG QUIET)
    if(TARGET Opus::opus)
        sc_add_imported_interface(SC::opus LINK_LIBRARIES Opus::opus)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        pkg_check_modules(SC_SYSTEM_OPUS QUIET IMPORTED_TARGET opus)
        if(SC_SYSTEM_OPUS_FOUND AND TARGET PkgConfig::SC_SYSTEM_OPUS)
            sc_add_imported_interface(SC::opus LINK_LIBRARIES PkgConfig::SC_SYSTEM_OPUS)
            set("${out}" TRUE PARENT_SCOPE)
        else()
            find_library(SC_SYSTEM_OPUS_LIBRARY NAMES opus)
            find_path(SC_SYSTEM_OPUS_INCLUDE_DIR NAMES opus/opus.h)
            if(SC_SYSTEM_OPUS_LIBRARY AND SC_SYSTEM_OPUS_INCLUDE_DIR)
                sc_add_imported_unknown(
                    SC::opus
                    "${SC_SYSTEM_OPUS_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_OPUS_INCLUDE_DIR}"
                    LINK_LIBRARIES "${SC_MATH_LIBRARY}"
                )
                set("${out}" TRUE PARENT_SCOPE)
            else()
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()

function(sc_sqlite_has_fts5 required_libraries required_includes out)
    if(CMAKE_CROSSCOMPILING)
        set("${out}" FALSE PARENT_SCOPE)
        return()
    endif()

    set(SC_PREVIOUS_REQUIRED_LIBRARIES "${CMAKE_REQUIRED_LIBRARIES}")
    set(SC_PREVIOUS_REQUIRED_INCLUDES "${CMAKE_REQUIRED_INCLUDES}")
    set(SC_PREVIOUS_REQUIRED_QUIET "${CMAKE_REQUIRED_QUIET}")
    set(CMAKE_REQUIRED_LIBRARIES "${required_libraries}")
    set(CMAKE_REQUIRED_INCLUDES "${required_includes}")
    set(CMAKE_REQUIRED_QUIET TRUE)
    unset(SC_SYSTEM_SQLITE_HAS_FTS5 CACHE)
    check_c_source_runs("
        #include <sqlite3.h>

        int main(void) {
            sqlite3 *db = 0;
            char *errmsg = 0;
            int rc = sqlite3_open(\":memory:\", &db);
            if (rc != SQLITE_OK) {
                if (db != 0) {
                    (void)sqlite3_close(db);
                }
                return 1;
            }
            rc = sqlite3_exec(db,
                              \"CREATE VIRTUAL TABLE sc_fts_probe USING fts5(content)\",
                              0,
                              0,
                              &errmsg);
            sqlite3_free(errmsg);
            (void)sqlite3_close(db);
            return rc == SQLITE_OK ? 0 : 1;
        }
    " SC_SYSTEM_SQLITE_HAS_FTS5)
    set(CMAKE_REQUIRED_LIBRARIES "${SC_PREVIOUS_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_INCLUDES "${SC_PREVIOUS_REQUIRED_INCLUDES}")
    set(CMAKE_REQUIRED_QUIET "${SC_PREVIOUS_REQUIRED_QUIET}")
    set("${out}" "${SC_SYSTEM_SQLITE_HAS_FTS5}" PARENT_SCOPE)
endfunction()

function(sc_try_system_sqlite out)
    pkg_check_modules(SC_SYSTEM_SQLITE QUIET IMPORTED_TARGET sqlite3)
    if(SC_SYSTEM_SQLITE_FOUND AND TARGET PkgConfig::SC_SYSTEM_SQLITE)
        set(accept TRUE)
        if(SC_REQUIRE_SQLITE_FTS5)
            sc_sqlite_has_fts5(PkgConfig::SC_SYSTEM_SQLITE "" accept)
        endif()
        if(accept)
            sc_add_imported_interface(SC::sqlite3 LINK_LIBRARIES PkgConfig::SC_SYSTEM_SQLITE)
            set("${out}" TRUE PARENT_SCOPE)
        else()
            message(STATUS "System SQLite from pkg-config does not provide FTS5; falling back to GitHub SQLite when allowed.")
            set("${out}" FALSE PARENT_SCOPE)
        endif()
    else()
        find_package(SQLite3 QUIET)
        if(SQLite3_FOUND AND TARGET SQLite::SQLite3)
            set(accept TRUE)
            if(SC_REQUIRE_SQLITE_FTS5)
                sc_sqlite_has_fts5(SQLite::SQLite3 "" accept)
            endif()
            if(accept)
                sc_add_imported_interface(SC::sqlite3 LINK_LIBRARIES SQLite::SQLite3)
                set("${out}" TRUE PARENT_SCOPE)
            else()
                message(STATUS "System SQLite from CMake package does not provide FTS5; falling back to GitHub SQLite when allowed.")
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        else()
            find_library(SC_SYSTEM_SQLITE_LIBRARY NAMES sqlite3)
            find_path(SC_SYSTEM_SQLITE_INCLUDE_DIR NAMES sqlite3.h)
            if(SC_SYSTEM_SQLITE_LIBRARY AND SC_SYSTEM_SQLITE_INCLUDE_DIR)
                set(accept TRUE)
                if(SC_REQUIRE_SQLITE_FTS5)
                    sc_sqlite_has_fts5("${SC_SYSTEM_SQLITE_LIBRARY}" "${SC_SYSTEM_SQLITE_INCLUDE_DIR}" accept)
                endif()
                if(accept)
                    sc_add_imported_unknown(
                        SC::sqlite3
                        "${SC_SYSTEM_SQLITE_LIBRARY}"
                        INCLUDE_DIRS "${SC_SYSTEM_SQLITE_INCLUDE_DIR}"
                    )
                    set("${out}" TRUE PARENT_SCOPE)
                else()
                    message(STATUS "System SQLite library does not provide FTS5; falling back to GitHub SQLite when allowed.")
                    set("${out}" FALSE PARENT_SCOPE)
                endif()
            else()
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()

function(sc_try_system_isocline out)
    pkg_check_modules(SC_SYSTEM_ISOCLINE QUIET IMPORTED_TARGET isocline)
    if(SC_SYSTEM_ISOCLINE_FOUND AND TARGET PkgConfig::SC_SYSTEM_ISOCLINE)
        sc_add_imported_interface(SC::isocline LINK_LIBRARIES PkgConfig::SC_SYSTEM_ISOCLINE)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        find_library(SC_SYSTEM_ISOCLINE_LIBRARY NAMES isocline)
        find_path(SC_SYSTEM_ISOCLINE_INCLUDE_DIR NAMES isocline.h)
        if(SC_SYSTEM_ISOCLINE_LIBRARY AND SC_SYSTEM_ISOCLINE_INCLUDE_DIR)
            sc_add_imported_unknown(
                SC::isocline
                "${SC_SYSTEM_ISOCLINE_LIBRARY}"
                INCLUDE_DIRS "${SC_SYSTEM_ISOCLINE_INCLUDE_DIR}"
            )
            set("${out}" TRUE PARENT_SCOPE)
        else()
            set("${out}" FALSE PARENT_SCOPE)
        endif()
    endif()
endfunction()

function(sc_try_system_mimalloc out)
    find_package(mimalloc CONFIG QUIET)
    if(TARGET mimalloc::mimalloc)
        sc_add_imported_interface(SC::mimalloc LINK_LIBRARIES mimalloc::mimalloc)
        set("${out}" TRUE PARENT_SCOPE)
    elseif(TARGET mimalloc::mimalloc-static)
        sc_add_imported_interface(SC::mimalloc LINK_LIBRARIES mimalloc::mimalloc-static)
        set("${out}" TRUE PARENT_SCOPE)
    elseif(TARGET mimalloc)
        sc_add_imported_interface(SC::mimalloc LINK_LIBRARIES mimalloc)
        set("${out}" TRUE PARENT_SCOPE)
    elseif(TARGET mimalloc-static)
        sc_add_imported_interface(SC::mimalloc LINK_LIBRARIES mimalloc-static)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        pkg_check_modules(SC_SYSTEM_MIMALLOC QUIET IMPORTED_TARGET mimalloc)
        if(SC_SYSTEM_MIMALLOC_FOUND AND TARGET PkgConfig::SC_SYSTEM_MIMALLOC)
            sc_add_imported_interface(SC::mimalloc LINK_LIBRARIES PkgConfig::SC_SYSTEM_MIMALLOC)
            set("${out}" TRUE PARENT_SCOPE)
        else()
            find_library(SC_SYSTEM_MIMALLOC_LIBRARY NAMES mimalloc)
            find_path(SC_SYSTEM_MIMALLOC_INCLUDE_DIR NAMES mimalloc.h)
            if(SC_SYSTEM_MIMALLOC_LIBRARY AND SC_SYSTEM_MIMALLOC_INCLUDE_DIR)
                sc_add_imported_unknown(
                    SC::mimalloc
                    "${SC_SYSTEM_MIMALLOC_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_MIMALLOC_INCLUDE_DIR}"
                    COMPILE_DEFINITIONS MI_STATIC_LIB
                )
                set("${out}" TRUE PARENT_SCOPE)
            else()
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()

function(sc_try_system_jemalloc out)
    find_package(jemalloc CONFIG QUIET)
    if(TARGET jemalloc::jemalloc)
        sc_add_imported_interface(SC::jemalloc LINK_LIBRARIES jemalloc::jemalloc)
        set("${out}" TRUE PARENT_SCOPE)
    elseif(TARGET jemalloc)
        sc_add_imported_interface(SC::jemalloc LINK_LIBRARIES jemalloc)
        set("${out}" TRUE PARENT_SCOPE)
    else()
        pkg_check_modules(SC_SYSTEM_JEMALLOC QUIET IMPORTED_TARGET jemalloc)
        if(SC_SYSTEM_JEMALLOC_FOUND AND TARGET PkgConfig::SC_SYSTEM_JEMALLOC)
            sc_add_imported_interface(SC::jemalloc LINK_LIBRARIES PkgConfig::SC_SYSTEM_JEMALLOC)
            set("${out}" TRUE PARENT_SCOPE)
        else()
            find_library(SC_SYSTEM_JEMALLOC_LIBRARY NAMES jemalloc)
            find_path(SC_SYSTEM_JEMALLOC_INCLUDE_DIR NAMES jemalloc/jemalloc.h)
            if(SC_SYSTEM_JEMALLOC_LIBRARY AND SC_SYSTEM_JEMALLOC_INCLUDE_DIR)
                sc_add_imported_unknown(
                    SC::jemalloc
                    "${SC_SYSTEM_JEMALLOC_LIBRARY}"
                    INCLUDE_DIRS "${SC_SYSTEM_JEMALLOC_INCLUDE_DIR}"
                )
                set("${out}" TRUE PARENT_SCOPE)
            else()
                set("${out}" FALSE PARENT_SCOPE)
            endif()
        endif()
    endif()
endfunction()

function(sc_common_external_cmake_args out)
    set(args
        "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
        "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
        "-DCMAKE_DEBUG_POSTFIX="
        "-DCMAKE_INSTALL_PREFIX=${SC_DEPS_PREFIX}"
        "-DCMAKE_INSTALL_LIBDIR=lib"
        "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
    )
    set("${out}" "${args}" PARENT_SCOPE)
endfunction()

function(sc_add_github_libuv)
    if(TARGET SC::libuv)
        return()
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_libuv
        GIT_REPOSITORY https://github.com/libuv/libuv.git
        GIT_TAG "${SC_LIBUV_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
            -DLIBUV_BUILD_SHARED=OFF
            -DLIBUV_BUILD_TESTS=OFF
            -DLIBUV_BUILD_BENCH=OFF
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}uv${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_libuv)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND link_libs ${CMAKE_DL_LIBS})
    endif()
    if(SC_RT_LIBRARY)
        list(APPEND link_libs "${SC_RT_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::libuv
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}uv${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_add_github_wolfssl)
    if(TARGET SC::wolfssl)
        return()
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_wolfssl
        GIT_REPOSITORY https://github.com/wolfSSL/wolfssl.git
        GIT_TAG "${SC_WOLFSSL_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
            -DWOLFSSL_INSTALL=yes
            -DWOLFSSL_EXAMPLES=no
            -DWOLFSSL_CRYPT_TESTS=no
            -DWOLFSSL_CURL=yes
            -DWOLFSSL_OPENSSLEXTRA=yes
            -DWOLFSSL_OPENSSLALL=yes
            -DWOLFSSL_SNI=yes
            -DWOLFSSL_TLS13=yes
            -DWOLFSSL_CERTGEN=yes
            -DWOLFSSL_KEYGEN=yes
            -DWOLFSSL_CERTEXT=yes
            -DWOLFSSL_PKCS7=yes
            -DWOLFSSL_CRL=yes
            -DWOLFSSL_OCSP=yes
            -DWOLFSSL_OCSPSTAPLING=yes
            -DWOLFSSL_OCSPSTAPLING_V2=yes
            -DWOLFSSL_ALT_CERT_CHAINS=yes
            -DCMAKE_C_FLAGS=-DHAVE_CURL
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}wolfssl${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_wolfssl)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::wolfssl
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}wolfssl${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_add_github_nghttp2)
    if(TARGET SC::nghttp2)
        return()
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_nghttp2
        GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
        GIT_TAG "${SC_NGHTTP2_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_STATIC_LIBS=ON
            -DBUILD_TESTING=OFF
            -DENABLE_LIB_ONLY=ON
            -DENABLE_APP=OFF
            -DENABLE_EXAMPLES=OFF
            -DENABLE_HPACK_TOOLS=OFF
            -DENABLE_DOC=OFF
            -DENABLE_FAILMALLOC=OFF
            -DWITH_LIBXML2=OFF
            -DWITH_JEMALLOC=OFF
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}nghttp2${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_nghttp2)

    sc_add_imported_static(
        SC::nghttp2
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}nghttp2${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        COMPILE_DEFINITIONS NGHTTP2_STATICLIB
    )
endfunction()

function(sc_add_uv_bootstrap out)
    if(SC_UV_EXECUTABLE)
        set("${out}" "" PARENT_SCOPE)
        return()
    endif()
    if(TARGET sc_ep_uv)
        set("${out}" sc_ep_uv PARENT_SCOPE)
        return()
    endif()
    if(NOT SC_CURL_EXECUTABLE OR NOT SC_SH_EXECUTABLE)
        message(FATAL_ERROR "Installing uv requires curl and sh. Install uv manually or provide curl and sh on PATH.")
    endif()

    ExternalProject_Add(sc_ep_uv
        PREFIX "${CMAKE_BINARY_DIR}/sc_ep_uv-prefix"
        DOWNLOAD_COMMAND ""
        CONFIGURE_COMMAND "${CMAKE_COMMAND}" -E make_directory "${SC_UV_BOOTSTRAP_DIR}"
        BUILD_COMMAND
            "${CMAKE_COMMAND}" -E env
                "UV_INSTALL_DIR=${SC_UV_BOOTSTRAP_DIR}"
                "${SC_SH_EXECUTABLE}" -c
                "\"${SC_CURL_EXECUTABLE}\" -LsSf https://astral.sh/uv/install.sh | \"${SC_SH_EXECUTABLE}\""
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS "${SC_UV_COMMAND}"
    )
    add_dependencies(sc_external_deps sc_ep_uv)
    set("${out}" sc_ep_uv PARENT_SCOPE)
endfunction()

function(sc_add_github_mbedtls)
    if(TARGET SC::mbedtls)
        return()
    endif()

    sc_add_uv_bootstrap(uv_bootstrap_dep)
    set(mbedtls_python "${SC_MBEDTLS_PYTHON_VENV}/bin/python")
    if(uv_bootstrap_dep)
        set(uv_bootstrap_dep_args DEPENDS ${uv_bootstrap_dep})
    else()
        set(uv_bootstrap_dep_args)
    endif()

    ExternalProject_Add(sc_ep_mbedtls_python
        ${uv_bootstrap_dep_args}
        PREFIX "${CMAKE_BINARY_DIR}/sc_ep_mbedtls_python-prefix"
        DOWNLOAD_COMMAND ""
        CONFIGURE_COMMAND "${SC_UV_COMMAND}" venv "${SC_MBEDTLS_PYTHON_VENV}"
        BUILD_COMMAND
            "${SC_UV_COMMAND}" pip install
                --python "${mbedtls_python}"
                jsonschema
                jinja2
        INSTALL_COMMAND ""
    )
    add_dependencies(sc_external_deps sc_ep_mbedtls_python)

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_mbedtls
        DEPENDS sc_ep_mbedtls_python
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG "${SC_MBEDTLS_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
            -DENABLE_PROGRAMS=OFF
            -DENABLE_TESTING=OFF
            -DUSE_SHARED_MBEDTLS_LIBRARY=OFF
            -DUSE_STATIC_MBEDTLS_LIBRARY=ON
            -DPython3_EXECUTABLE=${mbedtls_python}
        BUILD_BYPRODUCTS
            "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedtls${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedx509${CMAKE_STATIC_LIBRARY_SUFFIX}"
            "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_mbedtls)

    sc_add_imported_static(
        SC::mbedcrypto
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
    )
    sc_add_imported_static(
        SC::mbedx509
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedx509${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES SC::mbedcrypto
    )
    sc_add_imported_static(
        SC::mbedtls
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedtls${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES SC::mbedx509 SC::mbedcrypto
    )
endfunction()

function(sc_add_github_curl)
    if(TARGET SC::libcurl)
        return()
    endif()

    if(SC_CURL_TLS_BACKEND STREQUAL "mbedtls")
        sc_add_github_mbedtls()
        set(curl_tls_args
            -DCURL_USE_WOLFSSL=OFF
            -DCURL_USE_MBEDTLS=ON
            -DMBEDTLS_INCLUDE_DIR=${SC_DEPS_PREFIX}/include
            -DMBEDTLS_LIBRARY=${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedtls${CMAKE_STATIC_LIBRARY_SUFFIX}
            -DMBEDX509_LIBRARY=${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedx509${CMAKE_STATIC_LIBRARY_SUFFIX}
            -DMBEDCRYPTO_LIBRARY=${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mbedcrypto${CMAKE_STATIC_LIBRARY_SUFFIX}
        )
        set(curl_tls_link_libs SC::mbedtls)
        set(SC_LIBCURL_TLS_BACKEND "mbedtls" CACHE INTERNAL "TLS backend used by GitHub-built curl.")
    else()
        sc_add_github_wolfssl()
        set(curl_tls_args
            -DCURL_USE_WOLFSSL=ON
            -DCURL_USE_MBEDTLS=OFF
        )
        set(curl_tls_link_libs SC::wolfssl)
        set(SC_LIBCURL_TLS_BACKEND "wolfssl" CACHE INTERNAL "TLS backend used by GitHub-built curl.")
    endif()
    if(SC_ENABLE_CURL_HTTP2)
        sc_add_github_nghttp2()
        set(curl_http2_args
            -DUSE_NGHTTP2=ON
            -DNGHTTP2_INCLUDE_DIR=${SC_DEPS_PREFIX}/include
            -DNGHTTP2_LIBRARY=${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}nghttp2${CMAKE_STATIC_LIBRARY_SUFFIX}
        )
    else()
        set(curl_http2_args -DUSE_NGHTTP2=OFF)
    endif()
    set(curl_depends)
    if(TARGET sc_ep_wolfssl)
        list(APPEND curl_depends sc_ep_wolfssl)
    endif()
    if(TARGET sc_ep_mbedtls)
        list(APPEND curl_depends sc_ep_mbedtls)
    endif()
    if(TARGET sc_ep_nghttp2)
        list(APPEND curl_depends sc_ep_nghttp2)
    endif()
    if(curl_depends)
        set(curl_depends_args DEPENDS ${curl_depends})
    else()
        set(curl_depends_args)
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_curl
        ${curl_depends_args}
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG "${SC_CURL_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DCMAKE_PREFIX_PATH=${SC_DEPS_PREFIX}
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_STATIC_LIBS=ON
            -DBUILD_CURL_EXE=ON
            -DBUILD_EXAMPLES=OFF
            -DBUILD_TESTING=OFF
            -DBUILD_LIBCURL_DOCS=OFF
            -DBUILD_MISC_DOCS=OFF
            -DENABLE_CURL_MANUAL=OFF
            -DENABLE_THREADED_RESOLVER=OFF
            -DHTTP_ONLY=ON
            -DCURL_CA_BUNDLE=/etc/ssl/certs/ca-certificates.crt
            -DCURL_CA_PATH=none
            ${curl_http2_args}
            -DUSE_NGTCP2=OFF
            ${curl_tls_args}
            -DCURL_USE_OPENSSL=OFF
            -DCURL_USE_GNUTLS=OFF
            -DCURL_ZLIB=OFF
            -DCURL_BROTLI=OFF
            -DCURL_ZSTD=OFF
            -DUSE_LIBIDN2=OFF
            -DCURL_USE_LIBPSL=OFF
            -DCURL_USE_LIBSSH2=OFF
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/libcurl${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_curl)

    set(link_libs ${curl_tls_link_libs})
    if(SC_ENABLE_CURL_HTTP2)
        list(APPEND link_libs SC::nghttp2)
    endif()
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND link_libs ${CMAKE_DL_LIBS})
    endif()
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::libcurl
        "${SC_DEPS_PREFIX}/lib/libcurl${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
        COMPILE_DEFINITIONS CURL_STATICLIB
    )
    set(SC_LIBCURL_KNOWN_AWS_SIGV4 TRUE CACHE INTERNAL "GitHub curl tag supports AWS SigV4 options.")
    if(SC_ENABLE_CURL_HTTP2)
        set(SC_LIBCURL_KNOWN_HTTP2 TRUE CACHE INTERNAL "GitHub curl was configured with nghttp2.")
    endif()
endfunction()

function(sc_add_github_libsodium)
    if(TARGET SC::libsodium)
        return()
    endif()
    if(NOT SC_MAKE_PROGRAM)
        message(FATAL_ERROR "GitHub libsodium bootstrap requires make or gmake in PATH.")
    endif()

    ExternalProject_Add(sc_ep_libsodium
        GIT_REPOSITORY https://github.com/jedisct1/libsodium.git
        GIT_TAG "${SC_LIBSODIUM_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${SC_DEPS_PREFIX} --libdir=${SC_DEPS_PREFIX}/lib --disable-shared --enable-static --with-pic
        BUILD_COMMAND "${SC_MAKE_PROGRAM}"
        INSTALL_COMMAND "${SC_MAKE_PROGRAM}" install
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}sodium${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_libsodium)

    sc_add_imported_static(
        SC::libsodium
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}sodium${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
    )
endfunction()

function(sc_add_github_cmark)
    if(TARGET SC::cmark)
        return()
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_cmark
        GIT_REPOSITORY https://github.com/commonmark/cmark.git
        GIT_TAG "${SC_CMARK_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_TESTING=OFF
            -DCMARK_LIB_FUZZER=OFF
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cmark${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_cmark)

    sc_add_imported_static(
        SC::cmark
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}cmark${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        COMPILE_DEFINITIONS CMARK_STATIC_DEFINE
    )
endfunction()

function(sc_add_github_opus)
    if(TARGET SC::opus)
        return()
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_opus
        GIT_REPOSITORY https://github.com/xiph/opus.git
        GIT_TAG "${SC_OPUS_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
            -DOPUS_BUILD_SHARED_LIBRARY=OFF
            -DOPUS_BUILD_TESTING=OFF
            -DOPUS_BUILD_PROGRAMS=OFF
            -DBUILD_TESTING=OFF
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}opus${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_opus)

    set(link_libs)
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::opus
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}opus${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_add_github_sqlite)
    if(TARGET SC::sqlite3)
        return()
    endif()
    if(NOT SC_MAKE_PROGRAM)
        message(FATAL_ERROR "GitHub SQLite bootstrap requires make or gmake in PATH.")
    endif()

    ExternalProject_Add(sc_ep_sqlite
        GIT_REPOSITORY https://github.com/sqlite/sqlite.git
        GIT_TAG "${SC_SQLITE_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}"
                "CFLAGS=-fPIC"
                <SOURCE_DIR>/configure
                    --prefix=${SC_DEPS_PREFIX}
                    --libdir=${SC_DEPS_PREFIX}/lib
                    --disable-shared
                    --enable-static
                    --enable-fts5
                    --disable-readline
        BUILD_COMMAND "${SC_MAKE_PROGRAM}" sqlite3.c sqlite3.h libsqlite3.a
        INSTALL_COMMAND "${SC_MAKE_PROGRAM}" install
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}sqlite3${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_sqlite)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND link_libs ${CMAKE_DL_LIBS})
    endif()
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::sqlite3
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}sqlite3${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_add_github_isocline)
    if(TARGET SC::isocline)
        return()
    endif()

    sc_common_external_cmake_args(common_args)
    ExternalProject_Add(sc_ep_isocline
        GIT_REPOSITORY https://github.com/daanx/isocline.git
        GIT_TAG "${SC_ISOCLINE_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            ${common_args}
            -DBUILD_SHARED_LIBS=OFF
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}isocline${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_isocline)

    sc_add_imported_static(
        SC::isocline
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}isocline${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
    )
endfunction()

function(sc_wamr_platform out)
    if(APPLE)
        set("${out}" "darwin" PARENT_SCOPE)
    elseif(UNIX)
        set("${out}" "linux" PARENT_SCOPE)
    else()
        message(FATAL_ERROR "SC_ENABLE_WASM_PLUGINS=ON does not know a WAMR platform for this host.")
    endif()
endfunction()

function(sc_add_github_wamr)
    if(TARGET SC::wamr)
        return()
    endif()

    sc_wamr_platform(wamr_platform)
    set(wamr_source_dir "${CMAKE_BINARY_DIR}/sc_ep_wamr-prefix/src/sc_ep_wamr")
    ExternalProject_Add(sc_ep_wamr
        GIT_REPOSITORY https://github.com/bytecodealliance/wasm-micro-runtime.git
        GIT_TAG "${SC_WAMR_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        SOURCE_DIR "${wamr_source_dir}"
        PATCH_COMMAND
            "${CMAKE_COMMAND}" -E make_directory <SOURCE_DIR>/sc-vmlib
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wamr-vmlib/CMakeLists.txt"
                <SOURCE_DIR>/sc-vmlib/CMakeLists.txt
        SOURCE_SUBDIR sc-vmlib
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_INSTALL_PREFIX=${SC_DEPS_PREFIX}"
            "-DCMAKE_INSTALL_LIBDIR=lib"
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
            "-DWAMR_BUILD_PLATFORM=${wamr_platform}"
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}vmlib${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_wamr)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND link_libs ${CMAKE_DL_LIBS})
    endif()
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::wamr
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}vmlib${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include/wamr"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_add_github_cpython)
    if(TARGET SC::cpython)
        return()
    endif()
    if(NOT SC_MAKE_PROGRAM)
        message(FATAL_ERROR "GitHub CPython bootstrap requires make or gmake in PATH.")
    endif()

    ExternalProject_Add(sc_ep_cpython
        GIT_REPOSITORY https://github.com/python/cpython.git
        GIT_TAG "${SC_CPYTHON_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}"
                "CFLAGS=-fPIC"
                <SOURCE_DIR>/configure
                    --prefix=${SC_DEPS_PREFIX}
                    --libdir=${SC_DEPS_PREFIX}/lib
                    --disable-shared
                    --without-ensurepip
        BUILD_COMMAND "${SC_MAKE_PROGRAM}"
        INSTALL_COMMAND "${SC_MAKE_PROGRAM}" install
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}python3.14${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_cpython)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND link_libs ${CMAKE_DL_LIBS})
    endif()
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()
    if(SC_UTIL_LIBRARY)
        list(APPEND link_libs "${SC_UTIL_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::cpython
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}python3.14${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include/python3.14"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_add_github_mimalloc)
    if(TARGET SC::mimalloc)
        return()
    endif()

    ExternalProject_Add(sc_ep_mimalloc
        GIT_REPOSITORY https://github.com/microsoft/mimalloc.git
        GIT_TAG "${SC_MIMALLOC_TAG}"
        GIT_SHALLOW TRUE
        UPDATE_DISCONNECTED TRUE
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_INSTALL_PREFIX=${SC_DEPS_PREFIX}"
            "-DCMAKE_INSTALL_LIBDIR=lib"
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
            -DBUILD_SHARED_LIBS=OFF
            -DMI_BUILD_SHARED=OFF
            -DMI_BUILD_STATIC=ON
            -DMI_BUILD_OBJECT=OFF
            -DMI_BUILD_TESTS=OFF
            -DMI_INSTALL_TOPLEVEL=ON
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mimalloc${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_mimalloc)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(SC_RT_LIBRARY)
        list(APPEND link_libs "${SC_RT_LIBRARY}")
    endif()
    if(SC_ATOMIC_LIBRARY)
        list(APPEND link_libs "${SC_ATOMIC_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::mimalloc
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}mimalloc${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
        COMPILE_DEFINITIONS MI_STATIC_LIB
    )
endfunction()

function(sc_add_github_jemalloc)
    if(TARGET SC::jemalloc)
        return()
    endif()
    if(NOT SC_MAKE_PROGRAM)
        message(FATAL_ERROR "GitHub jemalloc bootstrap requires make or gmake in PATH.")
    endif()

    ExternalProject_Add(sc_ep_jemalloc
        URL "https://github.com/jemalloc/jemalloc/releases/download/${SC_JEMALLOC_TAG}/jemalloc-${SC_JEMALLOC_TAG}.tar.bz2"
        URL_HASH "SHA256=${SC_JEMALLOC_SHA256}"
        UPDATE_DISCONNECTED TRUE
        BUILD_IN_SOURCE TRUE
        CONFIGURE_COMMAND
            "${CMAKE_COMMAND}" -E env
                "CC=${CMAKE_C_COMPILER}"
                "CFLAGS=-fPIC"
                <SOURCE_DIR>/configure
                    --prefix=${SC_DEPS_PREFIX}
                    --libdir=${SC_DEPS_PREFIX}/lib
                    --disable-shared
                    --enable-static
                    --disable-cxx
                    --disable-doc
        BUILD_COMMAND "${SC_MAKE_PROGRAM}"
        INSTALL_COMMAND "${SC_MAKE_PROGRAM}" install
        BUILD_BYPRODUCTS "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jemalloc${CMAKE_STATIC_LIBRARY_SUFFIX}"
    )
    add_dependencies(sc_external_deps sc_ep_jemalloc)

    set(link_libs)
    if(Threads_FOUND)
        list(APPEND link_libs Threads::Threads)
    endif()
    if(CMAKE_DL_LIBS)
        list(APPEND link_libs ${CMAKE_DL_LIBS})
    endif()
    if(SC_MATH_LIBRARY)
        list(APPEND link_libs "${SC_MATH_LIBRARY}")
    endif()

    sc_add_imported_static(
        SC::jemalloc
        "${SC_DEPS_PREFIX}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}jemalloc${CMAKE_STATIC_LIBRARY_SUFFIX}"
        INCLUDE_DIRS "${SC_DEPS_PREFIX}/include"
        LINK_LIBRARIES ${link_libs}
    )
endfunction()

function(sc_resolve_allocator_dependencies)
    sc_provider_allows_system(try_system)
    sc_provider_allows_github(try_github)

    if(SC_ENABLE_MIMALLOC)
        set(found FALSE)
        if(try_system)
            sc_try_system_mimalloc(found)
        endif()
        if(NOT found AND try_github)
            sc_add_github_mimalloc()
            set(found TRUE)
        endif()
        if(found)
            set(SC_MIMALLOC_TARGET SC::mimalloc CACHE INTERNAL "mimalloc target")
        else()
            message(FATAL_ERROR "SC_ENABLE_MIMALLOC=ON requires mimalloc development files or SC_DEPS_PROVIDER=github/auto.")
        endif()
    endif()

    if(SC_ENABLE_JEMALLOC)
        set(found FALSE)
        if(try_system)
            sc_try_system_jemalloc(found)
        endif()
        if(NOT found AND try_github)
            sc_add_github_jemalloc()
            set(found TRUE)
        endif()
        if(found)
            set(SC_JEMALLOC_TARGET SC::jemalloc CACHE INTERNAL "jemalloc target")
        else()
            message(FATAL_ERROR "SC_ENABLE_JEMALLOC=ON requires jemalloc development files or SC_DEPS_PROVIDER=github/auto.")
        endif()
    endif()
endfunction()

function(sc_resolve_external_dependencies)
    sc_validate_deps_provider()

    if(NOT TARGET sc_external_deps)
        add_custom_target(sc_external_deps)
    endif()

    sc_provider_allows_system(try_system)
    sc_provider_allows_github(try_github)

    foreach(dep IN ITEMS libuv wolfssl nghttp2 curl libsodium cmark opus sqlite)
        set(found FALSE)
        if(try_system)
            if(dep STREQUAL "libuv")
                sc_try_system_libuv(found)
            elseif(dep STREQUAL "curl")
                sc_try_system_curl(found)
            elseif(dep STREQUAL "wolfssl")
                sc_try_system_wolfssl(found)
            elseif(dep STREQUAL "nghttp2")
                sc_try_system_nghttp2(found)
            elseif(dep STREQUAL "libsodium")
                sc_try_system_libsodium(found)
            elseif(dep STREQUAL "cmark")
                sc_try_system_cmark(found)
            elseif(dep STREQUAL "opus")
                sc_try_system_opus(found)
            elseif(dep STREQUAL "sqlite")
                sc_try_system_sqlite(found)
            endif()
        endif()

        if(NOT found AND try_github)
            if(dep STREQUAL "libuv")
                sc_add_github_libuv()
            elseif(dep STREQUAL "curl")
                sc_add_github_curl()
            elseif(dep STREQUAL "wolfssl")
                sc_add_github_wolfssl()
            elseif(dep STREQUAL "nghttp2")
                sc_add_github_nghttp2()
            elseif(dep STREQUAL "libsodium")
                sc_add_github_libsodium()
            elseif(dep STREQUAL "cmark")
                sc_add_github_cmark()
            elseif(dep STREQUAL "opus")
                sc_add_github_opus()
            elseif(dep STREQUAL "sqlite")
                sc_add_github_sqlite()
            endif()
            set(found TRUE)
        endif()

        if(dep STREQUAL "libsodium" AND NOT found)
            message(FATAL_ERROR "libsodium is required. Install libsodium development files or use SC_DEPS_PROVIDER=github/auto.")
        endif()
        if(dep STREQUAL "libuv" AND NOT found)
            message(FATAL_ERROR "libuv is required. Install libuv development files or use SC_DEPS_PROVIDER=github/auto.")
        endif()
        if(dep STREQUAL "sqlite" AND NOT found)
            message(FATAL_ERROR "SQLite is required. Install SQLite development files or use SC_DEPS_PROVIDER=github/auto.")
        endif()
    endforeach()

    if(SC_ENABLE_WASM_PLUGINS)
        if(NOT try_github)
            message(FATAL_ERROR "SC_ENABLE_WASM_PLUGINS=ON requires SC_DEPS_PROVIDER=github or auto for WAMR.")
        endif()
        sc_add_github_wamr()
    endif()

    if(SC_ENABLE_PYTHON_PLUGINS)
        if(NOT try_github)
            message(FATAL_ERROR "SC_ENABLE_PYTHON_PLUGINS=ON requires SC_DEPS_PROVIDER=github or auto for CPython.")
        endif()
        sc_add_github_cpython()
    endif()

    set(found FALSE)
    if(try_system)
        sc_try_system_isocline(found)
    endif()
    if(NOT found AND try_github)
        sc_add_github_isocline()
        set(found TRUE)
    endif()
    if(NOT found)
        message(STATUS "isocline not found; smolclaw chat will use basic stdin editing.")
    endif()
endfunction()

sc_resolve_external_dependencies()
sc_resolve_allocator_dependencies()

function(sc_third_party_c23_compat target visibility)
    target_compile_options("${target}" ${visibility}
        "$<$<COMPILE_LANGUAGE:C>:-include>"
        "$<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/include/compat/c23_keyword_compat.h>"
    )
endfunction()

function(sc_third_party_defaults target)
    set_target_properties("${target}" PROPERTIES
        C_STANDARD 23
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    target_compile_definitions("${target}" PRIVATE
        _DEFAULT_SOURCE
        _GNU_SOURCE
        _POSIX_C_SOURCE=200809L
    )
    target_include_directories("${target}" BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/compat"
    )
    sc_third_party_c23_compat("${target}" PUBLIC)
    sc_apply_sanitizers("${target}")
endfunction()

function(sc_third_party_include target)
    foreach(path IN LISTS ARGN)
        target_include_directories("${target}" SYSTEM PUBLIC "${path}")
    endforeach()
endfunction()

function(sc_third_party_add_static target)
    add_library("${target}" STATIC ${ARGN})
    sc_third_party_defaults("${target}")
endfunction()

function(sc_register_third_party target)
    if(TARGET "${target}")
        target_link_libraries(sc_third_party INTERFACE "${target}")
        add_dependencies(sc_third_party_build "${target}")
    endif()
endfunction()

function(sc_add_third_party_dependencies)
    if(TARGET sc_third_party)
        return()
    endif()

    add_library(sc_third_party INTERFACE)
    add_custom_target(sc_third_party_build)
    sc_third_party_c23_compat(sc_third_party INTERFACE)

    if(NOT SC_ENABLE_THIRD_PARTY_DEPS)
        message(STATUS "SmolClaw vendored dependency targets are disabled.")
        return()
    endif()

    if(NOT EXISTS "${SC_THIRD_PARTY_ROOT}")
        message(STATUS "SmolClaw vendored dependency root not found: ${SC_THIRD_PARTY_ROOT}")
        return()
    endif()

    set(tp "${SC_THIRD_PARTY_ROOT}")

    if(EXISTS "${tp}/arena/src/arena.c")
        sc_third_party_add_static(sc_tp_arena "${tp}/arena/src/arena.c")
        sc_third_party_include(sc_tp_arena "${tp}/arena/include")
        sc_register_third_party(sc_tp_arena)
    endif()

    if(EXISTS "${tp}/clags/src/clags.c")
        sc_third_party_add_static(sc_tp_clags "${tp}/clags/src/clags.c")
        sc_third_party_include(sc_tp_clags "${tp}/clags/include")
        target_compile_definitions(sc_tp_clags PUBLIC SC_HAVE_CLAGS=1)
        sc_register_third_party(sc_tp_clags)
    endif()

    if(EXISTS "${tp}/hiredis/src/hiredis.c")
        sc_third_party_add_static(sc_tp_hiredis
            "${tp}/hiredis/src/alloc.c"
            "${tp}/hiredis/src/async.c"
            "${tp}/hiredis/src/dict.c"
            "${tp}/hiredis/src/hiredis.c"
            "${tp}/hiredis/src/net.c"
            "${tp}/hiredis/src/read.c"
            "${tp}/hiredis/src/sds.c"
        )
        sc_third_party_include(sc_tp_hiredis
            "${tp}/hiredis/include"
            "${tp}/hiredis/adapters"
        )
        sc_register_third_party(sc_tp_hiredis)
    endif()

    if(EXISTS "${tp}/mqtt/src/mqtt.c")
        sc_third_party_add_static(sc_tp_mqtt
            "${tp}/mqtt/src/mqtt.c"
            "${tp}/mqtt/src/mqtt_pal.c"
        )
        sc_third_party_include(sc_tp_mqtt "${tp}/mqtt/include")
        if(Threads_FOUND)
            target_link_libraries(sc_tp_mqtt PUBLIC Threads::Threads)
        endif()
        sc_register_third_party(sc_tp_mqtt)
    endif()

    if(EXISTS "${tp}/microjson/mjson.c")
        sc_third_party_add_static(sc_tp_microjson
            "${tp}/microjson/mjson.c"
            "${tp}/microjson/mjson_write.c"
        )
        sc_third_party_include(sc_tp_microjson "${tp}/microjson")
        target_compile_definitions(sc_tp_microjson PUBLIC SC_HAVE_MICROJSON=1)
        sc_register_third_party(sc_tp_microjson)
    endif()

    if(EXISTS "${tp}/nanocron/src/nanocron.c")
        sc_third_party_add_static(sc_tp_nanocron "${tp}/nanocron/src/nanocron.c")
        sc_third_party_include(sc_tp_nanocron "${tp}/nanocron/include")
        target_compile_definitions(sc_tp_nanocron PUBLIC SC_HAVE_NANOCRON=1)
        sc_register_third_party(sc_tp_nanocron)
    endif()

    if(EXISTS "${tp}/parson/src/parson.c")
        sc_third_party_add_static(sc_tp_parson "${tp}/parson/src/parson.c")
        sc_third_party_include(sc_tp_parson
            "${tp}/parson/include"
            "${tp}/parson/include/parson"
        )
        target_compile_definitions(sc_tp_parson PUBLIC SC_HAVE_PARSON=1)
        sc_register_third_party(sc_tp_parson)
    endif()

    if(EXISTS "${tp}/jsonrpc/src/jsonrpc.c")
        sc_third_party_add_static(sc_tp_jsonrpc
            "${tp}/jsonrpc/src/jsonrpc.c"
            "${tp}/jsonrpc/src/arena.c"
        )
        sc_third_party_include(sc_tp_jsonrpc "${tp}/jsonrpc/include")
        if(TARGET sc_tp_parson)
            target_link_libraries(sc_tp_jsonrpc PUBLIC sc_tp_parson)
        endif()
        target_compile_definitions(sc_tp_jsonrpc PUBLIC SC_HAVE_JSONRPC=1)
        sc_register_third_party(sc_tp_jsonrpc)
    endif()

    if(EXISTS "${tp}/psimd/psimd.h")
        add_library(sc_tp_psimd INTERFACE)
        target_include_directories(sc_tp_psimd SYSTEM INTERFACE "${tp}/psimd")
        sc_register_third_party(sc_tp_psimd)
    endif()

    if(EXISTS "${tp}/rabbitmq/src/amqp_api.c")
        sc_third_party_add_static(sc_tp_rabbitmq
            "${tp}/rabbitmq/src/amqp_api.c"
            "${tp}/rabbitmq/src/amqp_connection.c"
            "${tp}/rabbitmq/src/amqp_consumer.c"
            "${tp}/rabbitmq/src/amqp_framing.c"
            "${tp}/rabbitmq/src/amqp_mem.c"
            "${tp}/rabbitmq/src/amqp_socket.c"
            "${tp}/rabbitmq/src/amqp_table.c"
            "${tp}/rabbitmq/src/amqp_tcp_socket.c"
            "${tp}/rabbitmq/src/amqp_time.c"
            "${tp}/rabbitmq/src/amqp_url.c"
        )
        sc_third_party_include(sc_tp_rabbitmq
            "${tp}/rabbitmq/include"
            "${tp}/rabbitmq/src"
        )
        target_compile_definitions(sc_tp_rabbitmq PRIVATE HAVE_POLL=1)
        sc_register_third_party(sc_tp_rabbitmq)
    endif()

    if(EXISTS "${tp}/toml/src/toml.c")
        sc_third_party_add_static(sc_tp_toml "${tp}/toml/src/toml.c")
        sc_third_party_include(sc_tp_toml "${tp}/toml/include")
        target_compile_definitions(sc_tp_toml PUBLIC SC_HAVE_TOML=1)
        sc_register_third_party(sc_tp_toml)
    endif()

    if(EXISTS "${tp}/ulog/src/ulog.c")
        sc_third_party_add_static(sc_tp_ulog "${tp}/ulog/src/ulog.c")
        sc_third_party_include(sc_tp_ulog "${tp}/ulog/include")
        target_compile_definitions(sc_tp_ulog
            PUBLIC
                SC_HAVE_ULOG=1
                ULOG_BUILD_DYNAMIC_CONFIG=1
        )
        sc_register_third_party(sc_tp_ulog)
    endif()

    if(EXISTS "${tp}/url.c/url.c")
        sc_third_party_add_static(sc_tp_url "${tp}/url.c/url.c")
        sc_third_party_include(sc_tp_url "${tp}/url.c")
        sc_register_third_party(sc_tp_url)
    endif()

    if(EXISTS "${tp}/websocket-server/src/websocket.c")
        sc_third_party_add_static(sc_tp_websocket_protocol "${tp}/websocket-server/src/websocket.c")
        sc_third_party_include(sc_tp_websocket_protocol "${tp}/websocket-server/include")
        sc_register_third_party(sc_tp_websocket_protocol)

        if(SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER AND EXISTS "${tp}/websocket-server/src/server.c")
            sc_third_party_add_static(sc_tp_websocket_server "${tp}/websocket-server/src/server.c")
            sc_third_party_include(sc_tp_websocket_server "${tp}/websocket-server/include")
            target_link_libraries(sc_tp_websocket_server PUBLIC sc_tp_websocket_protocol SC::libuv)
            target_compile_definitions(sc_third_party INTERFACE SC_HAVE_TP_WEBSOCKET_SERVER=1)
            if(TARGET sc_external_deps)
                add_dependencies(sc_tp_websocket_server sc_external_deps)
            endif()
            sc_register_third_party(sc_tp_websocket_server)
        elseif(SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER)
            message(STATUS "Skipping websocket-server libuv adapter; server source was not found.")
        endif()
    endif()

    if(SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT AND TARGET SC::wolfssl AND EXISTS "${tp}/websocket-client/src/websocket_client.c")
        sc_third_party_add_static(sc_tp_websocket_client
            "${tp}/websocket-client/src/websocket_client.c"
            "${CMAKE_CURRENT_SOURCE_DIR}/src/deps/wolfssl_ecc_fp_compat.c"
        )
        sc_third_party_include(sc_tp_websocket_client "${tp}/websocket-client/include")
        target_link_libraries(sc_tp_websocket_client PUBLIC SC::wolfssl)
        target_compile_definitions(sc_third_party INTERFACE SC_HAVE_TP_WEBSOCKET_CLIENT=1)
        if(TARGET sc_external_deps)
            add_dependencies(sc_tp_websocket_client sc_external_deps)
        endif()
        sc_register_third_party(sc_tp_websocket_client)
    elseif(EXISTS "${tp}/websocket-client/src/websocket_client.c")
        message(STATUS "Skipping websocket-client; enable SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT after wolfSSL keyword compatibility review.")
    endif()
endfunction()
