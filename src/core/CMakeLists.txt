add_library(trackloom_core STATIC
    AudioGain.cpp
    AudioMixer.cpp
    AudioEngine.cpp
    Project.cpp
    Command.cpp
    ProjectFile.cpp
    ProjectSerializer.cpp
    Transport.cpp
)

target_include_directories(trackloom_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(MSVC)
    target_compile_options(trackloom_core PUBLIC /utf-8)
endif()
