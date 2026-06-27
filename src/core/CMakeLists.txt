add_library(trackloom_core STATIC
    AudioDisable.cpp
    AudioGain.cpp
    AudioPan.cpp
    AudioMute.cpp
    AudioProjectGraph.cpp
    AudioMixer.cpp
    AudioSolo.cpp
    AudioTrackPlayback.cpp
    AudioEngine.cpp
    AudioEngineMidiBridge.cpp
    MidiDispatch.cpp
    MidiPlayback.cpp
    MidiOutputSession.cpp
    MidiTrackRouter.cpp
    PlaybackClock.cpp
    Project.cpp
    ProjectMidiOutputGraph.cpp
    ProjectPlaybackSession.cpp
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
