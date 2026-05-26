include(CheckCCompilerFlag)

function(sc_apply_sanitizers target)
    if(NOT SC_SANITIZERS)
        return()
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options("${target}" PRIVATE
            -fsanitize=address,undefined,leak
            -fno-omit-frame-pointer
        )
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            check_c_compiler_flag(
                "-fno-sanitize-address-use-odr-indicator"
                SC_HAS_NO_ASAN_ODR_INDICATOR
            )
            if(SC_HAS_NO_ASAN_ODR_INDICATOR)
                target_compile_options("${target}" PRIVATE
                    -fno-sanitize-address-use-odr-indicator
                )
            endif()
        endif()
        target_link_options("${target}" PRIVATE
            -fsanitize=address,undefined,leak
        )
    else()
        message(WARNING "SC_SANITIZERS is not configured for ${CMAKE_C_COMPILER_ID}.")
    endif()
endfunction()
