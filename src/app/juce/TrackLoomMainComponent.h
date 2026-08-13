#pragma once

#include "AppAudioSettings.h"
#include "AppCommandDispatcher.h"
#include "AppPlaybackActions.h"
#include "AudioSettingsComponent.h"
#include "JuceAudioHost.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace trackloom {

juce::String toJuceString(std::string_view text);
std::filesystem::path juceFileToPath(const juce::File& file);
std::filesystem::path trackLoomAudioSettingsPath(
    const std::filesystem::path& userApplicationDataDirectory);

inline constexpr auto mainNewButtonComponentId = "trackloom-main-new";
inline constexpr auto mainOpenButtonComponentId = "trackloom-main-open";
inline constexpr auto mainSaveButtonComponentId = "trackloom-main-save";
inline constexpr auto mainPlayButtonComponentId = "trackloom-main-play";
inline constexpr auto mainPlaybackStatusComponentId = "trackloom-main-playback-status";

struct TrackLoomMainComponentDependencies {
    std::unique_ptr<JuceAudioHost> audioHost;
    AppPreparedPlanBuildOperation buildOperation = buildPreparedMidiPlaybackPlan;
    std::filesystem::path audioSettingsPath;
    AppAudioSettingsLoadOperation loadAudioSettings = loadAppAudioSettings;
    AppAudioSettingsSaveOperation saveAudioSettings = saveAppAudioSettings;
    std::function<void(std::string)> titleChanged;
    std::function<void(std::unique_ptr<AudioSettingsComponent>)>
        presentAudioSettings;
};

class TrackLoomMainComponent final
    : public juce::Component
    , public juce::MenuBarModel
    , private juce::Timer
    , private juce::KeyListener {
public:
    explicit TrackLoomMainComponent(TrackLoomMainComponentDependencies dependencies);
    ~TrackLoomMainComponent() override;

    AppCommandDispatchResult dispatchCommand(int commandId);
    void serviceUiTimer();
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int index, const juce::String& name) override;
    void menuItemSelected(int commandId, int topLevelMenuIndex) override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyPressed(const juce::KeyPress& key, juce::Component* origin) override;
    void resized() override;

private:
    void timerCallback() override;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
