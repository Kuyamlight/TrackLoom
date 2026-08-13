add_executable(trackloom_core_tests
    core_tests.cpp
)

target_link_libraries(trackloom_core_tests
    PRIVATE
        trackloom_core
)

add_test(NAME trackloom_core_tests COMMAND trackloom_core_tests)
set_tests_properties(trackloom_core_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

add_test(NAME trackloom_audio_fixture_hash_tests
    COMMAND ${CMAKE_COMMAND}
        -DFIXTURE_DIR=${CMAKE_SOURCE_DIR}/tests/fixtures/audio/minimum-audible-midi
        -P ${CMAKE_SOURCE_DIR}/tests/verify_audio_fixture_hashes.cmake)

add_test(NAME trackloom_audio_fixture_manifest_tests
    COMMAND ${CMAKE_COMMAND}
        -DVERIFY_AUDIO_FIXTURE_HASHES_SELF_TEST=1
        -DREFERENCE_FIXTURE_DIR=${CMAKE_SOURCE_DIR}/tests/fixtures/audio/minimum-audible-midi
        -P ${CMAKE_SOURCE_DIR}/tests/verify_audio_fixture_hashes.cmake)

add_executable(trackloom_app_support_tests
    app_support_tests.cpp
)

target_link_libraries(trackloom_app_support_tests
    PRIVATE
        trackloom_app_support
)

add_test(NAME trackloom_app_support_tests COMMAND trackloom_app_support_tests)

if(TARGET trackloom_app)
    add_test(NAME trackloom_app_hidden_smoke_tests
        COMMAND trackloom_app --hidden-smoke-test)
    set_tests_properties(trackloom_app_hidden_smoke_tests
        PROPERTIES TIMEOUT 10)
endif()

if(TARGET trackloom_juce)
    add_executable(trackloom_app_juce_tests
        app_juce_audio_tests.cpp
    )

    add_executable(trackloom_juce_audio_tests
        juce_audio_tests.cpp
    )

    add_executable(trackloom_juce_tests
        juce_midi_output_tests.cpp
    )

    add_executable(trackloom_midi_hardware_smoke_tests
        juce_midi_hardware_smoke_tests.cpp
    )

    add_executable(trackloom_audio_hardware_smoke_tests
        juce_audio_hardware_smoke_tests.cpp
    )

    target_link_libraries(trackloom_juce_audio_tests
        PRIVATE
            trackloom_juce
    )

    target_link_libraries(trackloom_juce_tests
        PRIVATE
            trackloom_juce
    )

    target_link_libraries(trackloom_midi_hardware_smoke_tests
        PRIVATE
            trackloom_juce
    )

    target_link_libraries(trackloom_audio_hardware_smoke_tests
        PRIVATE
            trackloom_juce
    )

    target_link_libraries(trackloom_app_juce_tests
        PRIVATE
            trackloom_app_juce_support
    )

    if(MSVC)
        target_compile_options(trackloom_app_juce_tests PRIVATE /utf-8)
    endif()

    add_test(NAME trackloom_app_juce_tests COMMAND trackloom_app_juce_tests)
    set_tests_properties(trackloom_app_juce_tests PROPERTIES TIMEOUT 20)
    add_test(NAME trackloom_juce_audio_tests COMMAND trackloom_juce_audio_tests)
    add_test(NAME trackloom_juce_tests COMMAND trackloom_juce_tests)
    add_test(NAME trackloom_midi_hardware_smoke_tests COMMAND trackloom_midi_hardware_smoke_tests)
    # 真实 MIDI 硬件不是普通开发环境的必需条件，默认跳过而不是失败。
    set_tests_properties(trackloom_midi_hardware_smoke_tests
        PROPERTIES
            LABELS "manual;hardware"
            SKIP_RETURN_CODE 77
    )
    add_test(NAME trackloom_audio_hardware_smoke_tests COMMAND trackloom_audio_hardware_smoke_tests)
    set_tests_properties(trackloom_audio_hardware_smoke_tests
        PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
            LABELS "manual;hardware"
            SKIP_RETURN_CODE 77
            TIMEOUT 720
    )
    add_test(NAME trackloom_audio_hardware_smoke_completion_tests
        COMMAND trackloom_audio_hardware_smoke_tests)
    set_tests_properties(trackloom_audio_hardware_smoke_completion_tests
        PROPERTIES
            ENVIRONMENT "TRACKLOOM_AUDIO_HARDWARE_SMOKE_SELF_TEST=1"
    )
endif()
