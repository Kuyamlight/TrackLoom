add_executable(trackloom_core_tests
    core_tests.cpp
)

target_link_libraries(trackloom_core_tests
    PRIVATE
        trackloom_core
)

add_test(NAME trackloom_core_tests COMMAND trackloom_core_tests)
