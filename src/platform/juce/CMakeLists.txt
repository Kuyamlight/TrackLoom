add_library(trackloom_juce STATIC
    JuceAudioHost.cpp
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

target_compile_definitions(trackloom_juce
    PUBLIC
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0
        JUCE_ASIO=0
)

if(MSVC)
    target_compile_options(trackloom_juce PUBLIC /utf-8)
endif()
