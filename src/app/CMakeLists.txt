add_library(trackloom_app_support STATIC
    AppAudioClipActions.cpp
    AppAudioSettings.cpp
    AppCommandDispatcher.cpp
    AppCommandPalette.cpp
    AppCommandPaletteSession.cpp
    AppCommandShortcuts.cpp
    AppShortcutSettings.cpp
    AppShortcutStatus.cpp
    AppMainMenu.cpp
    AppLoopActions.cpp
    AppTimelineCanvasStatus.cpp
    AppMidiClipActions.cpp
    AppMidiNoteActions.cpp
    AppPlaybackActions.cpp
    AppProjectReplacementActions.cpp
    AppProjectFileActions.cpp
    AppProjectSession.cpp
    AppProjectStatus.cpp
    AppRecentProjects.cpp
    AppTimelineStatus.cpp
    AppTrackActions.cpp
    AppTrackListStatus.cpp
    AppTrackStateActions.cpp
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
    add_library(trackloom_app_juce_support STATIC
        juce/AudioSettingsComponent.cpp
        juce/TrackLoomMainComponent.cpp
    )

    target_include_directories(trackloom_app_juce_support
        PUBLIC
            ${CMAKE_CURRENT_SOURCE_DIR}/juce
    )

    target_link_libraries(trackloom_app_juce_support
        PUBLIC
            trackloom_app_support
            trackloom_juce
            juce::juce_gui_basics
    )

    target_compile_definitions(trackloom_app_juce_support
        PUBLIC
            JUCE_USE_CURL=0
            JUCE_WEB_BROWSER=0
    )

    if(MSVC)
        target_compile_options(trackloom_app_juce_support PUBLIC /utf-8)
    endif()

    add_executable(trackloom_app WIN32
        juce/TrackLoomApplication.cpp
    )

    target_link_libraries(trackloom_app
        PRIVATE
            trackloom_app_juce_support
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
