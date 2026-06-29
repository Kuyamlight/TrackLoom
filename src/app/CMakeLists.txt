add_library(trackloom_app_support STATIC
    AppProjectFileActions.cpp
    AppProjectSession.cpp
    AppProjectStatus.cpp
    AppTrackListStatus.cpp
    TrackLoomAppInfo.cpp
)

target_include_directories(trackloom_app_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(trackloom_app_support
    PUBLIC
        TRACKLOOM_APP_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(trackloom_app_support
    PUBLIC
        trackloom_core
)

if(MSVC)
    target_compile_options(trackloom_app_support PUBLIC /utf-8)
endif()

if(TARGET trackloom_juce)
    add_executable(trackloom_app WIN32
        juce/TrackLoomApplication.cpp
    )

    target_link_libraries(trackloom_app
        PRIVATE
            trackloom_app_support
            trackloom_juce
            juce::juce_gui_basics
    )

    target_compile_definitions(trackloom_app
        PRIVATE
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
    )

    if(MSVC)
        target_compile_options(trackloom_app PRIVATE /utf-8)
    endif()
endif()
