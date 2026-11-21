# Native unit tests. Prefers an installed GoogleTest (vcpkg / apt / conan);
# falls back to a pinned FetchContent download.
find_package(GTest 1.14 CONFIG QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)  # match the MSVC dynamic CRT
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(mixit_native_tests
    test_bounded_queue.cpp
    test_json.cpp
    test_last_error.cpp
    test_logger.cpp
    test_engine_abi.cpp
    test_system_info.cpp
    test_time_base.cpp
    test_media_buffers.cpp
    test_test_signal.cpp
    test_session.cpp
    test_media_library.cpp
    test_file_source.cpp
    media_file_writer.cpp
    media_file_reader.cpp
    test_outputs.cpp
    test_compositor.cpp
    test_buses.cpp
    test_audio_dsp.cpp
    test_audio_mixer.cpp
    test_audio_drift.cpp
    test_audio_devices.cpp
    test_vst3_host.cpp)
target_include_directories(mixit_native_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(mixit_native_tests PRIVATE MIXIT_HAVE_FFMPEG=$<BOOL:${MIXIT_FFMPEG_FOUND}>)
target_link_libraries(mixit_native_tests PRIVATE mixit_engine_core GTest::gtest_main)
mixit_apply_compiler_options(mixit_native_tests)

# A real VST3 bundle built from the interface headers, so the plug-in host is
# tested end to end on every platform.
if(MIXIT_VST3_FOUND)
    add_library(mixit_test_gain MODULE vst3_test_plugin/mixit_test_gain.cpp)
    target_link_libraries(mixit_test_gain PRIVATE mixit_vst3_interfaces)
    set(_bundle ${CMAKE_BINARY_DIR}/vst3/MixItTestGain.vst3)
    if(WIN32)
        set(_arch x86_64-win)
        set(_suffix .vst3)
    else()
        set(_arch x86_64-linux)
        set(_suffix .so)
    endif()
    set_target_properties(mixit_test_gain PROPERTIES
        PREFIX ""
        OUTPUT_NAME MixItTestGain
        SUFFIX ${_suffix}
        CXX_VISIBILITY_PRESET hidden
        LIBRARY_OUTPUT_DIRECTORY "${_bundle}/Contents/${_arch}$<0:>"
        RUNTIME_OUTPUT_DIRECTORY "${_bundle}/Contents/${_arch}$<0:>")
    if(MIXIT_ENABLE_SANITIZERS)
        target_compile_options(mixit_test_gain PRIVATE -fsanitize=address,undefined)
        target_link_options(mixit_test_gain PRIVATE -fsanitize=address,undefined)
    elseif(MIXIT_ENABLE_TSAN)
        target_compile_options(mixit_test_gain PRIVATE -fsanitize=thread)
        target_link_options(mixit_test_gain PRIVATE -fsanitize=thread)
    endif()
    add_dependencies(mixit_native_tests mixit_test_gain)
    target_compile_definitions(mixit_native_tests PRIVATE MIXIT_TEST_VST3_BUNDLE="${_bundle}")
endif()

include(GoogleTest)
# PRE_TEST: list tests when ctest runs, not at build time, so cross builds
# (MinGW + Wine emulator) link even when the emulator is not configured.
gtest_discover_tests(mixit_native_tests DISCOVERY_MODE PRE_TEST DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 120)
