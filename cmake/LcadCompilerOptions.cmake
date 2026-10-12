# Warning flags for LCAD's own targets. Third-party headers (Qt, OCCT) are included as SYSTEM and
# are not affected.
function(lcad_target_defaults target)
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
endfunction()
