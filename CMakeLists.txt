cmake_minimum_required(VERSION 3.22)

project(TrackLoom VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src/core)

option(TRACKLOOM_ENABLE_JUCE_ADAPTERS "Build JUCE-based platform adapters" ON)
set(TRACKLOOM_JUCE_ROOT "" CACHE PATH "Path to the JUCE source tree")

if(NOT TRACKLOOM_JUCE_ROOT AND DEFINED ENV{JUCE_ROOT})
    set(TRACKLOOM_JUCE_ROOT "$ENV{JUCE_ROOT}" CACHE PATH "Path to the JUCE source tree" FORCE)
endif()

if(TRACKLOOM_ENABLE_JUCE_ADAPTERS)
    if(NOT TRACKLOOM_JUCE_ROOT)
        message(FATAL_ERROR "TRACKLOOM_JUCE_ROOT or JUCE_ROOT is required when TRACKLOOM_ENABLE_JUCE_ADAPTERS is ON")
    endif()

    if(NOT EXISTS "${TRACKLOOM_JUCE_ROOT}/modules/juce_audio_devices/juce_audio_devices.h")
        message(FATAL_ERROR "TRACKLOOM_JUCE_ROOT does not point to a valid JUCE source tree: ${TRACKLOOM_JUCE_ROOT}")
    endif()

    set(JUCE_MODULES_ONLY ON CACHE BOOL "Only configure JUCE modules needed by TrackLoom" FORCE)
    add_subdirectory("${TRACKLOOM_JUCE_ROOT}" "${CMAKE_BINARY_DIR}/juce" EXCLUDE_FROM_ALL)
    add_subdirectory(src/platform/juce)
endif()

add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
