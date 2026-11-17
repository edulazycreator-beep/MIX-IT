# MixIt.Native — the native engine.
#
# Targets
#   mixit_engine_core  OBJECT  all engine code; its objects go into the DLL and the tests
#   mixit_engine       SHARED  mixit_engine.dll / libmixit_engine.so, exports the C ABI only

find_package(Threads REQUIRED)

set(MIXIT_CORE_SOURCES
    src/abi/engine_abi.cpp
    src/abi/media_library_abi.cpp
    src/abi/session_abi.cpp
    src/core/engine.cpp
    src/core/last_error.cpp
    src/logging/json.cpp
    src/logging/logger.cpp
    src/media/cpu_compositor.cpp
    src/media/d3d11_compositor.cpp
    src/media/capture_source_windows.cpp
    src/media/ffmpeg_library.cpp
    src/media/file_source.cpp
    src/media/session.cpp
    src/media/test_signal.cpp
    src/media/test_signal_source.cpp
    src/media/time_base.cpp
    src/platform/cpuid.cpp
)

if(WIN32)
    list(APPEND MIXIT_CORE_SOURCES
        src/platform/os_windows.cpp
        src/platform/system_info_windows.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND MIXIT_CORE_SOURCES
        src/platform/os_posix.cpp
        src/platform/system_info_linux.cpp)
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME} (Windows is the product platform, Linux a dev host)")
endif()

add_library(mixit_engine_core OBJECT ${MIXIT_CORE_SOURCES})
target_include_directories(mixit_engine_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Inside the core, MIXIT_API marks exports; the shared library re-exports them.
target_compile_definitions(mixit_engine_core
    PUBLIC  MIXIT_ENGINE_BUILDING
    PRIVATE MIXIT_BUILD_CONFIG="$<CONFIG>")
set_target_properties(mixit_engine_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(mixit_engine_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(mixit_engine_core PUBLIC dxgi d3d11 mfuuid ole32)
endif()
target_compile_definitions(mixit_engine_core PRIVATE
    MIXIT_HAVE_FFMPEG=$<BOOL:${MIXIT_FFMPEG_FOUND}>
    MIXIT_REQUIRE_LGPL_FFMPEG=$<BOOL:${MIXIT_REQUIRE_LGPL_FFMPEG}>)
if(MIXIT_FFMPEG_FOUND)
    target_link_libraries(mixit_engine_core PUBLIC mixit::ffmpeg)
endif()
mixit_apply_compiler_options(mixit_engine_core)

# The shared library. Every exported symbol is declared in
# include/mixit/mixit_engine.h; everything else is hidden.
add_library(mixit_engine SHARED src/abi/dll_entry.cpp)  # + core objects via link
target_link_libraries(mixit_engine PRIVATE mixit_engine_core)
set_target_properties(mixit_engine PROPERTIES
    OUTPUT_NAME mixit_engine
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})
if(WIN32)
    set_target_properties(mixit_engine PROPERTIES PREFIX "")  # mixit_engine.dll even with MinGW
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Template instantiations from libstdc++ headers have default visibility;
    # the version script guarantees only mixit_* is exported.
    target_link_options(mixit_engine PRIVATE "LINKER:--no-undefined"
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/abi/exports.map")
    set_property(TARGET mixit_engine APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/abi/exports.map)
endif()
if(WIN32 AND NOT MSVC)
    # MinGW: do not depend on libstdc++/libgcc DLLs at runtime.
    target_link_options(mixit_engine PRIVATE -static-libgcc -static-libstdc++ -static)
endif()
mixit_apply_compiler_options(mixit_engine)

# Ship the FFmpeg DLLs next to the engine (Windows LGPL shared build).
if(MIXIT_FFMPEG_DLLS)
    add_custom_command(TARGET mixit_engine POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${MIXIT_FFMPEG_DLLS} $<TARGET_FILE_DIR:mixit_engine>
        COMMENT "Copying FFmpeg DLLs")
    install(FILES ${MIXIT_FFMPEG_DLLS} DESTINATION bin)
endif()

install(TARGETS mixit_engine RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES include/mixit/mixit_engine.h DESTINATION include/mixit)
