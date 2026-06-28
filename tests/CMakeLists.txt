add_executable(trackloom_core_tests
    core_tests.cpp
)

target_link_libraries(trackloom_core_tests
    PRIVATE
        trackloom_core
)

add_test(NAME trackloom_core_tests COMMAND trackloom_core_tests)

if(TARGET trackloom_juce)
    add_executable(trackloom_juce_tests
        juce_midi_output_tests.cpp
    )

    target_link_libraries(trackloom_juce_tests
        PRIVATE
            trackloom_juce
    )

    add_test(NAME trackloom_juce_tests COMMAND trackloom_juce_tests)
endif()
