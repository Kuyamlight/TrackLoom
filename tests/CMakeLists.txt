add_executable(trackloom_core_tests
    core_tests.cpp
)

target_link_libraries(trackloom_core_tests
    PRIVATE
        trackloom_core
)

add_test(NAME trackloom_core_tests COMMAND trackloom_core_tests)

add_executable(trackloom_app_support_tests
    app_support_tests.cpp
)

target_link_libraries(trackloom_app_support_tests
    PRIVATE
        trackloom_app_support
)

add_test(NAME trackloom_app_support_tests COMMAND trackloom_app_support_tests)

if(TARGET trackloom_juce)
    add_executable(trackloom_juce_tests
        juce_midi_output_tests.cpp
    )

    add_executable(trackloom_midi_hardware_smoke_tests
        juce_midi_hardware_smoke_tests.cpp
    )

    target_link_libraries(trackloom_juce_tests
        PRIVATE
            trackloom_juce
    )

    target_link_libraries(trackloom_midi_hardware_smoke_tests
        PRIVATE
            trackloom_juce
    )

    add_test(NAME trackloom_juce_tests COMMAND trackloom_juce_tests)
    add_test(NAME trackloom_midi_hardware_smoke_tests COMMAND trackloom_midi_hardware_smoke_tests)
    # 真实 MIDI 硬件不是普通开发环境的必需条件，默认跳过而不是失败。
    set_tests_properties(trackloom_midi_hardware_smoke_tests
        PROPERTIES
            LABELS "manual;hardware"
            SKIP_RETURN_CODE 77
    )
endif()
