#include "AppProjectFileActions.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
#include "TrackLoomAppInfo.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

juce::String toJuceString(std::string_view text)
{
    return juce::String::fromUTF8(text.data(), static_cast<int>(text.size()));
}

std::filesystem::path juceFileToPath(const juce::File& file)
{
    const auto fullPath = file.getFullPathName();
    const auto* rawUtf8 = fullPath.toRawUTF8();
    const auto length = std::strlen(rawUtf8);
    const auto* begin = reinterpret_cast<const char8_t*>(rawUtf8);
    return std::filesystem::path(std::u8string(begin, begin + length));
}

juce::String pathToJuceString(const std::filesystem::path& path)
{
    const auto utf8Path = path.u8string();
    return juce::String::fromUTF8(
        reinterpret_cast<const char*>(utf8Path.data()),
        static_cast<int>(utf8Path.size()));
}

class MainComponent final : public juce::Component {
public:
    explicit MainComponent(std::function<void(std::string)> titleChanged)
        : titleChanged_(std::move(titleChanged))
    {
        // 首屏现在绑定真实 AppProjectSession；后续文件选择器和时间线 UI 继续沿着这个会话入口扩展。
        titleLabel_.setFont(juce::FontOptions(30.0f, juce::Font::bold));
        titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        statusLabel_.setFont(juce::FontOptions(16.0f));
        statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffb7c7b3));

        trackSummaryLabel_.setFont(juce::FontOptions(16.0f));
        trackSummaryLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        actionLabel_.setFont(juce::FontOptions(15.0f));
        actionLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffcfc7b1));

        newProjectButton_.setButtonText(toJuceString("新建工程"));
        openProjectButton_.setButtonText(toJuceString("打开工程"));
        saveProjectButton_.setButtonText(toJuceString("保存"));
        saveAsProjectButton_.setButtonText(toJuceString("另存为"));
        addInstrumentTrackButton_.setButtonText(toJuceString("添加乐器轨"));

        newProjectButton_.onClick = [this] { requestNewProject(); };
        openProjectButton_.onClick = [this] { chooseProjectToOpen(); };
        saveProjectButton_.onClick = [this] { saveCurrentProject(); };
        saveAsProjectButton_.onClick = [this] { chooseProjectToSaveAs(); };

        addInstrumentTrackButton_.onClick = [this] {
            const auto trackNumber = session_.project().tracks().size() + 1;
            session_.editProject().createTrack(
                "Instrument " + std::to_string(trackNumber),
                trackloom::TrackType::Instrument);
            lastActionMessage_ = "已添加乐器轨。";
            refreshFromSession();
        };

        addAndMakeVisible(titleLabel_);
        addAndMakeVisible(statusLabel_);
        addAndMakeVisible(trackSummaryLabel_);
        addAndMakeVisible(actionLabel_);
        addAndMakeVisible(newProjectButton_);
        addAndMakeVisible(openProjectButton_);
        addAndMakeVisible(saveProjectButton_);
        addAndMakeVisible(saveAsProjectButton_);
        addAndMakeVisible(addInstrumentTrackButton_);

        refreshFromSession();
        setSize(1040, 680);
    }

    void paint(juce::Graphics& graphics) override
    {
        // 深色中性底色减少长时间编辑疲劳；绿色细线呼应 MIDI/播放状态，不做装饰性渐变。
        graphics.fillAll(juce::Colour(0xff171917));
        graphics.setColour(juce::Colour(0xff6ccf8d));
        graphics.fillRect(0, 0, getWidth(), 4);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(40);
        titleLabel_.setBounds(bounds.removeFromTop(48));
        statusLabel_.setBounds(bounds.removeFromTop(36));

        auto buttonRow = bounds.removeFromTop(44);
        newProjectButton_.setBounds(buttonRow.removeFromLeft(120));
        buttonRow.removeFromLeft(12);
        openProjectButton_.setBounds(buttonRow.removeFromLeft(120));
        buttonRow.removeFromLeft(12);
        saveProjectButton_.setBounds(buttonRow.removeFromLeft(88));
        buttonRow.removeFromLeft(12);
        saveAsProjectButton_.setBounds(buttonRow.removeFromLeft(104));
        buttonRow.removeFromLeft(12);
        addInstrumentTrackButton_.setBounds(buttonRow.removeFromLeft(140));

        bounds.removeFromTop(24);
        trackSummaryLabel_.setBounds(bounds.removeFromTop(32));
        actionLabel_.setBounds(bounds.removeFromTop(32));
    }

