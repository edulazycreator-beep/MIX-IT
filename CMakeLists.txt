cmake_minimum_required(VERSION 3.25)

project(MixIt
    VERSION 0.6.0
    DESCRIPTION "MIX IT - Professional Live Production & Broadcast Studio (native engine)"
    LANGUAGES CXX)

if(PROJECT_IS_TOP_LEVEL)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
    # One output folder so the .NET projects can find the engine binary.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
endif()

option(MIXIT_BUILD_TESTS "Build native unit tests (GoogleTest)" ON)
option(MIXIT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)
option(MIXIT_ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)
option(MIXIT_ENABLE_TSAN "Enable ThreadSanitizer (GCC/Clang; exclusive with MIXIT_ENABLE_SANITIZERS)" OFF)

if(MIXIT_ENABLE_SANITIZERS AND MIXIT_ENABLE_TSAN)
    message(FATAL_ERROR "MIXIT_ENABLE_SANITIZERS and MIXIT_ENABLE_TSAN are mutually exclusive")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "MIX IT targets 64-bit platforms only")
endif()

include(cmake/MixItCompilerOptions.cmake)
include(cmake/MixItFfmpeg.cmake)
include(cmake/MixItVst3.cmake)
include(cmake/MixItGraphics.cmake)

add_subdirectory(native)

if(MIXIT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(native/tests)
endif()
