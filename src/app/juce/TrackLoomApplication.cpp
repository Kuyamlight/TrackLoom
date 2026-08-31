#include "TrackLoomMainComponent.h"
#include "TrackLoomAppInfo.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace {

std::filesystem::path appAudioSettingsPath()
{
    return trackloom::trackLoomAudioSettingsPath(trackloom::juceFileToPath(
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)));
}

trackloom::TrackLoomMainComponentDependencies makeMainComponentDependencies(
    std::function<void(std::string)> titleChanged = {})
{
    trackloom::TrackLoomMainComponentDependencies dependencies;
    dependencies.audioHost = std::make_unique<trackloom::JuceAudioHost>();
    dependencies.audioSettingsPath = appAudioSettingsPath();
    dependencies.titleChanged = std::move(titleChanged);
    return dependencies;
}

trackloom::TrackLoomMainComponentDependencies makeHiddenSmokeDependencies()
{
    auto dependencies = makeMainComponentDependencies();
    dependencies.audioHost = std::make_unique<trackloom::JuceAudioHost>(
        []() -> std::unique_ptr<juce::AudioIODeviceType> { return {}; });
    return dependencies;
}

class MainWindow final : public juce::DocumentWindow {
public:
    explicit MainWindow(juce::String windowName)
        : DocumentWindow(
              std::move(windowName),
              juce::Colour(0xff171917),
              DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, true);
        setContentOwned(
            new trackloom::TrackLoomMainComponent(
                makeMainComponentDependencies([this](std::string title) {
                    setName(trackloom::toJuceString(title));
                })),
            true);
        setResizeLimits(
            trackloom::trackLoomMainMinimumWidth,
            trackloom::trackLoomMainMinimumHeight,
            4096,
            4096);
        centreWithSize(
            trackloom::trackLoomMainDefaultWidth,
            trackloom::trackLoomMainDefaultHeight);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class TrackLoomApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override
    {
        return trackloom::toJuceString(trackloom::desktopAppInfo().applicationName);
    }

    const juce::String getApplicationVersion() override
    {
        return trackloom::toJuceString(trackloom::desktopAppInfo().applicationVersion);
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String& commandLine) override
    {
        if (commandLine.contains("--hidden-smoke-test")) {
            hiddenMainComponent_ = std::make_unique<trackloom::TrackLoomMainComponent>(
                makeHiddenSmokeDependencies());
            juce::Timer::callAfterDelay(250, [] {
                juce::JUCEApplicationBase::quit();
            });
            return;
        }
        mainWindow_ = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
        hiddenMainComponent_.reset();
        mainWindow_.reset();
    }

    void systemRequestedQuit() override
    {
        quit();
    }

    void anotherInstanceStarted(const juce::String&) override
    {
    }

private:
    std::unique_ptr<trackloom::TrackLoomMainComponent> hiddenMainComponent_;
    std::unique_ptr<MainWindow> mainWindow_;
};

}

START_JUCE_APPLICATION(TrackLoomApplication)
