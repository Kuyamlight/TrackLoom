cmake_minimum_required(VERSION 3.20)

project(TrackLoom VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(src/core)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
