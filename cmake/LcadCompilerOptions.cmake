# SPDX-License-Identifier: GPL-3.0-or-later

# Compiler settings for LCAD's own targets. Third-party headers (Qt, OCCT) are included as
# SYSTEM and are not held to the warning flags.
function(lcad_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            # The sources are UTF-8, with characters such as ° and ² in their strings.
            /utf-8
            /permissive-
            /Zc:__cplusplus
            /W4)
        # windows.h, which OCCT's headers and LCAD's include, would otherwise define min and
        # max as macros.
        target_compile_definitions(${target} PRIVATE NOMINMAX)
        if(LCAD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wnon-virtual-dtor
            -Woverloaded-virtual)
        if(LCAD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()