private:
    void requestNewProject()
    {
        if (session_.isDirty()) {
            lastActionMessage_ = "当前工程有未保存修改，请先保存或另存为，再新建工程。";
            refreshFromSession();
            return;
        }

        session_.createNewProject("Untitled");
        lastActionMessage_ = "已新建空白工程。";
        refreshFromSession();
    }

    void chooseProjectToOpen()
    {
        if (session_.isDirty()) {
            lastActionMessage_ = "当前工程有未保存修改，请先保存或另存为，再打开其他工程。";
            refreshFromSession();
            return;
        }

        // FileChooser 必须活到异步回调结束；成员指针保证弹窗生命周期不短于回调。
        fileChooser_ = std::make_unique<juce::FileChooser>(
            toJuceString("打开 TrackLoom 工程"),
            juce::File{},
            toJuceString("*.trackloom"),
            true,
            false,
            this);

        const juce::Component::SafePointer<MainComponent> safeThis(this);
        fileChooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis != nullptr) {
                    safeThis->finishOpenProjectChoice(chooser);
                }
            });
    }

    void saveCurrentProject()
    {
        if (!session_.currentProjectPath().has_value()) {
            chooseProjectToSaveAs();
            return;
        }

        setFileActionFeedback(trackloom::describeAppProjectFileActionResult(
            trackloom::AppProjectFileAction::Save,
            session_.save()));
    }

    void chooseProjectToSaveAs()
    {
        fileChooser_ = std::make_unique<juce::FileChooser>(
            toJuceString("保存 TrackLoom 工程"),
            initialSaveFile(),
            toJuceString("*.trackloom"),
            true,
            false,
            this);

        const juce::Component::SafePointer<MainComponent> safeThis(this);
        fileChooser_->launchAsync(
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [safeThis](const juce::FileChooser& chooser) {
                if (safeThis != nullptr) {
                    safeThis->finishSaveProjectChoice(chooser);
                }
            });
    }

    void finishOpenProjectChoice(const juce::FileChooser& chooser)
    {
        const auto selectedFile = chooser.getResult();
        if (selectedFile.getFullPathName().isEmpty()) {
            setFileActionFeedback(trackloom::describeCanceledAppProjectFileAction(
                trackloom::AppProjectFileAction::Open));
            return;
        }

        setFileActionFeedback(trackloom::describeAppProjectFileActionResult(
            trackloom::AppProjectFileAction::Open,
            session_.openFrom(juceFileToPath(selectedFile))));
    }

    void finishSaveProjectChoice(const juce::FileChooser& chooser)
    {
        const auto selectedFile = chooser.getResult();
        if (selectedFile.getFullPathName().isEmpty()) {
            setFileActionFeedback(trackloom::describeCanceledAppProjectFileAction(
                trackloom::AppProjectFileAction::SaveAs));
            return;
        }

        const auto path = trackloom::withTrackLoomProjectExtension(juceFileToPath(selectedFile));
        setFileActionFeedback(trackloom::describeAppProjectFileActionResult(
            trackloom::AppProjectFileAction::SaveAs,
            session_.saveAs(path)));
    }

    juce::File initialSaveFile() const
    {
        if (session_.currentProjectPath().has_value()) {
            return juce::File(pathToJuceString(*session_.currentProjectPath()));
        }

        return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory)
            .getChildFile(toJuceString("Untitled.trackloom"));
    }

    void setFileActionFeedback(const trackloom::AppProjectFileActionFeedback& feedback)
    {
        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void refreshFromSession()
    {
        const auto status = trackloom::describeAppProjectSession(session_);

        titleLabel_.setText(toJuceString(status.windowTitle), juce::dontSendNotification);
        statusLabel_.setText(toJuceString(status.statusLine), juce::dontSendNotification);
        trackSummaryLabel_.setText(toJuceString(trackSummaryText(status)), juce::dontSendNotification);
        actionLabel_.setText(toJuceString(lastActionMessage_), juce::dontSendNotification);

        if (titleChanged_) {
            titleChanged_(status.windowTitle);
        }
    }

    static std::string trackSummaryText(const trackloom::AppProjectStatus& status)
    {
        if (status.trackCount == 0) {
            return "轨道区：暂无轨道。点击“添加乐器轨”创建第一条 MIDI 乐器轨。";
        }

        return "轨道区：当前工程已有 " + std::to_string(status.trackCount)
            + " 条轨道；下一步会接入轨道列表和时间线编辑。";
    }

    trackloom::AppProjectSession session_;
    std::function<void(std::string)> titleChanged_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    std::string lastActionMessage_ = "文件动作：尚未打开或保存工程。";
    juce::Label titleLabel_;
    juce::Label statusLabel_;
    juce::Label trackSummaryLabel_;
    juce::Label actionLabel_;
    juce::TextButton newProjectButton_;
    juce::TextButton openProjectButton_;
    juce::TextButton saveProjectButton_;
    juce::TextButton saveAsProjectButton_;
    juce::TextButton addInstrumentTrackButton_;
};

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
            new MainComponent([this](std::string title) {
                setName(toJuceString(title));
            }),
            true);
        centreWithSize(getWidth(), getHeight());
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
        return toJuceString(trackloom::desktopAppInfo().applicationName);
    }

    const juce::String getApplicationVersion() override
    {
        return toJuceString(trackloom::desktopAppInfo().applicationVersion);
    }

    bool moreThanOneInstanceAllowed() override
    {
        return true;
    }

    void initialise(const juce::String&) override
    {
        mainWindow_ = std::make_unique<MainWindow>(getApplicationName());
    }

    void shutdown() override
    {
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
    std::unique_ptr<MainWindow> mainWindow_;
};

}

START_JUCE_APPLICATION(TrackLoomApplication)
