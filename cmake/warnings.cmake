include(CheckCCompilerFlag)

function(sc_target_add_supported_c_flag target flag)
    string(MAKE_C_IDENTIFIER "SC_HAS_C_FLAG_${flag}" flag_var)
    check_c_compiler_flag("${flag}" "${flag_var}")
    if(${flag_var})
        target_compile_options("${target}" PRIVATE "$<$<COMPILE_LANGUAGE:C>:${flag}>")
    endif()
endfunction()

function(sc_apply_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        foreach(flag IN ITEMS
            -Wall
            -Wextra
            -Wpedantic
            -Werror
            -Wconversion
            -Wshadow
            -Wstrict-prototypes
            -Wmissing-prototypes
            -Wstring-compare
        )
            sc_target_add_supported_c_flag("${target}" "${flag}")
        endforeach()
    endif()
endfunction()
