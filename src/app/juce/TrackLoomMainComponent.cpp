#include "AppAudioClipActions.h"
#include "AppCommandDispatcher.h"
#include "AppCommandPaletteSession.h"
#include "AppCommandShortcuts.h"
#include "AppShortcutSettings.h"
#include "AppShortcutStatus.h"
#include "AppMainMenu.h"
#include "AppMidiClipActions.h"
#include "AppMidiNoteActions.h"
#include "AppPlaybackActions.h"
#include "AppProjectFileActions.h"
#include "AppRecentProjects.h"
#include "AppProjectSession.h"
#include "AppProjectReplacementActions.h"
#include "AppProjectStatus.h"
#include "AppTimelineStatus.h"
#include "AppTrackActions.h"
#include "AppTrackListStatus.h"
#include "AppTrackStateActions.h"
#include "TrackLoomAppInfo.h"
#include "JuceAudioHost.h"
#include "TrackLoomMainComponent.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace trackloom {

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

}

namespace {

using trackloom::juceFileToPath;
using trackloom::juceStringToUtf8;
using trackloom::toJuceString;

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

std::filesystem::path appShortcutSettingsPath()
{
    const auto settingsFile = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile(toJuceString("TrackLoom"))
        .getChildFile(toJuceString("shortcuts.txt"));
    return juceFileToPath(settingsFile);
}

trackloom::AppShortcutChord appShortcutChordFromKeyPress(const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();
    const auto keyCode = key.getKeyCode();

    trackloom::AppShortcutChord chord;
    if ((keyCode >= 'a' && keyCode <= 'z') || (keyCode >= 'A' && keyCode <= 'Z')) {
        chord.key = static_cast<char>(keyCode);
    }

    // JUCE 的 command modifier 在 Windows 上等同于 Ctrl，在 macOS 上等同于 Command。
    // 应用层只关心“主修饰键”，避免每个平台各写一套快捷键规则。
    chord.primaryModifier = modifiers.isCommandDown();
    chord.shift = modifiers.isShiftDown();
    chord.alt = modifiers.isAltDown();
    return chord;
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

std::string shortcutStatusDialogText(const trackloom::AppShortcutStatus& status)
{
    // 这里仅把应用层快照格式化成人类可读文本；冲突、自定义和启用状态都已由 AppShortcutStatus 判定。
    std::string text = status.summary;

    if (!status.conflicts.empty()) {
        text += "\n\n冲突：";
        for (const auto& conflict : status.conflicts) {
            text += "\n- " + conflict.shortcutLabel
                + "：已属于“" + conflict.existingCommandLabel
                + "”，未应用到“" + conflict.requestedCommandLabel + "”。";
        }
    }

    text += "\n\n当前命令：";
    for (const auto& row : status.rows) {
        text += "\n- " + row.groupName + " / " + row.label + "：";
        text += row.shortcutLabel.empty() ? "未设置" : row.shortcutLabel;
        if (row.customized) {
            text += "（自定义）";
        }
        if (!row.enabled) {
            text += "（当前不可用）";
        }
    }

    return text;
}

// CommandPaletteRowLabel 只把 JUCE 鼠标事件转成一个简单回调。
// 命令是否存在、是否可用、应该执行哪个 handler，都继续交给应用层验证。
class CommandPaletteRowLabel final : public juce::Label {
public:
    std::function<void()> clicked;
    std::function<void()> hovered;
    std::function<void(const juce::MouseWheelDetails&)> wheelMoved;

    void mouseEnter(const juce::MouseEvent& event) override
    {
        juce::Label::mouseEnter(event);

        if (hovered) {
            hovered();
        }
    }

    void mouseUp(const juce::MouseEvent& event) override
    {
        juce::Label::mouseUp(event);

        if (event.mods.isPopupMenu()) {
            return;
        }

        if (clicked) {
            clicked();
        }
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (wheelMoved) {
            wheelMoved(wheel);
            return;
        }

        juce::Label::mouseWheelMove(event, wheel);
    }
};

class OwnedAudioSettingsWindow final : public juce::DialogWindow {
public:
    explicit OwnedAudioSettingsWindow(
        std::unique_ptr<trackloom::AudioSettingsComponent> component)
        : juce::DialogWindow(
              toJuceString("TrackLoom 音频设置"),
              juce::Colour(0xff171917),
              true)
    {
        setUsingNativeTitleBar(true);
        setResizable(false, false);
        setContentOwned(component.release(), true);
        centreAroundComponent(nullptr, 520, 230);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
    }
};

class TrackLoomMainComponentImpl
    : public juce::Component
    , public juce::MenuBarModel
    , private juce::Timer
    , private juce::KeyListener {
public:
    explicit TrackLoomMainComponentImpl(
        trackloom::TrackLoomMainComponentDependencies dependencies)
        : audioHost_(std::move(dependencies.audioHost))
        , playback_(*audioHost_, std::move(dependencies.buildOperation))
        , audioSettingsPath_(std::move(dependencies.audioSettingsPath))
        , loadAudioSettings_(std::move(dependencies.loadAudioSettings))
        , saveAudioSettings_(std::move(dependencies.saveAudioSettings))
        , presentAudioSettings_(std::move(dependencies.presentAudioSettings))
        , titleChanged_(std::move(dependencies.titleChanged))
        , recentProjectsSettingsPath_(appRecentProjectsSettingsPath())
        , recentProjects_(trackloom::loadAppRecentProjects(recentProjectsSettingsPath_))
        , shortcutSettingsPath_(appShortcutSettingsPath())
        , customShortcutBindings_(trackloom::loadAppShortcutCustomBindings(shortcutSettingsPath_))
        , shortcutCustomization_(trackloom::customizeAppShortcutBindings(customShortcutBindings_))
        , menuBar_(this)
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

        targetAudioTrackLabel_.setText(toJuceString("目标音频轨"), juce::dontSendNotification);
        targetAudioTrackLabel_.setFont(juce::FontOptions(15.0f));
        targetAudioTrackLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        targetAudioTrackBox_.setTextWhenNothingSelected(toJuceString("暂无可用音频轨"));
        targetAudioTrackBox_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff20231f));
        targetAudioTrackBox_.setColour(juce::ComboBox::textColourId, juce::Colour(0xfff2f0e8));
        targetAudioTrackBox_.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a463c));
        targetAudioTrackBox_.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff6ccf8d));

        targetAudioClipLabel_.setText(toJuceString("目标音频片段"), juce::dontSendNotification);
        targetAudioClipLabel_.setFont(juce::FontOptions(15.0f));
        targetAudioClipLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffd9d4c5));

        targetAudioClipBox_.setTextWhenNothingSelected(toJuceString("暂无音频片段"));
        targetAudioClipBox_.setColour(juce::ComboBox::backgroundColourId, juce::Colour(0xff20231f));
        targetAudioClipBox_.setColour(juce::ComboBox::textColourId, juce::Colour(0xfff2f0e8));
        targetAudioClipBox_.setColour(juce::ComboBox::outlineColourId, juce::Colour(0xff3a463c));
        targetAudioClipBox_.setColour(juce::ComboBox::arrowColourId, juce::Colour(0xff6ccf8d));
        styleSingleLineTextEditor(audioClipNameEditor_);

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

        commandPaletteBackground_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff1d211e));
        commandPalettePanel_.setText({});
        commandPalettePanel_.setColour(juce::GroupComponent::outlineColourId, juce::Colour(0xff6ccf8d));
        commandPalettePanel_.setColour(juce::GroupComponent::textColourId, juce::Colour(0xfff2f0e8));
        commandPaletteTitleLabel_.setText(toJuceString("命令面板"), juce::dontSendNotification);
        commandPaletteTitleLabel_.setFont(juce::FontOptions(18.0f, juce::Font::bold));
        commandPaletteTitleLabel_.setColour(juce::Label::textColourId, juce::Colour(0xfff2f0e8));
        commandPaletteRangeLabel_.setFont(juce::FontOptions(14.0f));
        commandPaletteRangeLabel_.setJustificationType(juce::Justification::centredRight);
        commandPaletteRangeLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffb7c7b3));
        styleSingleLineTextEditor(commandPaletteQueryEditor_);
        commandPaletteQueryEditor_.setTextToShowWhenEmpty(
            toJuceString("搜索命令"),
            juce::Colour(0xff7f8c7d));

        for (std::size_t rowIndex = 0; rowIndex < commandPaletteRowLabels_.size(); ++rowIndex) {
            auto& rowLabel = commandPaletteRowLabels_[rowIndex];
            rowLabel.setFont(juce::FontOptions(15.0f));
            rowLabel.setJustificationType(juce::Justification::centredLeft);
            rowLabel.setColour(juce::Label::textColourId, juce::Colour(0xffdfe9d8));
            rowLabel.setColour(juce::Label::backgroundColourId, juce::Colour(0xff20231f));
            rowLabel.setMouseCursor(juce::MouseCursor::PointingHandCursor);
            rowLabel.hovered = [this, rowIndex] { highlightCommandPaletteRowFromUi(rowIndex); };
            rowLabel.clicked = [this, rowIndex] { activateCommandPaletteRowFromUi(rowIndex); };
            rowLabel.wheelMoved = [this](const juce::MouseWheelDetails& wheel) {
                scrollCommandPaletteWithWheelFromUi(wheel);
            };
        }

        commandPaletteEmptyLabel_.setFont(juce::FontOptions(15.0f));
        commandPaletteEmptyLabel_.setJustificationType(juce::Justification::centredLeft);
        commandPaletteEmptyLabel_.setColour(juce::Label::textColourId, juce::Colour(0xffb7c7b3));
        commandPaletteEmptyLabel_.setColour(juce::Label::backgroundColourId, juce::Colour(0xff20231f));

        newProjectButton_.setButtonText(toJuceString("新建工程"));
        openProjectButton_.setButtonText(toJuceString("打开工程"));
        saveProjectButton_.setButtonText(toJuceString("保存"));
        saveAsProjectButton_.setButtonText(toJuceString("另存为"));
        addInstrumentTrackButton_.setButtonText(toJuceString("添加乐器轨"));
        addAudioTrackButton_.setButtonText(toJuceString("添加音频轨"));
        addFolderTrackButton_.setButtonText(toJuceString("添加文件夹"));
        playProjectButton_.setButtonText(toJuceString("播放"));
        stopProjectButton_.setButtonText(toJuceString("停止"));
        rewindProjectButton_.setButtonText(toJuceString("回到开头"));
        newProjectButton_.setComponentID(trackloom::mainNewButtonComponentId);
        openProjectButton_.setComponentID(trackloom::mainOpenButtonComponentId);
        saveProjectButton_.setComponentID(trackloom::mainSaveButtonComponentId);
        playProjectButton_.setComponentID(trackloom::mainPlayButtonComponentId);
        playbackStatusLabel_.setComponentID(trackloom::mainPlaybackStatusComponentId);
        createMidiClipButton_.setButtonText(toJuceString("创建 MIDI 片段"));
        createAudioClipButton_.setButtonText(toJuceString("创建音频片段"));
        deleteAudioTrackButton_.setButtonText(toJuceString("删除音频轨"));
        deleteAudioClipButton_.setButtonText(toJuceString("删除音频片段"));
        renameAudioClipButton_.setButtonText(toJuceString("重命名音频片段"));
        duplicateAudioClipButton_.setButtonText(toJuceString("复制音频片段"));
        splitAudioClipButton_.setButtonText(toJuceString("拆分音频片段"));
        moveAudioClipToTrackButton_.setButtonText(toJuceString("移到音频轨"));
        moveAudioClipLeftButton_.setButtonText(toJuceString("左移音频片段"));
        moveAudioClipRightButton_.setButtonText(toJuceString("右移音频片段"));
        trimAudioClipEndButton_.setButtonText(toJuceString("缩短音频片尾"));
        extendAudioClipEndButton_.setButtonText(toJuceString("延长音频片尾"));
        trimAudioClipStartButton_.setButtonText(toJuceString("缩短音频片头"));
        extendAudioClipStartButton_.setButtonText(toJuceString("延长音频片头"));
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
        duplicateMidiNoteButton_.setButtonText(toJuceString("复制末尾音符"));
        raiseMidiNotePitchButton_.setButtonText(toJuceString("升高音符"));
        lowerMidiNotePitchButton_.setButtonText(toJuceString("降低音符"));
        increaseMidiNoteVelocityButton_.setButtonText(toJuceString("增强力度"));
        decreaseMidiNoteVelocityButton_.setButtonText(toJuceString("减弱力度"));
        lengthenMidiNoteButton_.setButtonText(toJuceString("延长音符"));
        shortenMidiNoteButton_.setButtonText(toJuceString("缩短音符"));
        moveMidiNoteEarlierButton_.setButtonText(toJuceString("左移音符"));
        moveMidiNoteLaterButton_.setButtonText(toJuceString("右移音符"));
        duplicateMidiClipButton_.setButtonText(toJuceString("复制片段"));
        splitMidiClipButton_.setButtonText(toJuceString("拆分片段"));
        moveMidiClipLeftButton_.setButtonText(toJuceString("左移片段"));
        moveMidiClipRightButton_.setButtonText(toJuceString("右移片段"));
        moveMidiClipToTrackButton_.setButtonText(toJuceString("移到目标轨"));
        trimMidiClipStartButton_.setButtonText(toJuceString("缩短片头"));
        extendMidiClipStartButton_.setButtonText(toJuceString("延长片头"));
        trimMidiClipEndButton_.setButtonText(toJuceString("缩短片尾"));
        extendMidiClipEndButton_.setButtonText(toJuceString("延长片尾"));
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
        targetAudioTrackBox_.onChange = [this] { updateSelectedAudioTrackFromComboBox(); };
        targetAudioClipBox_.onChange = [this] { updateSelectedAudioClipFromComboBox(); };
        targetMidiClipBox_.onChange = [this] { updateSelectedMidiClipFromComboBox(); };
        recentProjectBox_.onChange = [this] { updateSelectedRecentProjectFromComboBox(); };
        addInstrumentTrackButton_.onClick = [this] { addDefaultInstrumentTrack(); };
        addAudioTrackButton_.onClick = [this] { addDefaultAudioTrack(); };
        addFolderTrackButton_.onClick = [this] { addDefaultFolderTrack(); };
        createMidiClipButton_.onClick = [this] { createMidiClipOnSelectedTrack(); };
        createAudioClipButton_.onClick = [this] { createAudioClipOnSelectedAudioTrack(); };
        deleteAudioTrackButton_.onClick = [this] { deleteSelectedAudioTrack(); };
        deleteAudioClipButton_.onClick = [this] { deleteSelectedAudioClip(); };
        renameAudioClipButton_.onClick = [this] { renameSelectedAudioClip(); };
        duplicateAudioClipButton_.onClick = [this] { duplicateSelectedAudioClip(); };
        splitAudioClipButton_.onClick = [this] { splitSelectedAudioClip(); };
        moveAudioClipToTrackButton_.onClick = [this] { moveSelectedAudioClipToAudioTrack(); };
        moveAudioClipLeftButton_.onClick = [this] { moveSelectedAudioClipLeft(); };
        moveAudioClipRightButton_.onClick = [this] { moveSelectedAudioClipRight(); };
        trimAudioClipEndButton_.onClick = [this] { trimSelectedAudioClipEnd(); };
        extendAudioClipEndButton_.onClick = [this] { extendSelectedAudioClipEnd(); };
        trimAudioClipStartButton_.onClick = [this] { trimSelectedAudioClipStart(); };
        extendAudioClipStartButton_.onClick = [this] { extendSelectedAudioClipStart(); };
        audioClipNameEditor_.onReturnKey = [this] { renameSelectedAudioClip(); };
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
        duplicateMidiNoteButton_.onClick = [this] { duplicateMidiNoteInSelectedClip(); };
        raiseMidiNotePitchButton_.onClick = [this] { raiseMidiNotePitchInSelectedClip(); };
        lowerMidiNotePitchButton_.onClick = [this] { lowerMidiNotePitchInSelectedClip(); };
        increaseMidiNoteVelocityButton_.onClick = [this] { increaseMidiNoteVelocityInSelectedClip(); };
        decreaseMidiNoteVelocityButton_.onClick = [this] { decreaseMidiNoteVelocityInSelectedClip(); };
        lengthenMidiNoteButton_.onClick = [this] { lengthenMidiNoteInSelectedClip(); };
        shortenMidiNoteButton_.onClick = [this] { shortenMidiNoteInSelectedClip(); };
        moveMidiNoteEarlierButton_.onClick = [this] { moveMidiNoteEarlierInSelectedClip(); };
        moveMidiNoteLaterButton_.onClick = [this] { moveMidiNoteLaterInSelectedClip(); };
        duplicateMidiClipButton_.onClick = [this] { duplicateSelectedMidiClip(); };
        splitMidiClipButton_.onClick = [this] { splitSelectedMidiClip(); };
        moveMidiClipLeftButton_.onClick = [this] { moveSelectedMidiClipLeft(); };
        moveMidiClipRightButton_.onClick = [this] { moveSelectedMidiClipRight(); };
        moveMidiClipToTrackButton_.onClick = [this] { moveSelectedMidiClipToSelectedTrack(); };
        trimMidiClipStartButton_.onClick = [this] { trimSelectedMidiClipStart(); };
        extendMidiClipStartButton_.onClick = [this] { extendSelectedMidiClipStart(); };
        trimMidiClipEndButton_.onClick = [this] { trimSelectedMidiClipEnd(); };
        extendMidiClipEndButton_.onClick = [this] { extendSelectedMidiClipEnd(); };
        deleteMidiClipButton_.onClick = [this] { deleteSelectedMidiClip(); };
        renameMidiClipButton_.onClick = [this] { renameSelectedMidiClip(); };
        clipNameEditor_.onReturnKey = [this] { renameSelectedMidiClip(); };
        openRecentProjectButton_.onClick = [this] { openSelectedRecentProject(); };
        commandPaletteQueryEditor_.onTextChange = [this] { updateCommandPaletteQueryFromUi(); };
        commandPaletteQueryEditor_.onReturnKey = [this] { activateCommandPaletteSelectionFromUi(); };
        commandPaletteQueryEditor_.onEscapeKey = [this] { closeCommandPaletteFromUi(); };
        commandPaletteQueryEditor_.addKeyListener(this);

        addAndMakeVisible(menuBar_);
        addAndMakeVisible(titleLabel_);
        addAndMakeVisible(statusLabel_);
        addAndMakeVisible(playbackStatusLabel_);
        addAndMakeVisible(trackSummaryLabel_);
        addAndMakeVisible(actionLabel_);
        addAndMakeVisible(targetTrackLabel_);
        addAndMakeVisible(targetTrackBox_);
        addAndMakeVisible(targetAudioTrackLabel_);
        addAndMakeVisible(targetAudioTrackBox_);
        addAndMakeVisible(targetAudioClipLabel_);
        addAndMakeVisible(targetAudioClipBox_);
        addAndMakeVisible(audioClipNameEditor_);
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
        addAndMakeVisible(addAudioTrackButton_);
        addAndMakeVisible(addFolderTrackButton_);
        addAndMakeVisible(playProjectButton_);
        addAndMakeVisible(stopProjectButton_);
        addAndMakeVisible(rewindProjectButton_);
        addAndMakeVisible(createMidiClipButton_);
        addAndMakeVisible(createAudioClipButton_);
        addAndMakeVisible(deleteAudioTrackButton_);
        addAndMakeVisible(deleteAudioClipButton_);
        addAndMakeVisible(renameAudioClipButton_);
        addAndMakeVisible(duplicateAudioClipButton_);
        addAndMakeVisible(splitAudioClipButton_);
        addAndMakeVisible(moveAudioClipToTrackButton_);
        addAndMakeVisible(moveAudioClipLeftButton_);
        addAndMakeVisible(moveAudioClipRightButton_);
        addAndMakeVisible(trimAudioClipEndButton_);
        addAndMakeVisible(extendAudioClipEndButton_);
        addAndMakeVisible(trimAudioClipStartButton_);
        addAndMakeVisible(extendAudioClipStartButton_);
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
        addAndMakeVisible(duplicateMidiNoteButton_);
        addAndMakeVisible(raiseMidiNotePitchButton_);
        addAndMakeVisible(lowerMidiNotePitchButton_);
        addAndMakeVisible(increaseMidiNoteVelocityButton_);
        addAndMakeVisible(decreaseMidiNoteVelocityButton_);
        addAndMakeVisible(lengthenMidiNoteButton_);
        addAndMakeVisible(shortenMidiNoteButton_);
        addAndMakeVisible(moveMidiNoteEarlierButton_);
        addAndMakeVisible(moveMidiNoteLaterButton_);
        addAndMakeVisible(duplicateMidiClipButton_);
        addAndMakeVisible(splitMidiClipButton_);
        addAndMakeVisible(moveMidiClipLeftButton_);
        addAndMakeVisible(moveMidiClipRightButton_);
        addAndMakeVisible(moveMidiClipToTrackButton_);
        addAndMakeVisible(trimMidiClipStartButton_);
        addAndMakeVisible(extendMidiClipStartButton_);
        addAndMakeVisible(trimMidiClipEndButton_);
        addAndMakeVisible(extendMidiClipEndButton_);
        addAndMakeVisible(deleteMidiClipButton_);
        addAndMakeVisible(renameMidiClipButton_);
        addAndMakeVisible(commandPaletteBackground_);
        addAndMakeVisible(commandPalettePanel_);
        addAndMakeVisible(commandPaletteTitleLabel_);
        addAndMakeVisible(commandPaletteRangeLabel_);
        addAndMakeVisible(commandPaletteQueryEditor_);
        for (auto& rowLabel : commandPaletteRowLabels_) {
            addAndMakeVisible(rowLabel);
        }
        addAndMakeVisible(commandPaletteEmptyLabel_);

        // MainComponent 主动获取键盘焦点后，Space 键才能先交给 keyPressed 处理。
        setWantsKeyboardFocus(true);
        if (!shortcutCustomization_.conflicts.empty()) {
            lastActionMessage_ = "快捷键设置：检测到冲突，已保留默认绑定，请在后续设置界面中修正。";
        }
        initialiseAudioOutput();
        playback_.poll(session_, loopState_);
        refreshFromSession();
        startTimerHz(30);
        setSize(1040, 920);
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
        menuBar_.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(10);
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
        targetTrackBox_.setBounds(targetRow.removeFromLeft(200));
        targetRow.removeFromLeft(10);
        createMidiClipButton_.setBounds(targetRow.removeFromLeft(136));
        targetRow.removeFromLeft(8);
        deleteInstrumentTrackButton_.setBounds(targetRow.removeFromLeft(112));
        targetRow.removeFromLeft(8);
        moveTrackUpButton_.setBounds(targetRow.removeFromLeft(64));
        targetRow.removeFromLeft(8);
        moveTrackDownButton_.setBounds(targetRow.removeFromLeft(64));

        bounds.removeFromTop(8);
        auto audioTargetRow = bounds.removeFromTop(36);
        targetAudioTrackLabel_.setBounds(audioTargetRow.removeFromLeft(96));
        audioTargetRow.removeFromLeft(8);
        targetAudioTrackBox_.setBounds(audioTargetRow.removeFromLeft(200));
        audioTargetRow.removeFromLeft(10);
        createAudioClipButton_.setBounds(audioTargetRow.removeFromLeft(136));
        audioTargetRow.removeFromLeft(8);
        addAudioTrackButton_.setBounds(audioTargetRow.removeFromLeft(104));
        audioTargetRow.removeFromLeft(8);
        deleteAudioTrackButton_.setBounds(audioTargetRow.removeFromLeft(112));
        audioTargetRow.removeFromLeft(8);
        addFolderTrackButton_.setBounds(audioTargetRow.removeFromLeft(104));

        bounds.removeFromTop(8);
        auto audioClipRow = bounds.removeFromTop(36);
        targetAudioClipLabel_.setBounds(audioClipRow.removeFromLeft(108));
        audioClipRow.removeFromLeft(10);
        targetAudioClipBox_.setBounds(audioClipRow.removeFromLeft(260));
        audioClipRow.removeFromLeft(12);
        audioClipNameEditor_.setBounds(audioClipRow.removeFromLeft(220));
        audioClipRow.removeFromLeft(12);
        renameAudioClipButton_.setBounds(audioClipRow.removeFromLeft(144));
        audioClipRow.removeFromLeft(8);
        deleteAudioClipButton_.setBounds(audioClipRow.removeFromLeft(144));

        bounds.removeFromTop(8);
        auto audioClipMoveRow = bounds.removeFromTop(36);
        audioClipMoveRow.removeFromLeft(108);
        audioClipMoveRow.removeFromLeft(10);
        duplicateAudioClipButton_.setBounds(audioClipMoveRow.removeFromLeft(144));
        audioClipMoveRow.removeFromLeft(8);
        moveAudioClipLeftButton_.setBounds(audioClipMoveRow.removeFromLeft(144));
        audioClipMoveRow.removeFromLeft(8);
        moveAudioClipRightButton_.setBounds(audioClipMoveRow.removeFromLeft(144));
        audioClipMoveRow.removeFromLeft(8);
        trimAudioClipEndButton_.setBounds(audioClipMoveRow.removeFromLeft(144));
        audioClipMoveRow.removeFromLeft(8);
        extendAudioClipEndButton_.setBounds(audioClipMoveRow.removeFromLeft(144));

        bounds.removeFromTop(8);
        auto audioClipStartRow = bounds.removeFromTop(36);
        audioClipStartRow.removeFromLeft(108);
        audioClipStartRow.removeFromLeft(10);
        trimAudioClipStartButton_.setBounds(audioClipStartRow.removeFromLeft(144));
        audioClipStartRow.removeFromLeft(8);
        extendAudioClipStartButton_.setBounds(audioClipStartRow.removeFromLeft(144));

        bounds.removeFromTop(8);
        auto audioClipSplitRow = bounds.removeFromTop(36);
        audioClipSplitRow.removeFromLeft(108);
        audioClipSplitRow.removeFromLeft(10);
        splitAudioClipButton_.setBounds(audioClipSplitRow.removeFromLeft(144));
        audioClipSplitRow.removeFromLeft(8);
        moveAudioClipToTrackButton_.setBounds(audioClipSplitRow.removeFromLeft(144));

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
        clipRow.removeFromLeft(12);
        duplicateMidiNoteButton_.setBounds(clipRow.removeFromLeft(144));

        bounds.removeFromTop(8);
        auto noteEditRow = bounds.removeFromTop(36);
        noteEditRow.removeFromLeft(108);
        noteEditRow.removeFromLeft(10);
        raiseMidiNotePitchButton_.setBounds(noteEditRow.removeFromLeft(96));
        noteEditRow.removeFromLeft(8);
        lowerMidiNotePitchButton_.setBounds(noteEditRow.removeFromLeft(96));
        noteEditRow.removeFromLeft(12);
        increaseMidiNoteVelocityButton_.setBounds(noteEditRow.removeFromLeft(96));
        noteEditRow.removeFromLeft(8);
        decreaseMidiNoteVelocityButton_.setBounds(noteEditRow.removeFromLeft(96));

        bounds.removeFromTop(8);
        auto noteTimingRow = bounds.removeFromTop(36);
        noteTimingRow.removeFromLeft(108);
        noteTimingRow.removeFromLeft(10);
        lengthenMidiNoteButton_.setBounds(noteTimingRow.removeFromLeft(96));
        noteTimingRow.removeFromLeft(8);
        shortenMidiNoteButton_.setBounds(noteTimingRow.removeFromLeft(96));
        noteTimingRow.removeFromLeft(12);
        moveMidiNoteEarlierButton_.setBounds(noteTimingRow.removeFromLeft(96));
        noteTimingRow.removeFromLeft(8);
        moveMidiNoteLaterButton_.setBounds(noteTimingRow.removeFromLeft(96));

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
        moveMidiClipLeftButton_.setBounds(clipMoveRow.removeFromLeft(96));
        clipMoveRow.removeFromLeft(8);
        moveMidiClipRightButton_.setBounds(clipMoveRow.removeFromLeft(96));
        clipMoveRow.removeFromLeft(8);
        moveMidiClipToTrackButton_.setBounds(clipMoveRow.removeFromLeft(104));
        clipMoveRow.removeFromLeft(8);
        trimMidiClipStartButton_.setBounds(clipMoveRow.removeFromLeft(96));
        clipMoveRow.removeFromLeft(8);
        extendMidiClipStartButton_.setBounds(clipMoveRow.removeFromLeft(96));
        clipMoveRow.removeFromLeft(8);
        trimMidiClipEndButton_.setBounds(clipMoveRow.removeFromLeft(96));
        clipMoveRow.removeFromLeft(8);
        extendMidiClipEndButton_.setBounds(clipMoveRow.removeFromLeft(96));

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

        const auto paletteWidth = std::max(320, std::min(720, getWidth() - 80));
        juce::Rectangle<int> paletteBounds(
            (getWidth() - paletteWidth) / 2,
            76,
            paletteWidth,
            292);
        commandPaletteBackground_.setBounds(paletteBounds);
        commandPalettePanel_.setBounds(paletteBounds);

        auto paletteInner = paletteBounds.reduced(18);
        auto paletteTitleRow = paletteInner.removeFromTop(24);
        commandPaletteRangeLabel_.setBounds(paletteTitleRow.removeFromRight(150));
        commandPaletteTitleLabel_.setBounds(paletteTitleRow);
        paletteInner.removeFromTop(8);
        commandPaletteQueryEditor_.setBounds(paletteInner.removeFromTop(34));
        paletteInner.removeFromTop(10);

        for (auto& rowLabel : commandPaletteRowLabels_) {
            rowLabel.setBounds(paletteInner.removeFromTop(28));
            paletteInner.removeFromTop(4);
        }

        commandPaletteEmptyLabel_.setBounds(commandPaletteRowLabels_.front().getBounds());
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (commandPaletteSession_.status().open) {
            if (key.getKeyCode() == juce::KeyPress::escapeKey) {
                closeCommandPaletteFromUi();
                return true;
            }

            if (key.getKeyCode() == juce::KeyPress::returnKey) {
                return activateCommandPaletteSelectionFromUi();
            }

            if (key.getKeyCode() == juce::KeyPress::downKey) {
                commandPaletteSession_.moveHighlightDown();
                refreshCommandPalettePanel();
                return true;
            }

            if (key.getKeyCode() == juce::KeyPress::upKey) {
                commandPaletteSession_.moveHighlightUp();
                refreshCommandPalettePanel();
                return true;
            }

            if (key.getKeyCode() == juce::KeyPress::homeKey) {
                commandPaletteSession_.moveHighlightToFirst();
                refreshCommandPalettePanel();
                return true;
            }

            if (key.getKeyCode() == juce::KeyPress::endKey) {
                commandPaletteSession_.moveHighlightToLast();
                refreshCommandPalettePanel();
                return true;
            }

            if (key.getKeyCode() == juce::KeyPress::pageDownKey) {
                commandPaletteSession_.moveHighlightPageDown(commandPaletteVisibleRowCount);
                refreshCommandPalettePanel();
                return true;
            }

            if (key.getKeyCode() == juce::KeyPress::pageUpKey) {
                commandPaletteSession_.moveHighlightPageUp(commandPaletteVisibleRowCount);
                refreshCommandPalettePanel();
                return true;
            }
        }

        const auto shortcutContext = commandPaletteSession_.status().open
            ? trackloom::AppShortcutContext::CommandPaletteOpen
            : trackloom::AppShortcutContext::MainWindow;
        if (const auto commandId = trackloom::appCommandIdForShortcut(
                appShortcutChordFromKeyPress(key),
                shortcutContext,
                shortcutCustomization_.bindings)) {
            return dispatchAppCommandFromUi(commandId.value());
        }

        // Space 仍是播放/停止“切换”语义，和菜单中的“播放”“停止”两个独立命令不同。
        if (key.getKeyCode() == juce::KeyPress::spaceKey) {
            toggleProjectPlayback();
            return true;
        }

        return false;
    }

    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        return keyPressed(key);
    }

    void mouseWheelMove(const juce::MouseEvent& event, const juce::MouseWheelDetails& wheel) override
    {
        if (commandPaletteSession_.status().open
            && commandPalettePanel_.getBounds().contains(event.getPosition())) {
            scrollCommandPaletteWithWheelFromUi(wheel);
            return;
        }

        juce::Component::mouseWheelMove(event, wheel);
    }

    juce::StringArray getMenuBarNames() override
    {
        juce::StringArray names;
        for (const auto& group : describeCurrentMainMenu().groups) {
            names.add(toJuceString(group.name));
        }

        return names;
    }

    juce::PopupMenu getMenuForIndex(int menuIndex, const juce::String&) override
    {
        juce::PopupMenu menu;
        const auto status = describeCurrentMainMenu();
        if (menuIndex < 0 || static_cast<std::size_t>(menuIndex) >= status.groups.size()) {
            return menu;
        }

        for (const auto& item : status.groups[static_cast<std::size_t>(menuIndex)].items) {
            if (item.separator) {
                menu.addSeparator();
            } else if (item.commandId == 0) {
                menu.addItem(toJuceString(item.label), false, false, [] {});
            } else {
                menu.addItem(item.commandId, toJuceString(item.label), item.enabled);
            }
        }

        return menu;
    }

    void menuItemSelected(int menuItemID, int) override
    {
        dispatchAppCommandFromUi(menuItemID);
    }

    trackloom::AppCommandDispatchResult dispatchCommand(int commandId)
    {
        const auto result = trackloom::dispatchAppCommand(commandId, makeAppCommandHandlers());
        if (!result.executed) {
            lastActionMessage_ = "未能执行命令：命令未注册或缺少处理函数。";
            refreshFromSession();
        }
        return result;
    }

    void serviceUiTimer()
    {
        timerCallback();
    }

