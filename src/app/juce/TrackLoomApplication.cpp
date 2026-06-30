#include "AppMidiClipActions.h"
#include "AppMidiNoteActions.h"
#include "AppPlaybackActions.h"
#include "AppProjectFileActions.h"
#include "AppRecentProjects.h"
#include "AppProjectSession.h"
#include "AppProjectStatus.h"
#include "AppTimelineStatus.h"
#include "AppTrackActions.h"
#include "AppTrackListStatus.h"
#include "AppTrackStateActions.h"
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

std::string juceStringToUtf8(const juce::String& text)
{
    return std::string(text.toRawUTF8());
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

std::filesystem::path appRecentProjectsSettingsPath()
{
    const auto settingsFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(toJuceString("TrackLoom"))
        .getChildFile(toJuceString("recent-projects.txt"));
    return juceFileToPath(settingsFile);
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

void styleSingleLineTextEditor(juce::TextEditor& editor)
{
    editor.setMultiLine(false);
    editor.setScrollbarsShown(false);
    editor.setPopupMenuEnabled(true);
    editor.setFont(juce::FontOptions(15.0f));
    editor.setColour(juce::TextEditor::backgroundColourId, juce::Colour(0xff20231f));
    editor.setColour(juce::TextEditor::textColourId, juce::Colour(0xfff2f0e8));
    editor.setColour(juce::TextEditor::outlineColourId, juce::Colour(0xff3a463c));
    editor.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colour(0xff6ccf8d));
}

class MainComponent final
    : public juce::Component
    , private juce::Timer {
public:
    explicit MainComponent(std::function<void(std::string)> titleChanged)
        : titleChanged_(std::move(titleChanged))
        , recentProjectsSettingsPath_(appRecentProjectsSettingsPath())
        , recentProjects_(trackloom::loadAppRecentProjects(recentProjectsSettingsPath_))
    {
        // 首屏现在绑定真实 AppProjectSession；后续文件选择器和时间线 UI 继续沿着这个会话入口扩展。
        titleLabel_.setFont(juce::FontOptions(30.0f, juce::Font::bold));
        titleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        statusLabel_.setFont(juce::FontOptions(16.0f));
        statusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffb7c7b3));

        playbackStatusLabel_.setFont(juce::FontOptions(15.0f));
        playbackStatusLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffc8dccb));

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

        trackNameLabel_.setText(toJuceString("轨道名称"), juce::dontSendNotification);
        trackNameLabel_.setFont(juce::FontOptions(15.0f));
        trackNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));
        styleSingleLineTextEditor(trackNameEditor_);

        targetMidiClipLabel_.setText(toJuceString("目标 MIDI 片段"), juce::dontSendNotification);
        targetMidiClipLabel_.setFont(juce::FontOptions(15.0f));
        targetMidiClipLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        targetMidiClipBox_.setTextWhenNothingSelected(toJuceString("暂无 MIDI 片段"));
        targetMidiClipBox_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff20231f));
        targetMidiClipBox_.setColour(juce::ComboBox::textColourId, juce::Colour(0xfff2f0e8));
        targetMidiClipBox_.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a463c));
        targetMidiClipBox_.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff6ccf8d));

        clipNameLabel_.setText(toJuceString("片段名称"), juce::dontSendNotification);
        clipNameLabel_.setFont(juce::FontOptions(15.0f));
        clipNameLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));
        styleSingleLineTextEditor(clipNameEditor_);

        trackListTitleLabel_.setText(toJuceString("轨道列表"), juce::dontSendNotification);
        trackListTitleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        trackListTitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        timelineTitleLabel_.setText(toJuceString("时间线片段"), juce::dontSendNotification);
        timelineTitleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        timelineTitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        recentProjectsTitleLabel_.setText(toJuceString("最近工程"), juce::dontSendNotification);
        recentProjectsTitleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        recentProjectsTitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));

        recentProjectLabel_.setText(toJuceString("最近工程"), juce::dontSendNotification);
        recentProjectLabel_.setFont(juce::FontOptions(15.0f));
        recentProjectLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        recentProjectBox_.setTextWhenNothingSelected(toJuceString("暂无最近工程"));
        recentProjectBox_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff20231f));
        recentProjectBox_.setColour(juce::ComboBox::textColourId, juce::Colour(0xfff2f0e8));
        recentProjectBox_.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a463c));
        recentProjectBox_.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff6ccf8d));

        styleReadOnlyTextEditor(trackListText_);
        styleReadOnlyTextEditor(timelineText_);
        styleReadOnlyTextEditor(recentProjectsText_);

        newProjectButton_.setButtonText(toJuceString("新建工程"));
        openProjectButton_.setButtonText(toJuceString("打开工程"));
        saveProjectButton_.setButtonText(toJuceString("保存"));
        saveAsProjectButton_.setButtonText(toJuceString("另存为"));
        addInstrumentTrackButton_.setButtonText(toJuceString("添加乐器轨"));
        playProjectButton_.setButtonText(toJuceString("播放"));
        stopProjectButton_.setButtonText(toJuceString("停止"));
        rewindProjectButton_.setButtonText(toJuceString("回到开头"));
        createMidiClipButton_.setButtonText(toJuceString("创建 MIDI 片段"));
        deleteInstrumentTrackButton_.setButtonText(toJuceString("删除乐器轨"));
        moveTrackUpButton_.setButtonText(toJuceString("上移"));
        moveTrackDownButton_.setButtonText(toJuceString("下移"));
        renameTrackButton_.setButtonText(toJuceString("重命名"));
        muteTrackButton_.setButtonText(toJuceString("静音"));
        soloTrackButton_.setButtonText(toJuceString("独奏"));
        disableTrackButton_.setButtonText(toJuceString("禁用"));
        hideTrackButton_.setButtonText(toJuceString("隐藏"));
        addMidiNoteButton_.setButtonText(toJuceString("添加默认音符"));
        deleteMidiNoteButton_.setButtonText(toJuceString("删除末尾音符"));
        duplicateMidiClipButton_.setButtonText(toJuceString("复制片段"));
        splitMidiClipButton_.setButtonText(toJuceString("拆分片段"));
        moveMidiClipLeftButton_.setButtonText(toJuceString("左移片段"));
        moveMidiClipRightButton_.setButtonText(toJuceString("右移片段"));
        deleteMidiClipButton_.setButtonText(toJuceString("删除片段"));
        renameMidiClipButton_.setButtonText(toJuceString("重命名片段"));
        openRecentProjectButton_.setButtonText(toJuceString("打开最近工程"));

        newProjectButton_.onClick = [this] { requestNewProject(); };
        openProjectButton_.onClick = [this] { chooseProjectToOpen(); };
        saveProjectButton_.onClick = [this] { saveCurrentProject(); };
        saveAsProjectButton_.onClick = [this] { chooseProjectToSaveAs(); };
        playProjectButton_.onClick = [this] { startProjectPlayback(); };
        stopProjectButton_.onClick = [this] { stopProjectPlayback(); };
        rewindProjectButton_.onClick = [this] { rewindProjectPlayback(); };
        targetTrackBox_.onChange = [this] { updateSelectedTrackFromComboBox(); };
        targetMidiClipBox_.onChange = [this] { updateSelectedMidiClipFromComboBox(); };
        recentProjectBox_.onChange = [this] { updateSelectedRecentProjectFromComboBox(); };
        addInstrumentTrackButton_.onClick = [this] { addDefaultInstrumentTrack(); };
        createMidiClipButton_.onClick = [this] { createMidiClipOnSelectedTrack(); };
        deleteInstrumentTrackButton_.onClick = [this] { deleteSelectedInstrumentTrack(); };
        moveTrackUpButton_.onClick = [this] { moveSelectedTrackUp(); };
        moveTrackDownButton_.onClick = [this] { moveSelectedTrackDown(); };
        renameTrackButton_.onClick = [this] { renameSelectedTrack(); };
        trackNameEditor_.onReturnKey = [this] { renameSelectedTrack(); };
        muteTrackButton_.onClick = [this] { toggleSelectedTrackMute(); };
        soloTrackButton_.onClick = [this] { toggleSelectedTrackSolo(); };
        disableTrackButton_.onClick = [this] { toggleSelectedTrackDisabled(); };
        hideTrackButton_.onClick = [this] { toggleSelectedTrackHidden(); };
        addMidiNoteButton_.onClick = [this] { addMidiNoteToSelectedClip(); };
        deleteMidiNoteButton_.onClick = [this] { deleteMidiNoteFromSelectedClip(); };
        duplicateMidiClipButton_.onClick = [this] { duplicateSelectedMidiClip(); };
        splitMidiClipButton_.onClick = [this] { splitSelectedMidiClip(); };
        moveMidiClipLeftButton_.onClick = [this] { moveSelectedMidiClipLeft(); };
        moveMidiClipRightButton_.onClick = [this] { moveSelectedMidiClipRight(); };
        deleteMidiClipButton_.onClick = [this] { deleteSelectedMidiClip(); };
        renameMidiClipButton_.onClick = [this] { renameSelectedMidiClip(); };
        clipNameEditor_.onReturnKey = [this] { renameSelectedMidiClip(); };
        openRecentProjectButton_.onClick = [this] { openSelectedRecentProject(); };

        addAndMakeVisible(titleLabel_);
        addAndMakeVisible(statusLabel_);
        addAndMakeVisible(playbackStatusLabel_);
        addAndMakeVisible(trackSummaryLabel_);
        addAndMakeVisible(actionLabel_);
        addAndMakeVisible(targetTrackLabel_);
        addAndMakeVisible(targetTrackBox_);
        addAndMakeVisible(trackNameLabel_);
        addAndMakeVisible(trackNameEditor_);
        addAndMakeVisible(targetMidiClipLabel_);
        addAndMakeVisible(targetMidiClipBox_);
        addAndMakeVisible(clipNameLabel_);
        addAndMakeVisible(clipNameEditor_);
        addAndMakeVisible(trackListTitleLabel_);
        addAndMakeVisible(trackListText_);
        addAndMakeVisible(timelineTitleLabel_);
        addAndMakeVisible(timelineText_);
        addAndMakeVisible(recentProjectsTitleLabel_);
        addAndMakeVisible(recentProjectLabel_);
        addAndMakeVisible(recentProjectBox_);
        addAndMakeVisible(openRecentProjectButton_);
        addAndMakeVisible(recentProjectsText_);
        addAndMakeVisible(newProjectButton_);
        addAndMakeVisible(openProjectButton_);
        addAndMakeVisible(saveProjectButton_);
        addAndMakeVisible(saveAsProjectButton_);
        addAndMakeVisible(addInstrumentTrackButton_);
        addAndMakeVisible(playProjectButton_);
        addAndMakeVisible(stopProjectButton_);
        addAndMakeVisible(rewindProjectButton_);
        addAndMakeVisible(createMidiClipButton_);
        addAndMakeVisible(deleteInstrumentTrackButton_);
        addAndMakeVisible(moveTrackUpButton_);
        addAndMakeVisible(moveTrackDownButton_);
        addAndMakeVisible(renameTrackButton_);
        addAndMakeVisible(muteTrackButton_);
        addAndMakeVisible(soloTrackButton_);
        addAndMakeVisible(disableTrackButton_);
        addAndMakeVisible(hideTrackButton_);
        addAndMakeVisible(addMidiNoteButton_);
        addAndMakeVisible(deleteMidiNoteButton_);
        addAndMakeVisible(duplicateMidiClipButton_);
        addAndMakeVisible(splitMidiClipButton_);
        addAndMakeVisible(moveMidiClipLeftButton_);
        addAndMakeVisible(moveMidiClipRightButton_);
        addAndMakeVisible(deleteMidiClipButton_);
        addAndMakeVisible(renameMidiClipButton_);

        // MainComponent 主动获取键盘焦点后，Space 键才能先交给 keyPressed 处理。
        setWantsKeyboardFocus(true);
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
        playbackStatusLabel_.setBounds(bounds.removeFromTop(28));
        bounds.removeFromTop(4);

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
        buttonRow.removeFromLeft(12);
        playProjectButton_.setBounds(buttonRow.removeFromLeft(88));
        buttonRow.removeFromLeft(12);
        stopProjectButton_.setBounds(buttonRow.removeFromLeft(88));
        buttonRow.removeFromLeft(12);
        rewindProjectButton_.setBounds(buttonRow.removeFromLeft(104));

        bounds.removeFromTop(24);
        trackSummaryLabel_.setBounds(bounds.removeFromTop(32));
        actionLabel_.setBounds(bounds.removeFromTop(32));
        bounds.removeFromTop(10);

        auto targetRow = bounds.removeFromTop(36);
        targetTrackLabel_.setBounds(targetRow.removeFromLeft(96));
        targetRow.removeFromLeft(8);
        targetTrackBox_.setBounds(targetRow.removeFromLeft(220));
        targetRow.removeFromLeft(10);
        createMidiClipButton_.setBounds(targetRow.removeFromLeft(144));
        targetRow.removeFromLeft(8);
        deleteInstrumentTrackButton_.setBounds(targetRow.removeFromLeft(112));
        targetRow.removeFromLeft(8);
        moveTrackUpButton_.setBounds(targetRow.removeFromLeft(64));
        targetRow.removeFromLeft(8);
        moveTrackDownButton_.setBounds(targetRow.removeFromLeft(64));

        bounds.removeFromTop(8);
        auto trackNameRow = bounds.removeFromTop(36);
        trackNameLabel_.setBounds(trackNameRow.removeFromLeft(96));
        trackNameRow.removeFromLeft(8);
        trackNameEditor_.setBounds(trackNameRow.removeFromLeft(220));
        trackNameRow.removeFromLeft(10);
        renameTrackButton_.setBounds(trackNameRow.removeFromLeft(88));
        trackNameRow.removeFromLeft(10);
        muteTrackButton_.setBounds(trackNameRow.removeFromLeft(64));
        trackNameRow.removeFromLeft(8);
        soloTrackButton_.setBounds(trackNameRow.removeFromLeft(64));
        trackNameRow.removeFromLeft(8);
        disableTrackButton_.setBounds(trackNameRow.removeFromLeft(64));
        trackNameRow.removeFromLeft(8);
        hideTrackButton_.setBounds(trackNameRow.removeFromLeft(64));

        bounds.removeFromTop(8);
        auto clipRow = bounds.removeFromTop(36);
        targetMidiClipLabel_.setBounds(clipRow.removeFromLeft(108));
        clipRow.removeFromLeft(10);
        targetMidiClipBox_.setBounds(clipRow.removeFromLeft(260));
        clipRow.removeFromLeft(12);
        addMidiNoteButton_.setBounds(clipRow.removeFromLeft(144));
        clipRow.removeFromLeft(12);
        deleteMidiNoteButton_.setBounds(clipRow.removeFromLeft(144));

        bounds.removeFromTop(8);
        auto clipNameRow = bounds.removeFromTop(36);
        clipNameLabel_.setBounds(clipNameRow.removeFromLeft(108));
        clipNameRow.removeFromLeft(10);
        clipNameEditor_.setBounds(clipNameRow.removeFromLeft(260));
        clipNameRow.removeFromLeft(12);
        renameMidiClipButton_.setBounds(clipNameRow.removeFromLeft(120));
        clipNameRow.removeFromLeft(12);
        duplicateMidiClipButton_.setBounds(clipNameRow.removeFromLeft(112));
        clipNameRow.removeFromLeft(12);
        splitMidiClipButton_.setBounds(clipNameRow.removeFromLeft(112));
        clipNameRow.removeFromLeft(12);
        deleteMidiClipButton_.setBounds(clipNameRow.removeFromLeft(112));

        bounds.removeFromTop(8);
        auto clipMoveRow = bounds.removeFromTop(36);
        clipMoveRow.removeFromLeft(108);
        clipMoveRow.removeFromLeft(10);
        moveMidiClipLeftButton_.setBounds(clipMoveRow.removeFromLeft(112));
        clipMoveRow.removeFromLeft(12);
        moveMidiClipRightButton_.setBounds(clipMoveRow.removeFromLeft(112));

        bounds.removeFromTop(14);
        auto columns = bounds;
        auto leftColumn = columns.removeFromLeft((columns.getWidth() - 16) / 2);
        columns.removeFromLeft(16);
        auto rightColumn = columns;

        trackListTitleLabel_.setBounds(leftColumn.removeFromTop(30));
        trackListText_.setBounds(leftColumn);
        timelineTitleLabel_.setBounds(rightColumn.removeFromTop(30));
        const auto timelineHeight = (rightColumn.getHeight() * 2) / 3;
        timelineText_.setBounds(rightColumn.removeFromTop(timelineHeight));
        rightColumn.removeFromTop(10);
        recentProjectsTitleLabel_.setBounds(rightColumn.removeFromTop(30));
        auto recentProjectRow = rightColumn.removeFromTop(34);
        recentProjectLabel_.setBounds(recentProjectRow.removeFromLeft(76));
        recentProjectRow.removeFromLeft(8);
        recentProjectBox_.setBounds(recentProjectRow.removeFromLeft(220));
        recentProjectRow.removeFromLeft(10);
        openRecentProjectButton_.setBounds(recentProjectRow.removeFromLeft(124));
        rightColumn.removeFromTop(8);
        recentProjectsText_.setBounds(rightColumn);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        // 当前只实现窗口内 Space 快捷键；全局快捷键和完整菜单属于后续阶段。
        if (key.getKeyCode() == juce::KeyPress::spaceKey) {
            toggleProjectPlayback();
            return true;
        }

        return false;
    }

