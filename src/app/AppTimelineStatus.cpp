#include "AppTimelineStatus.h"

#include <algorithm>
#include <string>
#include <utility>

namespace trackloom {
namespace {

std::string clipTypeLabel(ClipType type)
{
    switch (type) {
    case ClipType::Midi:
        return "MIDI";
    case ClipType::Audio:
        return "音频";
    }

    return "未知";
}

std::string trackNameForClip(const Project& project, const TimelineClip& clip)
{
    const auto track = project.findTrackById(clip.trackId);
    if (track.has_value()) {
        return track->name;
    }

    return "未知轨道";
}

std::string rowSummary(const AppTimelineClipRow& row)
{
    std::string summary = row.trackName
        + " - 起点 " + std::to_string(row.startTick)
        + " tick - 长度 " + std::to_string(row.lengthTick)
        + " tick";

    if (row.typeLabel == "MIDI") {
        summary += " - " + std::to_string(row.noteCount) + " 个音符";
        if (row.hasLastMidiNote) {
            summary += " - 末尾音符：音符起点 " + std::to_string(row.lastMidiNoteStartTick)
                + " tick，长度 " + std::to_string(row.lastMidiNoteLengthTick)
                + " tick，音高 " + std::to_string(row.lastMidiNoteNumber)
                + "，力度 " + std::to_string(row.lastMidiNoteVelocity);
        }
    }

    return summary;
}

const MidiNoteEvent* lastNoteInTimelineOrder(const TimelineClip& clip)
{
    if (clip.midiNotes.empty()) {
        return nullptr;
    }

    // 首屏编辑入口也用这个顺序选择“末尾音符”；摘要必须和编辑目标一致。
    return &*std::max_element(
        clip.midiNotes.begin(),
        clip.midiNotes.end(),
        [](const MidiNoteEvent& left, const MidiNoteEvent& right) {
            const auto leftEndTick = left.startTick + left.lengthTick;
            const auto rightEndTick = right.startTick + right.lengthTick;
            if (leftEndTick != rightEndTick) {
                return leftEndTick < rightEndTick;
            }

            return left.startTick < right.startTick;
        });
}

void fillLastMidiNoteDetails(AppTimelineClipRow& row, const TimelineClip& clip)
{
    if (clip.type != ClipType::Midi) {
        return;
    }

    const auto* lastNote = lastNoteInTimelineOrder(clip);
    if (lastNote == nullptr) {
        return;
    }

    row.hasLastMidiNote = true;
    row.lastMidiNoteStartTick = lastNote->startTick;
    row.lastMidiNoteLengthTick = lastNote->lengthTick;
    row.lastMidiNoteNumber = lastNote->noteNumber;
    row.lastMidiNoteVelocity = lastNote->velocity;
}

}

AppTimelineStatus describeAppTimeline(const Project& project)
{
    AppTimelineStatus status;
    status.emptyMessage = "时间线：暂无片段。请选择乐器轨创建 MIDI 片段，或选择音频轨创建音频片段。";

    const auto& clips = project.clips();
    status.rows.reserve(clips.size());

    for (std::size_t index = 0; index < clips.size(); ++index) {
        const auto& clip = clips[index];

        AppTimelineClipRow row;
        row.number = index + 1;
        row.clipId = clip.id;
        row.trackId = clip.trackId;
        row.trackName = trackNameForClip(project, clip);
        row.name = clip.name;
        row.type = clip.type;
        row.typeLabel = clipTypeLabel(clip.type);
        row.startTick = clip.startTick;
        row.lengthTick = clip.lengthTick;
        row.noteCount = clip.midiNotes.size();
        fillLastMidiNoteDetails(row, clip);
        row.summary = rowSummary(row);
        status.rows.push_back(std::move(row));
    }

    return status;
}

}
