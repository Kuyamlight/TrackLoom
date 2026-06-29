#include "AppTimelineStatus.h"

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
    }

    return summary;
}

}

AppTimelineStatus describeAppTimeline(const Project& project)
{
    AppTimelineStatus status;
    status.emptyMessage = "时间线：暂无 MIDI 片段。请选择乐器轨并点击“创建 MIDI 片段”。";

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
        row.summary = rowSummary(row);
        status.rows.push_back(std::move(row));
    }

    return status;
}

}
