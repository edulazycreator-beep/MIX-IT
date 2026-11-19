# VST3 plug-in interfaces (Steinberg "pluginterfaces", MIT licence since 3.8).
#
# Only the interface headers and three small .cpp files that define the base
# interface IDs are used; MIX IT implements its own host on top of them (no
# VST3 SDK "hosting" helpers). Pinned to an exact commit (tag
# v3.8.1_build_84). Use MIXIT_VST3_INTERFACES_DIR to point at a local checkout
# (offline builds); the directory must be named "pluginterfaces".

option(MIXIT_ENABLE_VST3 "Build the VST3 plug-in host" ON)
set(MIXIT_VST3_INTERFACES_DIR "" CACHE PATH "Local checkout of steinbergmedia/vst3_pluginterfaces (optional)")

set(MIXIT_VST3_FOUND FALSE)
if(MIXIT_ENABLE_VST3)
    if(MIXIT_VST3_INTERFACES_DIR)
        set(_vst3_dir "${MIXIT_VST3_INTERFACES_DIR}")
    else()
        include(FetchContent)
        FetchContent_Declare(vst3_pluginterfaces
            GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
            GIT_TAG 4f547e8e102b47de4a8b8aaf343c73b700786372  # v3.8.1_build_84
            GIT_SHALLOW FALSE
            SOURCE_DIR ${CMAKE_BINARY_DIR}/_deps/vst3/pluginterfaces)
        FetchContent_GetProperties(vst3_pluginterfaces)
        if(NOT vst3_pluginterfaces_POPULATED)
            FetchContent_MakeAvailable(vst3_pluginterfaces)
        endif()
        set(_vst3_dir "${CMAKE_BINARY_DIR}/_deps/vst3/pluginterfaces")
    endif()
    get_filename_component(_vst3_name "${_vst3_dir}" NAME)
    if(NOT EXISTS "${_vst3_dir}/base/funknown.cpp" OR NOT _vst3_name STREQUAL "pluginterfaces")
        message(FATAL_ERROR "VST3 interfaces not found at ${_vst3_dir} (set MIXIT_ENABLE_VST3=OFF to build without)")
    endif()
    get_filename_component(_vst3_include "${_vst3_dir}" DIRECTORY)

    # Third-party code: compiled with default warnings, never with -Werror.
    add_library(mixit_vst3_interfaces STATIC
        ${_vst3_dir}/base/funknown.cpp
        ${_vst3_dir}/base/coreiids.cpp
        ${CMAKE_SOURCE_DIR}/native/src/audio/vst3_iids.cpp)
    target_include_directories(mixit_vst3_interfaces SYSTEM PUBLIC ${_vst3_include})
    set_target_properties(mixit_vst3_interfaces PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
    target_compile_features(mixit_vst3_interfaces PUBLIC cxx_std_20)
    if(MIXIT_ENABLE_SANITIZERS)
        target_compile_options(mixit_vst3_interfaces PRIVATE -fsanitize=address,undefined)
    elseif(MIXIT_ENABLE_TSAN)
        target_compile_options(mixit_vst3_interfaces PRIVATE -fsanitize=thread)
    endif()
    install(FILES ${_vst3_dir}/LICENSE.txt DESTINATION share/licenses RENAME VST3_PLUGINTERFACES_LICENSE.txt)
    set(MIXIT_VST3_FOUND TRUE)
    set(MIXIT_VST3_LICENSE_FILE ${_vst3_dir}/LICENSE.txt)
endif()
message(STATUS "MIX IT: VST3 host ${MIXIT_VST3_FOUND}")
