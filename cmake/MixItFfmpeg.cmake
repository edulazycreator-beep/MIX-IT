# FFmpeg discovery for the native engine (see docs/adr/0004-ffmpeg-integration-and-licensing.md).
#
#   MIXIT_WITH_FFMPEG          AUTO (default) | ON | OFF
#   MIXIT_REQUIRE_LGPL_FFMPEG  ON by default on Windows (product), OFF on dev hosts.
#   FFMPEG_ROOT                folder with include/, lib/ (import libs) and bin/ (DLLs)
#                              of a pinned LGPL shared build. Required on Windows.
#
# Result: target mixit::ffmpeg (INTERFACE) and MIXIT_FFMPEG_FOUND.

set(MIXIT_WITH_FFMPEG "AUTO" CACHE STRING "Build with FFmpeg: AUTO, ON or OFF")
set_property(CACHE MIXIT_WITH_FFMPEG PROPERTY STRINGS AUTO ON OFF)
if(WIN32)
    option(MIXIT_REQUIRE_LGPL_FFMPEG "Reject non-LGPL FFmpeg builds at runtime" ON)
else()
    option(MIXIT_REQUIRE_LGPL_FFMPEG "Reject non-LGPL FFmpeg builds at runtime" OFF)
endif()
set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}" CACHE PATH "FFmpeg LGPL shared build (include/, lib/, bin/)")

set(MIXIT_FFMPEG_FOUND FALSE)
set(MIXIT_FFMPEG_DLLS "")

if(NOT MIXIT_WITH_FFMPEG STREQUAL "OFF")
    if(FFMPEG_ROOT)
        find_path(MIXIT_FFMPEG_INCLUDE_DIR libavcodec/avcodec.h PATHS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
        set(_libs "")
        foreach(_lib avcodec avformat avutil)
            find_library(MIXIT_FFMPEG_${_lib}_LIBRARY NAMES ${_lib} PATHS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
            list(APPEND _libs ${MIXIT_FFMPEG_${_lib}_LIBRARY})
        endforeach()
        if(MIXIT_FFMPEG_INCLUDE_DIR AND MIXIT_FFMPEG_avcodec_LIBRARY AND MIXIT_FFMPEG_avformat_LIBRARY AND MIXIT_FFMPEG_avutil_LIBRARY)
            add_library(mixit::ffmpeg INTERFACE IMPORTED)
            target_include_directories(mixit::ffmpeg SYSTEM INTERFACE "${MIXIT_FFMPEG_INCLUDE_DIR}")
            target_link_libraries(mixit::ffmpeg INTERFACE ${_libs})
            file(GLOB MIXIT_FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
            set(MIXIT_FFMPEG_FOUND TRUE)
        endif()
    elseif(NOT CMAKE_CROSSCOMPILING)
        # Dev hosts: system FFmpeg via pkg-config. Never used when cross
        # compiling (it would pick up the host's libraries).
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MIXIT_FFMPEG_PC QUIET IMPORTED_TARGET libavcodec libavformat libavutil)
            if(MIXIT_FFMPEG_PC_FOUND)
                add_library(mixit::ffmpeg INTERFACE IMPORTED)
                target_link_libraries(mixit::ffmpeg INTERFACE PkgConfig::MIXIT_FFMPEG_PC)
                set(MIXIT_FFMPEG_FOUND TRUE)
            endif()
        endif()
    endif()

    if(NOT MIXIT_FFMPEG_FOUND AND MIXIT_WITH_FFMPEG STREQUAL "ON")
        message(FATAL_ERROR "MIXIT_WITH_FFMPEG=ON but FFmpeg was not found (set FFMPEG_ROOT or install pkg-config packages)")
    endif()
endif()

message(STATUS "MIX IT: FFmpeg ${MIXIT_FFMPEG_FOUND} (LGPL required at runtime: ${MIXIT_REQUIRE_LGPL_FFMPEG})")
