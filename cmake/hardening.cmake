include(CheckCCompilerFlag)
include(CheckCSourceCompiles)
include(CheckLinkerFlag)

function(sc_apply_hardening target)
    if(NOT SC_HARDENING)
        return()
    endif()

    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        check_c_compiler_flag("-fstack-protector-strong" SC_HAS_STACK_PROTECTOR_STRONG)
        if(SC_HAS_STACK_PROTECTOR_STRONG)
            target_compile_options("${target}" PRIVATE -fstack-protector-strong)
        endif()

        check_c_compiler_flag("-fPIE" SC_HAS_FPIE)
        if(SC_HAS_FPIE)
            target_compile_options("${target}" PRIVATE -fPIE)
            target_link_options("${target}" PRIVATE -pie)
        endif()

        check_c_compiler_flag("-fvisibility=hidden" SC_HAS_HIDDEN_VISIBILITY)
        if(SC_HAS_HIDDEN_VISIBILITY)
            target_compile_options("${target}" PRIVATE -fvisibility=hidden)
        endif()

        set(SC_PREVIOUS_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
        set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -O2")
        check_c_source_compiles("
            #ifndef _FORTIFY_SOURCE
            #error _FORTIFY_SOURCE is not predefined
            #endif
            int main() { return _FORTIFY_SOURCE; }
        " SC_COMPILER_PREDEFINES_FORTIFY_SOURCE)
        set(CMAKE_REQUIRED_FLAGS "${SC_PREVIOUS_REQUIRED_FLAGS}")
        if(NOT SC_COMPILER_PREDEFINES_FORTIFY_SOURCE)
            target_compile_definitions("${target}" PRIVATE
                $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=3>
            )
        endif()

        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            check_linker_flag(C "-Wl,-z,relro" SC_HAS_RELRO)
            if(SC_HAS_RELRO)
                target_link_options("${target}" PRIVATE -Wl,-z,relro)
            endif()

            check_linker_flag(C "-Wl,-z,now" SC_HAS_NOW)
            if(SC_HAS_NOW)
                target_link_options("${target}" PRIVATE -Wl,-z,now)
            endif()

            check_linker_flag(C "-Wl,-z,noexecstack" SC_HAS_NOEXECSTACK)
            if(SC_HAS_NOEXECSTACK)
                target_link_options("${target}" PRIVATE -Wl,-z,noexecstack)
            endif()
        endif()
    endif()
endfunction()
