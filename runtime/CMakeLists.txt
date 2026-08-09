cmake_minimum_required(VERSION 3.16)

# scripts/setup_env.sh historically passes the cross ar/ranlib as bare command
# names.  CMake may anchor such cache values to its invocation directory;
# resolve that form through PATH before generating the archive rule.
function(yoolang_resolve_archive_tool cache_variable)
    if(DEFINED ${cache_variable} AND NOT EXISTS "${${cache_variable}}")
        get_filename_component(tool_name "${${cache_variable}}" NAME)
        unset(resolved_tool CACHE)
        unset(resolved_tool)
        find_program(resolved_tool NAMES "${tool_name}")
        if(resolved_tool)
            set(${cache_variable} "${resolved_tool}" CACHE FILEPATH
                "Archive tool resolved by the yoolang runtime" FORCE)
        endif()
        unset(resolved_tool CACHE)
    endif()
endfunction()

yoolang_resolve_archive_tool(CMAKE_AR)
yoolang_resolve_archive_tool(CMAKE_RANLIB)

project(yoolang_runtime LANGUAGES C)

add_library(sysy STATIC
    alias_runtime.c
    sylib.c
    rvv_runtime.c
)

target_include_directories(sysy PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(sysy PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
    OUTPUT_NAME sysy
    UNITY_BUILD OFF
)

# The detector is intentionally its own translation unit/archive member.  It
# has no loops that benefit from vectorization, and must itself remain safe on
# a system where V is absent or disabled.
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(alias_runtime.c PROPERTIES
        COMPILE_OPTIONS "-fno-tree-vectorize"
    )
    set_source_files_properties(rvv_runtime.c PROPERTIES
        COMPILE_OPTIONS "-fno-tree-vectorize"
    )
endif()
