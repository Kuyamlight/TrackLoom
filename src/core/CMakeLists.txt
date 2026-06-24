add_library(trackloom_core STATIC
    Project.cpp
    Command.cpp
    ProjectSerializer.cpp
)

target_include_directories(trackloom_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(trackloom_core PUBLIC /utf-8)
endif()