private:
    static constexpr std::size_t commandPaletteVisibleRowCount = 6;

    trackloom::AppMainMenuSelection currentMainMenuSelection() const
    {
        return {
            selectedTrackId_,
            selectedAudioTrackId_,
            selectedMidiClipId_,
            selectedAudioClipId_
        };
    }

    trackloom::AppMainMenuStatus describeCurrentMainMenu() const
    {
        return trackloom::describeAppMainMenu(
            session_,
            playback_,
            recentProjects_,
            currentMainMenuSelection());
    }

    trackloom::AppShortcutStatus describeCurrentShortcutStatus() const
    {
        return trackloom::describeAppShortcutStatus(
            describeCurrentMainMenu(),
            shortcutCustomization_,
            customShortcutBindings_);
    }

    bool dispatchAppCommandFromUi(int commandId)
    {
        return dispatchCommand(commandId).executed;
    }

    trackloom::AppCommandHandlers makeAppCommandHandlers()
    {
        trackloom::AppCommandHandlers handlers;
        handlers.newProject = [this] { requestNewProject(); };
        handlers.openProject = [this] { chooseProjectToOpen(); };
        handlers.saveProject = [this] { saveCurrentProject(); };
        handlers.saveProjectAs = [this] { chooseProjectToSaveAs(); };
        handlers.undoProject = [this] { undoProjectEditFromMenu(); };
        handlers.redoProject = [this] { redoProjectEditFromMenu(); };
        handlers.addInstrumentTrack = [this] { addDefaultInstrumentTrack(); };
        handlers.addAudioTrack = [this] { addDefaultAudioTrack(); };
        handlers.addFolderTrack = [this] { addDefaultFolderTrack(); };
        handlers.renameSelectedInstrumentTrack = [this] { renameSelectedTrack(); };
        handlers.deleteSelectedInstrumentTrack = [this] { deleteSelectedInstrumentTrack(); };
        handlers.moveSelectedInstrumentTrackUp = [this] { moveSelectedTrackUp(); };
        handlers.moveSelectedInstrumentTrackDown = [this] { moveSelectedTrackDown(); };
        handlers.toggleSelectedInstrumentTrackMute = [this] { toggleSelectedTrackMute(); };
        handlers.toggleSelectedInstrumentTrackSolo = [this] { toggleSelectedTrackSolo(); };
        handlers.toggleSelectedInstrumentTrackDisabled = [this] { toggleSelectedTrackDisabled(); };
        handlers.toggleSelectedInstrumentTrackHidden = [this] { toggleSelectedTrackHidden(); };
        handlers.deleteSelectedAudioTrack = [this] { deleteSelectedAudioTrack(); };
        handlers.moveSelectedAudioTrackUp = [this] { moveSelectedAudioTrackUp(); };
        handlers.moveSelectedAudioTrackDown = [this] { moveSelectedAudioTrackDown(); };
        handlers.toggleSelectedAudioTrackMute = [this] { toggleSelectedAudioTrackMute(); };
        handlers.toggleSelectedAudioTrackSolo = [this] { toggleSelectedAudioTrackSolo(); };
        handlers.toggleSelectedAudioTrackDisabled = [this] { toggleSelectedAudioTrackDisabled(); };
        handlers.toggleSelectedAudioTrackHidden = [this] { toggleSelectedAudioTrackHidden(); };
        handlers.renameSelectedMidiClip = [this] { renameSelectedMidiClip(); };
        handlers.renameSelectedAudioClip = [this] { renameSelectedAudioClip(); };
        handlers.deleteSelectedMidiClip = [this] { deleteSelectedMidiClip(); };
        handlers.deleteSelectedAudioClip = [this] { deleteSelectedAudioClip(); };
        handlers.duplicateSelectedMidiClip = [this] { duplicateSelectedMidiClip(); };
        handlers.duplicateSelectedAudioClip = [this] { duplicateSelectedAudioClip(); };
        handlers.splitSelectedMidiClip = [this] { splitSelectedMidiClip(); };
        handlers.splitSelectedAudioClip = [this] { splitSelectedAudioClip(); };
        handlers.moveSelectedMidiClipToTargetTrack = [this] { moveSelectedMidiClipToSelectedTrack(); };
        handlers.moveSelectedAudioClipToTargetTrack = [this] { moveSelectedAudioClipToAudioTrack(); };
        handlers.moveSelectedMidiClipLeft = [this] { moveSelectedMidiClipLeft(); };
        handlers.moveSelectedMidiClipRight = [this] { moveSelectedMidiClipRight(); };
        handlers.trimSelectedMidiClipEnd = [this] { trimSelectedMidiClipEnd(); };
        handlers.extendSelectedMidiClipEnd = [this] { extendSelectedMidiClipEnd(); };
        handlers.trimSelectedMidiClipStart = [this] { trimSelectedMidiClipStart(); };
        handlers.extendSelectedMidiClipStart = [this] { extendSelectedMidiClipStart(); };
        handlers.moveSelectedAudioClipLeft = [this] { moveSelectedAudioClipLeft(); };
        handlers.moveSelectedAudioClipRight = [this] { moveSelectedAudioClipRight(); };
        handlers.trimSelectedAudioClipEnd = [this] { trimSelectedAudioClipEnd(); };
        handlers.extendSelectedAudioClipEnd = [this] { extendSelectedAudioClipEnd(); };
        handlers.trimSelectedAudioClipStart = [this] { trimSelectedAudioClipStart(); };
        handlers.extendSelectedAudioClipStart = [this] { extendSelectedAudioClipStart(); };
        handlers.playProject = [this] { startProjectPlayback(); };
        handlers.stopProject = [this] { stopProjectPlayback(); };
        handlers.rewindProject = [this] { rewindProjectPlayback(); };
        handlers.openCommandPalette = [this] { openCommandPaletteFromUi(); };
        handlers.openShortcutStatus = [this] { openShortcutStatusFromUi(); };
        handlers.openAudioSettings = [this] { openAudioSettingsFromUi(); };
        handlers.openRecentProject = [this](std::size_t number) {
            selectedRecentProjectNumber_ = number;
            openSelectedRecentProject();
        };
        return handlers;
    }

    void openAudioSettingsFromUi()
    {
        auto component = std::make_unique<trackloom::AudioSettingsComponent>(
            *audioHost_,
            currentAudioSettings_,
            trackloom::AudioSettingsComponentCallbacks {
                [this](const trackloom::AppAudioSettings& settings) {
                    currentAudioSettings_ = settings;
                    if (!audioSettingsPath_.empty()
                        && saveAudioSettings_(settings, audioSettingsPath_)) {
                        lastActionMessage_ = "音频设置：已应用并保存。";
                    } else {
                        lastActionMessage_ = "音频设置：设备已应用，但本地设置保存失败。";
                    }
                    refreshFromSession();
                },
                [this](std::string feedback) {
                    if (!feedback.empty()) {
                        lastActionMessage_ = "音频设置：" + std::move(feedback);
                        refreshFromSession();
                    }
                }
            });
        component->setSize(520, 230);

        if (presentAudioSettings_) {
            presentAudioSettings_(std::move(component));
            return;
        }

        audioSettingsWindow_ =
            std::make_unique<OwnedAudioSettingsWindow>(std::move(component));
    }

    void initialiseAudioOutput()
    {
        const auto loaded = loadAudioSettings_(audioSettingsPath_);
        currentAudioSettings_ = loaded.kind == trackloom::AppAudioSettingsLoadKind::Loaded
            ? loaded.settings
            : trackloom::AppAudioSettings {};

        auto opened = audioHost_->openOutput({
            currentAudioSettings_.outputDeviceName,
            currentAudioSettings_.requestedSampleRate,
            currentAudioSettings_.requestedBufferFrames,
            currentAudioSettings_.requestedOutputChannels
        });
        if (!opened.success && !currentAudioSettings_.outputDeviceName.empty()) {
            currentAudioSettings_ = {};
            opened = audioHost_->openOutput({
                currentAudioSettings_.outputDeviceName,
                currentAudioSettings_.requestedSampleRate,
                currentAudioSettings_.requestedBufferFrames,
                currentAudioSettings_.requestedOutputChannels
            });
        }

        if (!loaded.warning.empty()) {
            lastActionMessage_ = loaded.warning;
        } else if (!opened.success) {
            lastActionMessage_ = "音频输出：未找到可用的 Windows Audio 共享输出。";
        } else if (!opened.warning.empty()) {
            lastActionMessage_ = "音频输出：" + opened.warning;
        }
    }


    void openCommandPaletteFromUi()
    {
        const auto palette = trackloom::addAppCommandPaletteShortcutLabels(
            trackloom::describeAppCommandPalette(describeCurrentMainMenu()),
            shortcutCustomization_.bindings);

        // 菜单、快捷键和工具入口都在这里汇合，避免可见浮层自己复制命令列表规则。
        commandPaletteSession_.open(palette);
        const auto view = trackloom::describeAppCommandPaletteSession(commandPaletteSession_.status());
        syncingCommandPaletteQuery_ = true;
        commandPaletteQueryEditor_.setText({}, false);
        syncingCommandPaletteQuery_ = false;

        lastActionMessage_ = "命令面板：已准备 "
            + std::to_string(view.rows.size())
            + " 个命令入口。";
        refreshFromSession();
        commandPaletteQueryEditor_.grabKeyboardFocus();
        commandPaletteQueryEditor_.selectAll();
    }

    void openShortcutStatusFromUi()
    {
        const auto status = describeCurrentShortcutStatus();
        lastActionMessage_ = status.conflicts.empty()
            ? "快捷键状态：已打开当前快捷键列表。"
            : "快捷键状态：已打开当前快捷键列表，但存在 "
                + std::to_string(status.conflicts.size()) + " 个冲突。";

        juce::AlertWindow::showMessageBoxAsync(
            juce::AlertWindow::InfoIcon,
            toJuceString("快捷键状态"),
            toJuceString(shortcutStatusDialogText(status)),
            toJuceString("关闭"),
            this);
        refreshFromSession();
    }

    void updateCommandPaletteQueryFromUi()
    {
        if (syncingCommandPaletteQuery_) {
            return;
        }

        commandPaletteSession_.updateQuery(juceStringToUtf8(commandPaletteQueryEditor_.getText()));
        refreshCommandPalettePanel();
    }

    void scrollCommandPaletteWithWheelFromUi(const juce::MouseWheelDetails& wheel)
    {
        if (!commandPaletteSession_.status().open || wheel.deltaY == 0.0f) {
            return;
        }

        // 高精度触控板的滚动幅度暂不累计；当前只把一次 JUCE wheel 事件转换成一行窗口滚动。
        commandPaletteSession_.scrollVisibleRowsByWheelSteps(
            wheel.deltaY < 0.0f ? 1 : -1,
            commandPaletteVisibleRowCount);
        refreshCommandPalettePanel();
    }

    bool highlightCommandPaletteRowFromUi(std::size_t visibleIndex)
    {
        if (visibleIndex >= visibleCommandPaletteRowCommandIds_.size()) {
            return false;
        }

        const auto commandId = visibleCommandPaletteRowCommandIds_[visibleIndex];
        if (commandId <= 0 || !commandPaletteSession_.highlightCommandById(commandId)) {
            return false;
        }

        refreshCommandPalettePanel();
        return true;
    }

    bool activateCommandPaletteSelectionFromUi()
    {
        const auto result = trackloom::activateHighlightedAppCommandPaletteCommand(
            commandPaletteSession_.status(),
            makeAppCommandHandlers());

        return finishCommandPaletteActivationFromUi(result);
    }

    bool activateCommandPaletteRowFromUi(std::size_t visibleIndex)
    {
        if (visibleIndex >= visibleCommandPaletteRowCommandIds_.size()) {
            return false;
        }

        // 可见行每次刷新都会重新绑定 commandId；0 表示这一行当前没有命令。
        // 这里不直接用行号执行，避免过滤结果变化后点击到过期命令。
        const auto commandId = visibleCommandPaletteRowCommandIds_[visibleIndex];
        if (commandId <= 0) {
            return false;
        }

        const auto result = trackloom::activateAppCommandPaletteSessionRow(
            commandPaletteSession_.status(),
            commandId,
            makeAppCommandHandlers());

        return finishCommandPaletteActivationFromUi(result);
    }

    bool finishCommandPaletteActivationFromUi(
        const trackloom::AppCommandPaletteActivationResult& result)
    {
        if (result.executed) {
            if (result.dispatch.command != trackloom::AppCommandKind::OpenCommandPalette) {
                commandPaletteSession_.close();
            }
            refreshFromSession();
            grabKeyboardFocus();
            return true;
        }

        switch (result.kind) {
        case trackloom::AppCommandPaletteActivationResultKind::NoMatchingCommand:
            lastActionMessage_ = "命令面板：没有匹配的命令。";
            break;
        case trackloom::AppCommandPaletteActivationResultKind::OnlyDisabledMatches:
            lastActionMessage_ = "命令面板：匹配命令当前不可用。";
            break;
        case trackloom::AppCommandPaletteActivationResultKind::DispatchFailed:
            lastActionMessage_ = "命令面板：命令缺少处理函数。";
            break;
        case trackloom::AppCommandPaletteActivationResultKind::Executed:
            break;
        }

        refreshFromSession();
        commandPaletteQueryEditor_.grabKeyboardFocus();
        return true;
    }

    void closeCommandPaletteFromUi()
    {
        commandPaletteSession_.close();
        lastActionMessage_ = "命令面板：已关闭。";
        refreshFromSession();
        grabKeyboardFocus();
    }

    void refreshCommandPalettePanel()
    {
        const auto view = trackloom::describeAppCommandPaletteSession(commandPaletteSession_.status());
        const auto visible = view.open;

        commandPaletteBackground_.setVisible(visible);
        commandPalettePanel_.setVisible(visible);
        commandPaletteTitleLabel_.setVisible(visible);
        commandPaletteRangeLabel_.setVisible(visible);
        commandPaletteQueryEditor_.setVisible(visible);
        commandPaletteEmptyLabel_.setVisible(visible && view.rows.empty());

        if (!visible) {
            for (std::size_t visibleIndex = 0; visibleIndex < commandPaletteRowLabels_.size(); ++visibleIndex) {
                visibleCommandPaletteRowCommandIds_[visibleIndex] = 0;
                auto& rowLabel = commandPaletteRowLabels_[visibleIndex];
                rowLabel.setVisible(false);
            }
            return;
        }

        syncingCommandPaletteQuery_ = true;
        commandPaletteQueryEditor_.setText(toJuceString(view.query), false);
        syncingCommandPaletteQuery_ = false;

        const auto visibleRows = trackloom::describeVisibleAppCommandPaletteSessionRows(
            view,
            commandPaletteVisibleRowCount);
        commandPaletteRangeLabel_.setText(
            toJuceString(trackloom::describeAppCommandPaletteVisibleRowsRange(visibleRows)),
            juce::dontSendNotification);
        for (std::size_t visibleIndex = 0; visibleIndex < commandPaletteRowLabels_.size(); ++visibleIndex) {
            auto& rowLabel = commandPaletteRowLabels_[visibleIndex];

            if (visibleIndex >= visibleRows.rows.size()) {
                visibleCommandPaletteRowCommandIds_[visibleIndex] = 0;
                rowLabel.setVisible(false);
                continue;
            }

            const auto& row = visibleRows.rows[visibleIndex];
            visibleCommandPaletteRowCommandIds_[visibleIndex] = row.commandId;
            rowLabel.setVisible(true);
            rowLabel.setText(
                toJuceString(trackloom::describeAppCommandPaletteSessionRow(row)),
                juce::dontSendNotification);
            rowLabel.setColour(
                juce::Label::backgroundColourId,
                row.highlighted ? juce::Colour(0xff284634) : juce::Colour(0xff20231f));
            rowLabel.setColour(
                juce::Label::textColourId,
                row.enabled ? juce::Colour(0xffdfe9d8) : juce::Colour(0xff84917f));
        }

        commandPaletteEmptyLabel_.setText(toJuceString(view.emptyMessage), juce::dontSendNotification);
        commandPaletteBackground_.toFront(false);
        commandPalettePanel_.toFront(false);
        commandPaletteTitleLabel_.toFront(false);
        commandPaletteRangeLabel_.toFront(false);
        commandPaletteQueryEditor_.toFront(false);
        for (auto& rowLabel : commandPaletteRowLabels_) {
            rowLabel.toFront(false);
        }
        commandPaletteEmptyLabel_.toFront(false);
    }

    void undoProjectEditFromMenu()
    {
        // 撤销只处理 Project 命令历史；播放头、最近工程和文件选择器状态不进入工程撤销栈。
        if (session_.undoProjectEdit()) {
            lastActionMessage_ = "编辑动作：已撤销上一步工程编辑。";
        } else {
            lastActionMessage_ = "编辑动作：当前没有可撤销的工程编辑。";
        }
        refreshFromSession();
    }

    void redoProjectEditFromMenu()
    {
        // 重做沿用同一会话历史；失败时只更新提示，不制造新的工程修改。
        if (session_.redoProjectEdit()) {
            lastActionMessage_ = "编辑动作：已重做上一步工程编辑。";
        } else {
            lastActionMessage_ = "编辑动作：当前没有可重做的工程编辑。";
        }
        refreshFromSession();
    }

    void timerCallback() override
    {
        playback_.poll(session_, loopState_);
        refreshPlaybackPresentation(audioHost_->snapshot());
    }

    void requestNewProject()
    {
        const auto result = trackloom::createNewAppProjectIfSafe(
            session_, playback_, loopState_, projectObjectSelection(), "Untitled");
        lastActionMessage_ = result.message;
        refreshFromSession();
    }

    void chooseProjectToOpen()
    {
        if (session_.isDirty()) {
            lastActionMessage_ = "当前工程有未保存修改，请先保存或另存为，再打开其他工程。";
            refreshFromSession();
            return;
        }

        const auto safety = trackloom::prepareAppProjectReplacement(playback_);
        if (!safety.safe) {
            lastActionMessage_ = safety.message;
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

        const juce::Component::SafePointer<TrackLoomMainComponentImpl> safeThis(this);
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

        const juce::Component::SafePointer<TrackLoomMainComponentImpl> safeThis(this);
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

        const auto openResult = trackloom::openAppProjectIfSafe(
            session_,
            playback_,
            loopState_,
            projectObjectSelection(),
            juceFileToPath(selectedFile));
        if (openResult.success) {
            recordCurrentProjectAsRecent();
        }
        lastActionMessage_ = openResult.message;
        refreshFromSession();
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
        const auto presentation = trackloom::describeAppProjectFileActionPresentation(feedback);
        lastActionMessage_ = presentation.summary;
        actionLabel_.setTooltip(toJuceString(presentation.details));
        if (feedback.success) {
            recordCurrentProjectAsRecent();
        }
        refreshFromSession();

        if (presentation.showWarningDetails) {
            // 弹窗是完整详情的主入口；TooltipWindow 让状态栏悬停时也能再次查看同一内容。
            juce::AlertWindow::showMessageBoxAsync(
                juce::AlertWindow::WarningIcon,
                toJuceString("工程保存警告"),
                toJuceString(presentation.details),
                toJuceString("我知道了"),
                this);
        }
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

    void updateSelectedAudioTrackFromComboBox()
    {
        const auto selectedId = targetAudioTrackBox_.getSelectedId();
        if (selectedId <= 0
            || static_cast<std::size_t>(selectedId) > selectableAudioTrackIds_.size()) {
            selectedAudioTrackId_.clear();
            return;
        }

        selectedAudioTrackId_ = selectableAudioTrackIds_[static_cast<std::size_t>(selectedId - 1)];
    }

    void updateSelectedAudioClipFromComboBox()
    {
        const auto selectedId = targetAudioClipBox_.getSelectedId();
        if (selectedId <= 0
            || static_cast<std::size_t>(selectedId) > selectableAudioClipIds_.size()) {
            selectedAudioClipId_.clear();
            syncAudioClipNameEditorFromSelection(true);
            return;
        }

        selectedAudioClipId_ = selectableAudioClipIds_[static_cast<std::size_t>(selectedId - 1)];
        syncAudioClipNameEditorFromSelection(true);
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
        const auto feedback = trackloom::startAppPlayback(playback_, session_, loopState_);
        lastActionMessage_ = feedback.message;
        startTimerHz(30);
        refreshFromSession();
    }

    void stopProjectPlayback()
    {
        const auto feedback = trackloom::stopAppPlayback(playback_);
        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void toggleProjectPlayback()
    {
        const auto feedback = trackloom::toggleAppPlayback(playback_, session_, loopState_);
        lastActionMessage_ = feedback.message;
        startTimerHz(30);
        refreshFromSession();
    }

    void rewindProjectPlayback()
    {
        const auto feedback = trackloom::rewindAppPlaybackToStart(playback_);
        lastActionMessage_ = feedback.message;
        startTimerHz(30);
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

    void addDefaultAudioTrack()
    {
        const auto feedback = trackloom::createDefaultAudioTrack(session_);
        if (feedback.success) {
            selectedAudioTrackId_ = feedback.trackId;
        }

        // 音频轨有独立目标选择器；不能复用乐器轨选择，避免把 MIDI 与音频片段创建规则混在一起。
        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void addDefaultFolderTrack()
    {
        const auto feedback = trackloom::createDefaultFolderTrack(session_);

        // 文件夹轨当前只在只读轨道列表中展示；层级归组和折叠编辑后续单独接入。
        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void deleteSelectedAudioTrack()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再删除音频轨。";
            refreshFromSession();
            return;
        }

        const auto targetTrackId = selectedAudioTrackId_;
        const auto feedback = trackloom::deleteAudioTrackById(session_, targetTrackId);
        if (feedback.success && selectedAudioTrackId_ == targetTrackId) {
            selectedAudioTrackId_.clear();
            selectedAudioClipId_.clear();
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedAudioTrackUp()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再上移音频轨。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveAudioTrackUp(session_, selectedAudioTrackId_);
        if (feedback.success) {
            selectedAudioTrackId_ = feedback.trackId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedAudioTrackDown()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再下移音频轨。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveAudioTrackDown(session_, selectedAudioTrackId_);
        if (feedback.success) {
            selectedAudioTrackId_ = feedback.trackId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void setAudioTrackStateFeedback(const trackloom::AppTrackStateActionFeedback& feedback)
    {
        lastActionMessage_ = feedback.message;
        if (feedback.success) {
            selectedAudioTrackId_ = feedback.trackId;
        }
        refreshFromSession();
    }

    void toggleSelectedAudioTrackMute()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再切换静音状态。";
            refreshFromSession();
            return;
        }

        setAudioTrackStateFeedback(trackloom::toggleTrackMuted(session_, selectedAudioTrackId_));
    }

    void toggleSelectedAudioTrackSolo()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再切换独奏状态。";
            refreshFromSession();
            return;
        }

        setAudioTrackStateFeedback(trackloom::toggleTrackSoloed(session_, selectedAudioTrackId_));
    }

    void toggleSelectedAudioTrackDisabled()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再切换禁用状态。";
            refreshFromSession();
            return;
        }

        setAudioTrackStateFeedback(trackloom::toggleTrackDisabled(session_, selectedAudioTrackId_));
    }

    void toggleSelectedAudioTrackHidden()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条音频轨，再切换隐藏状态。";
            refreshFromSession();
            return;
        }

        setAudioTrackStateFeedback(trackloom::toggleTrackHidden(session_, selectedAudioTrackId_));
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

    void createAudioClipOnSelectedAudioTrack()
    {
        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先添加并选择一条音频轨，再创建音频片段。";
            refreshFromSession();
            return;
        }

        const auto targetTrackId = selectedAudioTrackId_;
        const auto feedback = trackloom::createDefaultAudioClipOnTrack(session_, targetTrackId);
        if (feedback.success) {
            selectedAudioTrackId_ = targetTrackId;
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void deleteSelectedAudioClip()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再删除音频片段。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedAudioClipId_;
        const auto feedback = trackloom::deleteAudioClipById(session_, targetClipId);
        if (feedback.success && selectedAudioClipId_ == targetClipId) {
            selectedAudioClipId_.clear();
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void duplicateSelectedAudioClip()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再复制音频片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::duplicateAudioClipAfterItself(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void splitSelectedAudioClip()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再拆分音频片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::splitAudioClipAtMidpoint(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedAudioClipToAudioTrack()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再移动到目标音频轨。";
            refreshFromSession();
            return;
        }

        if (selectedAudioTrackId_.empty()) {
            lastActionMessage_ = "请先选择目标音频轨，再移动音频片段。";
            refreshFromSession();
            return;
        }

        const auto targetTrackId = selectedAudioTrackId_;
        const auto feedback = trackloom::moveAudioClipToTrack(
            session_,
            selectedAudioClipId_,
            targetTrackId);
        if (feedback.success) {
            selectedAudioTrackId_ = targetTrackId;
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void renameSelectedAudioClip()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再重命名音频片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::renameAudioClipById(
            session_,
            selectedAudioClipId_,
            juceStringToUtf8(audioClipNameEditor_.getText()));
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedAudioClipLeft()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再左移音频片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveAudioClipLeftOneBeat(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveSelectedAudioClipRight()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再右移音频片段。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::moveAudioClipRightOneBeat(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void trimSelectedAudioClipEnd()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再缩短音频片尾。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::trimAudioClipEndEarlierOneBeat(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void extendSelectedAudioClipEnd()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再延长音频片尾。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::extendAudioClipEndLaterOneBeat(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void trimSelectedAudioClipStart()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再缩短音频片头。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::trimAudioClipStartLaterOneBeat(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void extendSelectedAudioClipStart()
    {
        if (selectedAudioClipId_.empty()) {
            lastActionMessage_ = "请先选择一个音频片段，再延长音频片头。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::extendAudioClipStartEarlierOneBeat(session_, selectedAudioClipId_);
        if (feedback.success) {
            selectedAudioClipId_ = feedback.clipId;
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

    void duplicateMidiNoteInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再复制末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::duplicateLastMidiNoteInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void raiseMidiNotePitchInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再升高末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::raiseLastMidiNotePitchInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void lowerMidiNotePitchInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再降低末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::lowerLastMidiNotePitchInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void increaseMidiNoteVelocityInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再增强末尾音符力度。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::increaseLastMidiNoteVelocityInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void decreaseMidiNoteVelocityInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再减弱末尾音符力度。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::decreaseLastMidiNoteVelocityInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void lengthenMidiNoteInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再延长末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::lengthenLastMidiNoteInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void shortenMidiNoteInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再缩短末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::shortenLastMidiNoteInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveMidiNoteEarlierInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再左移末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::moveLastMidiNoteStartEarlierInClip(session_, targetClipId);
        if (feedback.success) {
            selectedMidiClipId_ = targetClipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void moveMidiNoteLaterInSelectedClip()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再右移末尾音符。";
            refreshFromSession();
            return;
        }

        const auto targetClipId = selectedMidiClipId_;
        const auto feedback = trackloom::moveLastMidiNoteStartLaterInClip(session_, targetClipId);
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

    void moveSelectedMidiClipToSelectedTrack()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再移动到目标轨。";
            refreshFromSession();
            return;
        }

        if (selectedTrackId_.empty()) {
            lastActionMessage_ = "请先选择一条目标乐器轨，再移动 MIDI 片段。";
            refreshFromSession();
            return;
        }

        const auto targetTrackId = selectedTrackId_;
        const auto feedback = trackloom::moveMidiClipToTrack(session_, selectedMidiClipId_, targetTrackId);
        if (feedback.success) {
            selectedTrackId_ = targetTrackId;
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void trimSelectedMidiClipEnd()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再缩短片尾。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::trimMidiClipEndEarlierOneBeat(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void trimSelectedMidiClipStart()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再缩短片头。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::trimMidiClipStartLaterOneBeat(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void extendSelectedMidiClipStart()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再延长片头。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::extendMidiClipStartEarlierOneBeat(session_, selectedMidiClipId_);
        if (feedback.success) {
            selectedMidiClipId_ = feedback.clipId;
        }

        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    void extendSelectedMidiClipEnd()
    {
        if (selectedMidiClipId_.empty()) {
            lastActionMessage_ = "请先选择一个 MIDI 片段，再延长片尾。";
            refreshFromSession();
            return;
        }

        const auto feedback = trackloom::extendMidiClipEndLaterOneBeat(session_, selectedMidiClipId_);
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
            playback_,
            loopState_,
            projectObjectSelection(),
            recentProjects_,
            selectedRecentProjectNumber_,
            recentProjectsSettingsPath_);
        lastActionMessage_ = feedback.message;
        refreshFromSession();
    }

    trackloom::AppProjectObjectSelection projectObjectSelection() noexcept
    {
        return {
            selectedTrackId_,
            selectedAudioTrackId_,
            selectedAudioClipId_,
            selectedMidiClipId_
        };
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

    void refreshAudioTrackTargetSelector()
    {
        const auto previousSelection = selectedAudioTrackId_;
        selectableAudioTrackIds_.clear();
        targetAudioTrackBox_.clear(juce::dontSendNotification);

        int itemId = 1;
        int selectedItemId = 0;
        for (const auto& track : session_.project().tracks()) {
            if (track.type != trackloom::TrackType::Audio) {
                continue;
            }

            selectableAudioTrackIds_.push_back(track.id);
            targetAudioTrackBox_.addItem(toJuceString(track.name), itemId);

            if (track.id == previousSelection) {
                selectedItemId = itemId;
            }

            ++itemId;
        }

        if (selectedItemId == 0 && !selectableAudioTrackIds_.empty()) {
            selectedItemId = 1;
            selectedAudioTrackId_ = selectableAudioTrackIds_.front();
        } else if (selectedItemId > 0) {
            selectedAudioTrackId_ = previousSelection;
        } else {
            selectedAudioTrackId_.clear();
        }

        targetAudioTrackBox_.setSelectedId(selectedItemId, juce::dontSendNotification);
        targetAudioTrackBox_.setEnabled(!selectableAudioTrackIds_.empty());
        createAudioClipButton_.setEnabled(!selectedAudioTrackId_.empty());
        deleteAudioTrackButton_.setEnabled(!selectedAudioTrackId_.empty());
    }

    void refreshAudioClipTargetSelector(const trackloom::AppTimelineStatus& timelineStatus)
    {
        const auto previousSelection = selectedAudioClipId_;
        selectableAudioClipIds_.clear();
        targetAudioClipBox_.clear(juce::dontSendNotification);

        int itemId = 1;
        int selectedItemId = 0;
        for (const auto& row : timelineStatus.rows) {
            if (row.type != trackloom::ClipType::Audio) {
                continue;
            }

            selectableAudioClipIds_.push_back(row.clipId);
            targetAudioClipBox_.addItem(
                toJuceString(row.name + " - " + row.trackName),
                itemId);

            if (row.clipId == previousSelection) {
                selectedItemId = itemId;
            }

            ++itemId;
        }

        if (selectedItemId == 0 && !selectableAudioClipIds_.empty()) {
            selectedItemId = 1;
            selectedAudioClipId_ = selectableAudioClipIds_.front();
        } else if (selectedItemId > 0) {
            selectedAudioClipId_ = previousSelection;
        } else {
            selectedAudioClipId_.clear();
        }

        targetAudioClipBox_.setSelectedId(selectedItemId, juce::dontSendNotification);
        targetAudioClipBox_.setEnabled(!selectableAudioClipIds_.empty());
        deleteAudioClipButton_.setEnabled(!selectedAudioClipId_.empty());
        renameAudioClipButton_.setEnabled(!selectedAudioClipId_.empty());
        duplicateAudioClipButton_.setEnabled(!selectedAudioClipId_.empty());
        splitAudioClipButton_.setEnabled(!selectedAudioClipId_.empty());
        moveAudioClipToTrackButton_.setEnabled(!selectedAudioClipId_.empty() && !selectedAudioTrackId_.empty());
        moveAudioClipLeftButton_.setEnabled(!selectedAudioClipId_.empty());
        moveAudioClipRightButton_.setEnabled(!selectedAudioClipId_.empty());
        trimAudioClipEndButton_.setEnabled(!selectedAudioClipId_.empty());
        extendAudioClipEndButton_.setEnabled(!selectedAudioClipId_.empty());
        trimAudioClipStartButton_.setEnabled(!selectedAudioClipId_.empty());
        extendAudioClipStartButton_.setEnabled(!selectedAudioClipId_.empty());
        syncAudioClipNameEditorFromSelection(selectedAudioClipId_ != previousSelection);
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
        duplicateMidiNoteButton_.setEnabled(!selectedMidiClipId_.empty());
        raiseMidiNotePitchButton_.setEnabled(!selectedMidiClipId_.empty());
        lowerMidiNotePitchButton_.setEnabled(!selectedMidiClipId_.empty());
        increaseMidiNoteVelocityButton_.setEnabled(!selectedMidiClipId_.empty());
        decreaseMidiNoteVelocityButton_.setEnabled(!selectedMidiClipId_.empty());
        lengthenMidiNoteButton_.setEnabled(!selectedMidiClipId_.empty());
        shortenMidiNoteButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiNoteEarlierButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiNoteLaterButton_.setEnabled(!selectedMidiClipId_.empty());
        duplicateMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
        splitMidiClipButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiClipLeftButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiClipRightButton_.setEnabled(!selectedMidiClipId_.empty());
        moveMidiClipToTrackButton_.setEnabled(!selectedMidiClipId_.empty() && !selectedTrackId_.empty());
        trimMidiClipStartButton_.setEnabled(!selectedMidiClipId_.empty());
        extendMidiClipStartButton_.setEnabled(!selectedMidiClipId_.empty());
        trimMidiClipEndButton_.setEnabled(!selectedMidiClipId_.empty());
        extendMidiClipEndButton_.setEnabled(!selectedMidiClipId_.empty());
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

    void syncAudioClipNameEditorFromSelection(bool forceUpdate)
    {
        audioClipNameEditor_.setEnabled(!selectedAudioClipId_.empty());

        const auto selectedClip = session_.project().findClipById(selectedAudioClipId_);
        if (!selectedClip.has_value()) {
            audioClipNameEditor_.setText(juce::String{}, false);
            return;
        }

        // 播放 Timer 和列表刷新不应覆盖用户正在输入但尚未提交的音频片段名称。
        if (forceUpdate || !audioClipNameEditor_.hasKeyboardFocus(true)) {
            audioClipNameEditor_.setText(toJuceString(selectedClip->name), false);
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

    void refreshPlaybackPresentation(
        const trackloom::RealtimePlaybackHostSnapshot& hostStatus)
    {
        const auto playbackStatus = trackloom::describeAppPlayback(playback_);
        const auto playbackText = playbackStatus.state == trackloom::AppPlaybackState::Preparing
            ? std::string("正在准备音频…")
            : playbackStatus.summary;
        playbackStatusLabel_.setText(
            toJuceString(playbackText + "  " + diagnosticText(hostStatus)),
            juce::dontSendNotification);
        playProjectButton_.setEnabled(playbackStatus.canStart);
        stopProjectButton_.setEnabled(
            playbackStatus.state == trackloom::AppPlaybackState::Preparing
            || playbackStatus.state == trackloom::AppPlaybackState::Playing);
        rewindProjectButton_.setEnabled(playback_.currentSample() > 0);
    }

    void refreshFromSession()
    {
        const auto status = trackloom::describeAppProjectSession(session_);
        const auto hostStatus = audioHost_->snapshot();
        const auto timelineStatus = trackloom::describeAppTimeline(session_.project());
        const auto recentStatus = trackloom::describeAppRecentProjects(recentProjects_);
        refreshTrackTargetSelector();
        refreshAudioTrackTargetSelector();
        refreshAudioClipTargetSelector(timelineStatus);
        refreshMidiClipTargetSelector(timelineStatus);
        refreshRecentProjectSelector(recentStatus);

        titleLabel_.setText(toJuceString(status.windowTitle), juce::dontSendNotification);
        statusLabel_.setText(toJuceString(status.statusLine), juce::dontSendNotification);
        refreshPlaybackPresentation(hostStatus);
        trackSummaryLabel_.setText(toJuceString(trackSummaryText(status)), juce::dontSendNotification);
        actionLabel_.setText(toJuceString(lastActionMessage_), juce::dontSendNotification);
        trackListText_.setText(
            toJuceString(trackListText(trackloom::describeAppTrackList(session_.project()))),
            false);
        timelineText_.setText(
            toJuceString(timelineText(timelineStatus)),
            false);
        recentProjectsText_.setText(
            toJuceString(recentProjectsText(recentStatus)),
            false);
        refreshCommandPalettePanel();

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

    static std::string diagnosticText(
        const trackloom::RealtimePlaybackHostSnapshot& snapshot)
    {
        const auto& format = snapshot.format;
        const auto& realtime = snapshot.realtime;
        const auto deviceName = format.available && !format.deviceName.empty()
            ? format.deviceName
            : std::string("无设备");
        const auto xrun = snapshot.xRunCount < 0
            ? std::string("后端未提供")
            : std::to_string(snapshot.xRunCount);
        return "设备 " + deviceName
            + "，" + std::to_string(static_cast<std::int64_t>(format.sampleRate)) + " Hz"
            + "，buffer " + std::to_string(format.maximumBlockFrames)
            + "，声道 " + std::to_string(format.outputChannelCount)
            + "，callback " + std::to_string(realtime.callbackCount)
            + "，timeout " + std::to_string(realtime.callbackTimeoutCount)
            + "，oversized " + std::to_string(realtime.oversizedBlockCount)
            + "，xrun " + xrun
            + "，voice stealing " + std::to_string(realtime.voiceStealCount)
            + "，stale Note Off " + std::to_string(realtime.staleNoteOffCount);
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

    std::unique_ptr<trackloom::JuceAudioHost> audioHost_;
    trackloom::AppPlaybackController playback_;
    std::filesystem::path audioSettingsPath_;
    trackloom::AppAudioSettingsLoadOperation loadAudioSettings_;
    trackloom::AppAudioSettingsSaveOperation saveAudioSettings_;
    std::function<void(std::unique_ptr<trackloom::AudioSettingsComponent>)>
        presentAudioSettings_;
    trackloom::AppAudioSettings currentAudioSettings_;
    trackloom::AppProjectSession session_;
    trackloom::AppLoopPlaybackState loopState_;
    std::function<void(std::string)> titleChanged_;
    std::unique_ptr<juce::FileChooser> fileChooser_;
    std::filesystem::path recentProjectsSettingsPath_;
    trackloom::AppRecentProjects recentProjects_;
    std::filesystem::path shortcutSettingsPath_;
    std::vector<trackloom::AppShortcutBinding> customShortcutBindings_;
    trackloom::AppShortcutCustomizationResult shortcutCustomization_;
    trackloom::AppCommandPaletteSession commandPaletteSession_;
    // 这个数组保存“当前屏幕上第 N 行对应哪个命令”。
    // 真正执行前仍会让 AppCommandPaletteSession 重新确认该命令仍在当前过滤结果里。
    std::array<int, commandPaletteVisibleRowCount> visibleCommandPaletteRowCommandIds_{};
    bool syncingCommandPaletteQuery_ = false;
    std::vector<std::string> selectableTrackIds_;
    std::vector<std::string> selectableAudioTrackIds_;
    std::vector<std::string> selectableAudioClipIds_;
    std::vector<std::string> selectableMidiClipIds_;
    std::vector<std::size_t> selectableRecentProjectNumbers_;
    std::string lastActionMessage_ = "文件动作：尚未打开或保存工程。";
    std::string selectedTrackId_;
    std::string selectedAudioTrackId_;
    std::string selectedAudioClipId_;
    std::string selectedMidiClipId_;
    std::size_t selectedRecentProjectNumber_ = 0;
    juce::TooltipWindow tooltipWindow_;
    juce::MenuBarComponent menuBar_;
    juce::Label commandPaletteBackground_;
    juce::GroupComponent commandPalettePanel_;
    juce::Label commandPaletteTitleLabel_;
    juce::Label commandPaletteRangeLabel_;
    juce::TextEditor commandPaletteQueryEditor_;
    std::array<CommandPaletteRowLabel, commandPaletteVisibleRowCount> commandPaletteRowLabels_;
    juce::Label commandPaletteEmptyLabel_;
    juce::Label titleLabel_;
    juce::Label statusLabel_;
    juce::Label playbackStatusLabel_;
    juce::Label trackSummaryLabel_;
    juce::Label actionLabel_;
    juce::Label targetTrackLabel_;
    juce::ComboBox targetTrackBox_;
    juce::Label targetAudioTrackLabel_;
    juce::ComboBox targetAudioTrackBox_;
    juce::Label targetAudioClipLabel_;
    juce::ComboBox targetAudioClipBox_;
    juce::TextEditor audioClipNameEditor_;
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
    juce::TextButton addAudioTrackButton_;
    juce::TextButton addFolderTrackButton_;
    juce::TextButton playProjectButton_;
    juce::TextButton stopProjectButton_;
    juce::TextButton rewindProjectButton_;
    juce::TextButton createMidiClipButton_;
    juce::TextButton createAudioClipButton_;
    juce::TextButton deleteAudioTrackButton_;
    juce::TextButton deleteAudioClipButton_;
    juce::TextButton renameAudioClipButton_;
    juce::TextButton duplicateAudioClipButton_;
    juce::TextButton splitAudioClipButton_;
    juce::TextButton moveAudioClipToTrackButton_;
    juce::TextButton moveAudioClipLeftButton_;
    juce::TextButton moveAudioClipRightButton_;
    juce::TextButton trimAudioClipEndButton_;
    juce::TextButton extendAudioClipEndButton_;
    juce::TextButton trimAudioClipStartButton_;
    juce::TextButton extendAudioClipStartButton_;
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
    juce::TextButton duplicateMidiNoteButton_;
    juce::TextButton raiseMidiNotePitchButton_;
    juce::TextButton lowerMidiNotePitchButton_;
    juce::TextButton increaseMidiNoteVelocityButton_;
    juce::TextButton decreaseMidiNoteVelocityButton_;
    juce::TextButton lengthenMidiNoteButton_;
    juce::TextButton shortenMidiNoteButton_;
    juce::TextButton moveMidiNoteEarlierButton_;
    juce::TextButton moveMidiNoteLaterButton_;
    juce::TextButton duplicateMidiClipButton_;
    juce::TextButton splitMidiClipButton_;
    juce::TextButton moveMidiClipLeftButton_;
    juce::TextButton moveMidiClipRightButton_;
    juce::TextButton moveMidiClipToTrackButton_;
    juce::TextButton trimMidiClipStartButton_;
    juce::TextButton extendMidiClipStartButton_;
    juce::TextButton trimMidiClipEndButton_;
    juce::TextButton extendMidiClipEndButton_;
    juce::TextButton deleteMidiClipButton_;
    juce::TextButton renameMidiClipButton_;
    std::unique_ptr<OwnedAudioSettingsWindow> audioSettingsWindow_;
};

}

namespace trackloom {

std::filesystem::path trackLoomAudioSettingsPath(
    const std::filesystem::path& userApplicationDataDirectory)
{
    return userApplicationDataDirectory / "TrackLoom" / "audio-settings.txt";
}

namespace {

TrackLoomMainComponentDependencies normaliseDependencies(
    TrackLoomMainComponentDependencies dependencies)
{
    if (!dependencies.audioHost) {
        throw std::invalid_argument("TrackLoomMainComponent requires audioHost");
    }

    if (!dependencies.buildOperation) {
        dependencies.buildOperation = buildPreparedMidiPlaybackPlan;
    }
    if (!dependencies.loadAudioSettings) {
        dependencies.loadAudioSettings = loadAppAudioSettings;
    }
    if (!dependencies.saveAudioSettings) {
        dependencies.saveAudioSettings = saveAppAudioSettings;
    }
    return dependencies;
}

}

struct TrackLoomMainComponent::Impl final : public TrackLoomMainComponentImpl {
    explicit Impl(TrackLoomMainComponentDependencies dependencies)
        : TrackLoomMainComponentImpl(std::move(dependencies))
    {
    }
};

TrackLoomMainComponent::TrackLoomMainComponent(
    TrackLoomMainComponentDependencies dependencies)
    : impl_(std::make_unique<Impl>(normaliseDependencies(std::move(dependencies))))
{
    addAndMakeVisible(*impl_);
    setSize(impl_->getWidth(), impl_->getHeight());
}

TrackLoomMainComponent::~TrackLoomMainComponent() = default;

AppCommandDispatchResult TrackLoomMainComponent::dispatchCommand(int commandId)
{
    return impl_->dispatchCommand(commandId);
}

void TrackLoomMainComponent::serviceUiTimer()
{
    impl_->serviceUiTimer();
}

juce::StringArray TrackLoomMainComponent::getMenuBarNames()
{
    return impl_->getMenuBarNames();
}

juce::PopupMenu TrackLoomMainComponent::getMenuForIndex(
    int index,
    const juce::String& name)
{
    return impl_->getMenuForIndex(index, name);
}

void TrackLoomMainComponent::menuItemSelected(int commandId, int topLevelMenuIndex)
{
    impl_->menuItemSelected(commandId, topLevelMenuIndex);
}

bool TrackLoomMainComponent::keyPressed(const juce::KeyPress& key)
{
    return impl_->keyPressed(key);
}

bool TrackLoomMainComponent::keyPressed(
    const juce::KeyPress& key,
    juce::Component* origin)
{
    return impl_->keyPressed(key, origin);
}

void TrackLoomMainComponent::resized()
{
    impl_->setBounds(getLocalBounds());
}

void TrackLoomMainComponent::timerCallback()
{
    serviceUiTimer();
}

}