private:
    void timerCallback() override
    {
        const auto feedback = trackloom::advanceAppPlaybackForUiTick(playback_, session_.project());
        if (!feedback.success) {
            lastActionMessage_ = feedback.message;
            stopTimer();
        } else if (feedback.kind == trackloom::AppPlaybackActionFeedbackKind::NoOp) {
            stopTimer();
        }

        refreshFromSession();
    }

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
        if (feedback.success) {
            recordCurrentProjectAsRecent();
        }
        refreshFromSession();
    }

    void recordCurrentProjectAsRecent()
    {
        if (!session_.currentProjectPath().has_value()) {
            return;
        }

        // 最近工程是本机偏好；写入失败只提示，不影响当前打开或保存动作。
        const auto result = trackloom::recordAndSaveAppRecentProject(
            recentProjects_,
            *session_.currentProjectPath(),
            recentProjectsSettingsPath_);
        if (result.recorded && !result.saved) {
            lastActionMessage_ += " 最近工程列表暂未写入本地设置。";
        }
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

    void updateSelectedRecentProjectFromComboBox()
    {
        const auto selectedId = recentProjectBox_.getSelectedId();
        if (selectedId <= 0
            || static_cast<std::size_t>(selectedId) > selectableRecentProjectNumbers_.size()) {
            selectedRecentProjectNumber_ = 0;
            return;
        }

        selectedRecentProjectNumber_ =
            selectableRecentProjectNumbers_[static_cast<std::size_t>(selectedId - 1)];
    }

    void startProjectPlayback()
    {
        const auto feedback = trackloom::startAppPlayback(playback_, session_.project());
        lastActionMessage_ = feedback.message;
        if (feedback.success) {
            // 当前阶段还没有真实音频设备回调；先用 UI Timer 推进可见播放头。
            // 后续接入声卡时，应改由设备 block 回调驱动播放会话。
            startTimerHz(30);
        }
        refreshFromSession();
    }

    void stopProjectPlayback()
    {
        const auto feedback = trackloom::stopAppPlayback(playback_, session_.project());
        lastActionMessage_ = feedback.message;
        if (feedback.success) {
            stopTimer();
        }
        refreshFromSession();
    }

    void toggleProjectPlayback()
    {
        const auto feedback = trackloom::toggleAppPlayback(playback_, session_.project());
        lastActionMessage_ = feedback.message;
        // Timer 只跟随成功后的真实播放状态，避免快捷键和按钮各自维护一套状态。
        if (feedback.success && playback_.isPlaying()) {
            startTimerHz(30);
        } else if (feedback.success) {
            stopTimer();
        }
        refreshFromSession();
    }

    void rewindProjectPlayback()
    {
        const auto feedback = trackloom::rewindAppPlaybackToStart(playback_, session_.project());
        lastActionMessage_ = feedback.message;
        if (feedback.success && playback_.isPlaying()) {
            startTimerHz(30);
        }
        refreshFromSession();
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

    void moveSelectedTrackUp()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再上移轨道。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveInstrumentTrackUp(session_, selectedTrackId_);
        if (feedback.success) {
            selectedTrackId_ = feedback.trackId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedTrackDown()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再下移轨道。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveInstrumentTrackDown(session_, selectedTrackId_);
        if (feedback.success) {
            selectedTrackId_ = feedback.trackId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void renameSelectedTrack()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再重命名轨道。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::renameTrackById(
            session_,
            selectedTrackId_,
            juceStringToUtf8(trackNameEditor_.getText()));
        if (feedback.success) {
            selectedTrackId_ = feedback.trackId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void setTrackStateFeedback(const trackloom::AppTrackStateActionFeedback& feedback)
    {
        lastActionMessage_ = feedback.message;
        if (feedback.success) {
            selectedTrackId_ = feedback.trackId;
        }
        refreshFromSession();
    }

    void toggleSelectedTrackMute()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再切换静音状态。";
            refreshFromSession();
            return;
        }

        setTrackStateFeedback(trackloom::toggleTrackMuted(session_, selectedTrackId_));
    }

    void toggleSelectedTrackSolo()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再切换独奏状态。";
            refreshFromSession();
            return;
        }

        setTrackStateFeedback(trackloom::toggleTrackSoloed(session_, selectedTrackId_));
    }

    void toggleSelectedTrackDisabled()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再切换禁用状态。";
            refreshFromSession();
            return;
        }

        setTrackStateFeedback(trackloom::toggleTrackDisabled(session_, selectedTrackId_));
    }

    void toggleSelectedTrackHidden()
    {
        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条乐器轨，再切换隐藏状态。";
            refreshFromSession();
            return;
        }

        setTrackStateFeedback(trackloom::toggleTrackHidden(session_, selectedTrackId_));
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

    void renameSelectedMidiClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再重命名片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::renameMidiClipById(
            session_,
            selectedMidiClipId_,
            juceStringToUtf8(clipNameEditor_.getText()));
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void splitSelectedMidiClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再拆分片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::splitMidiClipAtMidpoint(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedMidiClipLeft()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再左移片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveMidiClipLeftOneBeat(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedMidiClipRight()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再右移片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveMidiClipRightOneBeat(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void duplicateSelectedMidiClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再复制片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::duplicateMidiClipAfterItself(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
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

    void openSelectedRecentProject()
    {
        if (selectedRecentProjectNumber_ == 0) {
            lastActionMessage_ = "请先选择一个最近工程。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::openAppRecentProjectByNumber(
            session_,
            recentProjects_,
            selectedRecentProjectNumber_,
            recentProjectsSettingsPath_);
        lastActionMessage_ = feedback.message;
        if (feedback.success) {
            selectedTrackId_.clear();
            selectedMidiClipId_.clear();
        }

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
        moveTrackUpButton_.setEnabled(!selectedTrackId_.empty());
        moveTrackDownButton_.setEnabled(!selectedTrackId_.empty());
        renameTrackButton_.setEnabled(!selectedTrackId_.empty());
        muteTrackButton_.setEnabled(!selectedTrackId_.empty());
        soloTrackButton_.setEnabled(!selectedTrackId_.empty());
        disableTrackButton_.setEnabled(!selectedTrackId_.empty());
        hideTrackButton_.setEnabled(!selectedTrackId_.empty());
        syncTrackNameEditorFromSelection(selectedTrackId_ != previousSelection);
    }

    void syncTrackNameEditorFromSelection(bool forceUpdate)
    {
        trackNameEditor_.setEnabled(!selectedTrackId_.empty());

        const auto selectedTrack = session_.project().findTrackById(selectedTrackId_);
        if (!selectedTrack.has_value()) {
            trackNameEditor_.setText(juce::String{}, false);
            return;
        }

        // 播放 Timer 会定期刷新界面；用户正在输入时不能把文本框重置回旧名称。
        if (forceUpdate || !trackNameEditor_.hasKeyboardFocus(true)) {
            trackNameEditor_.setText(toJuceString(selectedTrack->name), false);
        }
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
        duplicateMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
        splitMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiClipLeftButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiClipRightButton_.setEnabled(!selectedMidiClipId_.empty());
        deleteMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
        renameMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
        syncClipNameEditorFromSelection(selectedMidiClipId_ != previousSelection);
    }

    void syncClipNameEditorFromSelection(bool forceUpdate)
    {
        clipNameEditor_.setEnabled(!selectedMidiClipId_.empty());

        const auto selectedClip = session_.project().findClipById(selectedMidiClipId_);
        if (!selectedClip.has_value()) {
            clipNameEditor_.setText(juce::String{}, false);
            return;
        }

        // 播放 Timer 和列表刷新不应覆盖用户正在输入但尚未提交的片段名称。
        if (forceUpdate || !clipNameEditor_.hasKeyboardFocus(true)) {
            clipNameEditor_.setText(toJuceString(selectedClip->name), false);
        }
    }

    void refreshRecentProjectSelector(const trackloom::AppRecentProjectsStatus& recentStatus)
    {
        const auto previousSelection = selectedRecentProjectNumber_;
        selectableRecentProjectNumbers_.clear();
        recentProjectBox_.clear(juce::dontSendNotification);

        int itemId = 1;
        int selectedItemId = 0;
        for (const auto& row : recentStatus.rows) {
            selectableRecentProjectNumbers_.push_back(row.number);
            recentProjectBox_.addItem(toJuceString(row.displayName), itemId);

            if (row.number == previousSelection) {
                selectedItemId = itemId;
            }

            ++itemId;
        }

        if (selectedItemId == 0 && !selectableRecentProjectNumbers_.empty()) {
            selectedItemId = 1;
            selectedRecentProjectNumber_ = selectableRecentProjectNumbers_.front();
        } else if (selectedItemId > 0) {
            selectedRecentProjectNumber_ = previousSelection;
        } else {
            selectedRecentProjectNumber_ = 0;
        }

        recentProjectBox_.setSelectedId(selectedItemId, juce::dontSendNotification);
        recentProjectBox_.setEnabled(!selectableRecentProjectNumbers_.empty());
        openRecentProjectButton_.setEnabled(!selectableRecentProjectNumbers_.empty());
    }

    void refreshFromSession()
    {
        const auto status = trackloom::describeAppProjectSession(session_);
        const auto playbackStatus = trackloom::describeAppPlayback(playback_);
        const auto timelineStatus = trackloom::describeAppTimeline(session_.project());
        const auto recentStatus = trackloom::describeAppRecentProjects(recentProjects_);
        refreshTrackTargetSelector();
        refreshMidiClipTargetSelector(timelineStatus);
        refreshRecentProjectSelector(recentStatus);

        titleLabel_.setText(toJuceString(status.windowTitle), juce::dontSendNotification);
        statusLabel_.setText(toJuceString(status.statusLine), juce::dontSendNotification);
        playbackStatusLabel_.setText(toJuceString(playbackStatus.summary), juce::dontSendNotification);
        trackSummaryLabel_.setText(toJuceString(trackSummaryText(status)), juce::dontSendNotification);
        actionLabel_.setText(toJuceString(lastActionMessage_), juce::dontSendNotification);
        playProjectButton_.setEnabled(!playback_.isPlaying());
        stopProjectButton_.setEnabled(playback_.isPlaying());
        rewindProjectButton_.setEnabled(playback_.currentSample() > 0);
        trackListText_.setText(
            toJuceString(trackListText(trackloom::describeAppTrackList(session_.project()))),
            false);
        timelineText_.setText(
            toJuceString(timelineText(timelineStatus)),
            false);
        recentProjectsText_.setText(
            toJuceString(recentProjectsText(recentStatus)),
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

    static std::string recentProjectsText(const trackloom::AppRecentProjectsStatus& status)
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
                + "  " + row.displayName
                + "\n    " + row.fullPath;
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
    trackloom::AppPlaybackController playback_;
    std::function<void(std::string)> titleChanged_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    std::filesystem::path recentProjectsSettingsPath_;
    trackloom::AppRecentProjects recentProjects_;
    std::vector<std::string> selectableTrackIds_;
    std::vector<std::string> selectableMidiClipIds_;
    std::vector<std::size_t> selectableRecentProjectNumbers_;
    std::string lastActionMessage_ = "文件动作：尚未打开或保存工程。";
    std::string selectedTrackId_;
    std::string selectedMidiClipId_;
    std::size_t selectedRecentProjectNumber_ = 0;
    juce::Label titleLabel_;
    juce::Label statusLabel_;
    juce::Label playbackStatusLabel_;
    juce::Label trackSummaryLabel_;
    juce::Label actionLabel_;
    juce::Label targetTrackLabel_;
    juce::ComboBox targetTrackBox_;
    juce::Label trackNameLabel_;
    juce::TextEditor trackNameEditor_;
    juce::Label targetMidiClipLabel_;
    juce::ComboBox targetMidiClipBox_;
    juce::Label clipNameLabel_;
    juce::TextEditor clipNameEditor_;
    juce::Label trackListTitleLabel_;
    juce::TextEditor trackListText_;
    juce::Label timelineTitleLabel_;
    juce::TextEditor timelineText_;
    juce::Label recentProjectsTitleLabel_;
    juce::Label recentProjectLabel_;
    juce::ComboBox recentProjectBox_;
    juce::TextButton openRecentProjectButton_;
    juce::TextEditor recentProjectsText_;
    juce::TextButton newProjectButton_;
    juce::TextButton openProjectButton_;
    juce::TextButton saveProjectButton_;
    juce::TextButton saveAsProjectButton_;
    juce::TextButton addInstrumentTrackButton_;
    juce::TextButton playProjectButton_;
    juce::TextButton stopProjectButton_;
    juce::TextButton rewindProjectButton_;
    juce::TextButton createMidiClipButton_;
    juce::TextButton deleteInstrumentTrackButton_;
    juce::TextButton moveTrackUpButton_;
    juce::TextButton moveTrackDownButton_;
    juce::TextButton renameTrackButton_;
    juce::TextButton muteTrackButton_;
    juce::TextButton soloTrackButton_;
    juce::TextButton disableTrackButton_;
    juce::TextButton hideTrackButton_;
    juce::TextButton addMidiNoteButton_;
    juce::TextButton deleteMidiNoteButton_;
    juce::TextButton duplicateMidiClipButton_;
    juce::TextButton splitMidiClipButton_;
    juce::TextButton moveMidiClipLeftButton_;
    juce::TextButton moveMidiClipRightButton_;
    juce::TextButton deleteMidiClipButton_;
    juce::TextButton renameMidiClipButton_;
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
