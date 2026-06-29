#include "AppMidiClipActions.h"
#include "AppMidiNoteActions.h"
#include "AppProjectFileActions.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
#include "AppTimelineStatus.h"
#include "AppTrackActions.h"
#include "AppTrackListStatus.h"
#include "TrackLoomAppInfo.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

void styleReadOnlyTextEditor(juce::TextEditor& editor)
{
    editor.setReadOnly(true);
    editor.setMultiLine(true);
    editor.setScrollbarsShown(true);
    editor.setPopupMenuEnabled(false);
    editor.setFont(juce::FontOptions(15.0f));
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff20231f));
    editor.setColour(juce::TextEditor::textColourId, juce::Colour(0xffe4dfd0));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff3a463c));
    editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff6ccf8d));
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

        targetTrackLabel_.setText(toJuceString("目标乐器轨"), juce::dontSendNotification);
        targetTrackLabel_.setFont(juce::FontOptions(15.0f));
        targetTrackLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        targetTrackBox_.setTextWhenNothingSelected(toJuceString("暂无可用乐器轨"));
        targetTrackBox_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff20231f));
        targetTrackBox_.setColour(juce::ComboBox::textColourId, juce::Colour(0xfff2f0e8));
        targetTrackBox_.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a463c));
        targetTrackBox_.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff6ccf8d));

        targetMidiClipLabel_.setText(toJuceString("目标 MIDI 片段"), juce::dontSendNotification);
        targetMidiClipLabel_.setFont(juce::FontOptions(15.0f));
        targetMidiClipLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        targetMidiClipBox_.setTextWhenNothingSelected(toJuceString("暂无 MIDI 片段"));
        targetMidiClipBox_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff20231f));
        targetMidiClipBox_.setColour(juce::ComboBox::textColourId, juce::Colour(0xfff2f0e8));
        targetMidiClipBox_.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a463c));
        targetMidiClipBox_.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff6ccf8d));

        trackListTitleLabel_.setText(toJuceString("轨道列表"), juce::dontSendNotification);
        trackListTitleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        trackListTitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        timelineTitleLabel_.setText(toJuceString("时间线片段"), juce::dontSendNotification);
        timelineTitleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        timelineTitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        styleReadOnlyTextEditor(trackListText_);
        styleReadOnlyTextEditor(timelineText_);

        newProjectButton_.setButtonText(toJuceString("新建工程"));
        openProjectButton_.setButtonText(toJuceString("打开工程"));
        saveProjectButton_.setButtonText(toJuceString("保存"));
        saveAsProjectButton_.setButtonText(toJuceString("另存为"));
        addInstrumentTrackButton_.setButtonText(toJuceString("添加乐器轨"));
        createMidiClipButton_.setButtonText(toJuceString("创建 MIDI 片段"));
        deleteInstrumentTrackButton_.setButtonText(toJuceString("删除乐器轨"));
        addMidiNoteButton_.setButtonText(toJuceString("添加默认音符"));
        deleteMidiNoteButton_.setButtonText(toJuceString("删除末尾音符"));
        deleteMidiClipButton_.setButtonText(toJuceString("删除片段"));

        newProjectButton_.onClick = [this] { requestNewProject(); };
        openProjectButton_.onClick = [this] { chooseProjectToOpen(); };
        saveProjectButton_.onClick = [this] { saveCurrentProject(); };
        saveAsProjectButton_.onClick = [this] { chooseProjectToSaveAs(); };
        targetTrackBox_.onChange = [this] { updateSelectedTrackFromComboBox(); };
        targetMidiClipBox_.onChange = [this] { updateSelectedMidiClipFromComboBox(); };
        addInstrumentTrackButton_.onClick = [this] { addDefaultInstrumentTrack(); };
        createMidiClipButton_.onClick = [this] { createMidiClipOnSelectedTrack(); };
        deleteInstrumentTrackButton_.onClick = [this] { deleteSelectedInstrumentTrack(); };
        addMidiNoteButton_.onClick = [this] { addMidiNoteToSelectedClip(); };
        deleteMidiNoteButton_.onClick = [this] { deleteMidiNoteFromSelectedClip(); };
        deleteMidiClipButton_.onClick = [this] { deleteSelectedMidiClip(); };

        addAndMakeVisible(titleLabel_);
        addAndMakeVisible(statusLabel_);
        addAndMakeVisible(trackSummaryLabel_);
        addAndMakeVisible(actionLabel_);
        addAndMakeVisible(targetTrackLabel_);
        addAndMakeVisible(targetTrackBox_);
        addAndMakeVisible(targetMidiClipLabel_);
        addAndMakeVisible(targetMidiClipBox_);
        addAndMakeVisible(trackListTitleLabel_);
        addAndMakeVisible(trackListText_);
        addAndMakeVisible(timelineTitleLabel_);
        addAndMakeVisible(timelineText_);
        addAndMakeVisible(newProjectButton_);
        addAndMakeVisible(openProjectButton_);
        addAndMakeVisible(saveProjectButton_);
        addAndMakeVisible(saveAsProjectButton_);
        addAndMakeVisible(addInstrumentTrackButton_);
        addAndMakeVisible(createMidiClipButton_);
        addAndMakeVisible(deleteInstrumentTrackButton_);
        addAndMakeVisible(addMidiNoteButton_);
        addAndMakeVisible(deleteMidiNoteButton_);
        addAndMakeVisible(deleteMidiClipButton_);

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
        bounds.removeFromTop(10);

        auto targetRow = bounds.removeFromTop(36);
        targetTrackLabel_.setBounds(targetRow.removeFromLeft(96));
        targetRow.removeFromLeft(10);
        targetTrackBox_.setBounds(targetRow.removeFromLeft(260));
        targetRow.removeFromLeft(12);
        createMidiClipButton_.setBounds(targetRow.removeFromLeft(160));
        targetRow.removeFromLeft(12);
        deleteInstrumentTrackButton_.setBounds(targetRow.removeFromLeft(140));

        bounds.removeFromTop(8);
        auto clipRow = bounds.removeFromTop(36);
        targetMidiClipLabel_.setBounds(clipRow.removeFromLeft(108));
        clipRow.removeFromLeft(10);
        targetMidiClipBox_.setBounds(clipRow.removeFromLeft(260));
        clipRow.removeFromLeft(12);
        addMidiNoteButton_.setBounds(clipRow.removeFromLeft(144));
        clipRow.removeFromLeft(12);
        deleteMidiNoteButton_.setBounds(clipRow.removeFromLeft(144));
        clipRow.removeFromLeft(12);
        deleteMidiClipButton_.setBounds(clipRow.removeFromLeft(112));

        bounds.removeFromTop(14);
        auto columns = bounds;
        auto leftColumn = columns.removeFromLeft((columns.getWidth() - 16) / 2);
        columns.removeFromLeft(16);
        auto rightColumn = columns;

        trackListTitleLabel_.setBounds(leftColumn.removeFromTop(30));
        trackListText_.setBounds(leftColumn);
        timelineTitleLabel_.setBounds(rightColumn.removeFromTop(30));
        timelineText_.setBounds(rightColumn);
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

    void updateSelectedTrackFromComboBox()
    {
        const auto selectedId = targetTrackBox_.getSelectedId();
        if (selectedId <= 0
            || static_cast<std::size_t>(selectedId) > selectableTrackIds_.size()) {
            selectedTrackId_.clear();
            return;
        }

        selectedTrackId_ = selectableTrackIds_[static_cast<std::size_t>(selectedId - 1)];
    }

    void updateSelectedMidiClipFromComboBox()
    {
        const auto selectedId = targetMidiClipBox_.getSelectedId();
        if (selectedId <= 0
            || static_cast<std::size_t>(selectedId) > selectableMidiClipIds_.size()) {
            selectedMidiClipId_.clear();
            return;
        }

        selectedMidiClipId_ = selectableMidiClipIds_[static_cast<std::size_t>(selectedId - 1)];
    }

    void addDefaultInstrumentTrack()
    {
        const auto feedback = trackloom::createDefaultInstrumentTrack(session_);
        if (feedback.success) {
            selectedTrackId_ = feedback.trackId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void createMidiClipOnSelectedTrack()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先添加并选择一条乐器轨，再创建 MIDI 片段。";
            refreshFromSession();
            return;
        }

        const auto targetTrackId = selectedTrackId_;
        const auto feedback = trackloom::createDefaultMidiClipOnTrack(session_, targetTrackId);
        if (feedback.success) {
            selectedTrackId_ = targetTrackId;
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void deleteSelectedInstrumentTrack()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再删除乐器轨。";
            refreshFromSession();
            return;
        }

        const auto targetTrackId = selectedTrackId_;
        const auto feedback = trackloom::deleteInstrumentTrackById(session_, targetTrackId);
        if (feedback.success && selectedTrackId_ == targetTrackId) {
            selectedTrackId_.clear();
            selectedMidiClipId_.clear();
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void addMidiNoteToSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先创建并选择一个 MIDI 片段，再添加默认音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::createDefaultMidiNoteInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void deleteMidiNoteFromSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再删除末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::deleteLastMidiNoteInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void deleteSelectedMidiClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再删除片段。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::deleteMidiClipById(session_, targetClipId);
        if (feedback.success && selectedMidiClipId_ == targetClipId) {
            selectedMidiClipId_.clear();
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void refreshTrackTargetSelector()
    {
        const auto previousSelection = selectedTrackId_;
        selectableTrackIds_.clear();
        targetTrackBox_.clear(juce::dontSendNotification);

        int itemId = 1;
        int selectedItemId = 0;
        for (const auto& track : session_.project().tracks()) {
            if (track.type != trackloom::TrackType::Instrument) {
                continue;
            }

            selectableTrackIds_.push_back(track.id);
            targetTrackBox_.addItem(toJuceString(track.name), itemId);

            if (track.id == previousSelection) {
                selectedItemId = itemId;
            }

            ++itemId;
        }

        if (selectedItemId == 0 && !selectableTrackIds_.empty()) {
            selectedItemId = 1;
            selectedTrackId_ = selectableTrackIds_.front();
        } else if (selectedItemId > 0) {
            selectedTrackId_ = previousSelection;
        } else {
            selectedTrackId_.clear();
        }

        targetTrackBox_.setSelectedId(selectedItemId, juce::dontSendNotification);
        targetTrackBox_.setEnabled(!selectableTrackIds_.empty());
        createMidiClipButton_.setEnabled(!selectedTrackId_.empty());
        deleteInstrumentTrackButton_.setEnabled(!selectedTrackId_.empty());
    }

    void refreshMidiClipTargetSelector(const trackloom::AppTimelineStatus& timelineStatus)
    {
        const auto previousSelection = selectedMidiClipId_;
        selectableMidiClipIds_.clear();
        targetMidiClipBox_.clear(juce::dontSendNotification);

        int itemId = 1;
        int selectedItemId = 0;
        for (const auto& row : timelineStatus.rows) {
            if (row.type != trackloom::ClipType::Midi) {
                continue;
            }

            selectableMidiClipIds_.push_back(row.clipId);
            targetMidiClipBox_.addItem(
                toJuceString(row.name + " - " + row.trackName),
                itemId);

            if (row.clipId == previousSelection) {
                selectedItemId = itemId;
            }

            ++itemId;
        }

        if (selectedItemId == 0 && !selectableMidiClipIds_.empty()) {
            selectedItemId = 1;
            selectedMidiClipId_ = selectableMidiClipIds_.front();
        } else if (selectedItemId > 0) {
            selectedMidiClipId_ = previousSelection;
        } else {
            selectedMidiClipId_.clear();
        }

        targetMidiClipBox_.setSelectedId(selectedItemId, juce::dontSendNotification);
        targetMidiClipBox_.setEnabled(!selectableMidiClipIds_.empty());
        addMidiNoteButton_.setEnabled(!selectedMidiClipId_.empty());
        deleteMidiNoteButton_.setEnabled(!selectedMidiClipId_.empty());
        deleteMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
    }

    void refreshFromSession()
    {
        const auto status = trackloom::describeAppProjectSession(session_);
        const auto timelineStatus = trackloom::describeAppTimeline(session_.project());
        refreshTrackTargetSelector();
        refreshMidiClipTargetSelector(timelineStatus);

        titleLabel_.setText(toJuceString(status.windowTitle), juce::dontSendNotification);
        statusLabel_.setText(toJuceString(status.statusLine), juce::dontSendNotification);
        trackSummaryLabel_.setText(toJuceString(trackSummaryText(status)), juce::dontSendNotification);
        actionLabel_.setText(toJuceString(lastActionMessage_), juce::dontSendNotification);
        trackListText_.setText(
            toJuceString(trackListText(trackloom::describeAppTrackList(session_.project()))),
            false);
        timelineText_.setText(
            toJuceString(timelineText(timelineStatus)),
            false);

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
            + " 条轨道；可创建 MIDI 片段并添加默认音符。";
    }

    static std::string trackListText(const trackloom::AppTrackListStatus& status)
    {
        if (status.rows.empty()) {
            return status.emptyMessage;
        }

        std::string text;
        for (const auto& row : status.rows) {
            if (!text.empty()) {
                text += "\n";
            }

            text += trackNumberText(row.number)
                + "  " + row.name
                + "  [" + row.typeLabel + "]  "
                + row.summary;
        }
        return text;
    }

    static std::string timelineText(const trackloom::AppTimelineStatus& status)
    {
        if (status.rows.empty()) {
            return status.emptyMessage;
        }

        std::string text;
        for (const auto& row : status.rows) {
            if (!text.empty()) {
                text += "\n";
            }

            text += trackNumberText(row.number)
                + "  " + row.name
                + "  [" + row.typeLabel + "]  "
                + row.summary;
        }
        return text;
    }

    static std::string trackNumberText(std::size_t number)
    {
        if (number < 10) {
            return "0" + std::to_string(number);
        }

        return std::to_string(number);
    }

    trackloom::AppProjectSession session_;
    std::function<void(std::string)> titleChanged_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    std::vector<std::string> selectableTrackIds_;
    std::vector<std::string> selectableMidiClipIds_;
    std::string lastActionMessage_ = "文件动作：尚未打开或保存工程。";
    std::string selectedTrackId_;
    std::string selectedMidiClipId_;
    juce::Label titleLabel_;
    juce::Label statusLabel_;
    juce::Label trackSummaryLabel_;
    juce::Label actionLabel_;
    juce::Label targetTrackLabel_;
    juce::ComboBox targetTrackBox_;
    juce::Label targetMidiClipLabel_;
    juce::ComboBox targetMidiClipBox_;
    juce::Label trackListTitleLabel_;
    juce::TextEditor trackListText_;
    juce::Label timelineTitleLabel_;
    juce::TextEditor timelineText_;
    juce::TextButton newProjectButton_;
    juce::TextButton openProjectButton_;
    juce::TextButton saveProjectButton_;
    juce::TextButton saveAsProjectButton_;
    juce::TextButton addInstrumentTrackButton_;
    juce::TextButton createMidiClipButton_;
    juce::TextButton deleteInstrumentTrackButton_;
    juce::TextButton addMidiNoteButton_;
    juce::TextButton deleteMidiNoteButton_;
    juce::TextButton deleteMidiClipButton_;
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
