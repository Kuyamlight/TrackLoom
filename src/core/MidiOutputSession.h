#pragma once

#include "ProjectMidiOutputGraph.h"

#include <cstddef>
#include <string>
#include <vector>

namespace trackloom {

// MidiOutputSession 是一次播放输出会话的轻量状态层。
// 它不打开设备、不拥有插件，只记录已经成功送达的活动 MIDI 音符。
class MidiOutputSession final {
public:
    // rebuild 只在没有活动音符时允许换路由，避免 Note On 和 Note Off 被发到不同目标。
    bool rebuild(const Project& project, const std::vector<MidiTrackReceiverBinding>& bindings);

    std::size_t receiverCount() const;
    std::size_t activeNoteCount() const;

    // dispatch 先复用工程输出图发送事件，再根据成功送达的事件更新活动音符状态。
    MidiDispatchResult dispatch(const AudioEngineRenderResult& result);

    // releaseAllActiveNotes 为当前活动音符生成 Note Off，并只清除已经成功送达的释放事件。
    MidiDispatchResult releaseAllActiveNotes(int sampleOffset);

private:
    struct ActiveMidiNote {
        std::string trackId;
        std::string clipId;
        std::string noteId;
        int noteNumber = 60;
        int channel = 1;
        int holdCount = 1;
    };

    static bool sameMidiKey(const ActiveMidiNote& note, const MidiPlaybackEvent& event);
    static ScheduledMidiPlaybackEvent releaseEventForActiveNote(
        const ActiveMidiNote& note,
        int sampleOffset);

    void observeDeliveredEvent(const MidiPlaybackEvent& event);

    ProjectMidiOutputGraph graph_;
    std::vector<ActiveMidiNote> activeNotes_;
};

}
