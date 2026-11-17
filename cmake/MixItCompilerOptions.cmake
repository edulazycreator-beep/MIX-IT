# Shared compiler settings for every MIX IT native target.
# Usage: mixit_apply_compiler_options(<target>)

function(mixit_apply_compiler_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /MP
            # C4324 "structure was padded due to alignment specifier": the padding
            # is intentional (cache-line separation of hot atomics in lock-free queues).
            /wd4324
            $<$<BOOL:${MIXIT_WARNINGS_AS_ERRORS}>:/WX>)
        target_compile_definitions(${target} PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wnull-dereference
            $<$<BOOL:${MIXIT_WARNINGS_AS_ERRORS}>:-Werror>)
        if(WIN32)  # MinGW cross-compile check
            target_compile_definitions(${target} PRIVATE
                WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
        endif()
        if(MIXIT_ENABLE_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            target_link_options(${target} PRIVATE -fsanitize=address,undefined)
        endif()
        if(MIXIT_ENABLE_TSAN)
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
    endif()
endfunction()
