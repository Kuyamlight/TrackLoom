add_library(trackloom_juce STATIC
    JuceMidiOutput.cpp
    JuceMidiOutputDeviceManager.cpp
)

target_include_directories(trackloom_juce
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(trackloom_juce
    PUBLIC
        trackloom_core
        juce::juce_audio_devices
)

if(MSVC)
    target_compile_options(trackloom_juce PUBLIC /utf-8)
endif()
