#include "AppTrackListStatus.h"

#include <string>
#include <utility>

namespace trackloom {
namespace {

std::string trackTypeLabel(TrackType type)
{
    switch (type) {
    case TrackType::Instrument:
        return "乐器轨";
    case TrackType::Audio:
        return "音频轨";
    case TrackType::Folder:
        return "文件夹";
    }

    return "未知轨道";
}

std::vector<std::string> trackStateLabels(const Track& track)
{
    std::vector<std::string> labels;

    if (track.playback.muted) {
        labels.push_back("静音");
    }

    if (track.playback.soloed) {
        labels.push_back("独奏");
    }

    if (track.playback.disabled) {
        labels.push_back("禁用");
    }

    if (track.view.hidden) {
        labels.push_back("隐藏");
    }

    if (track.view.collapsed) {
        labels.push_back("折叠");
    }

    return labels;
}

std::string joinedStateLabels(const std::vector<std::string>& labels)
{
    if (labels.empty()) {
        return "正常";
    }

    std::string joined;
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (index > 0) {
            joined += "、";
        }
        joined += labels[index];
    }
    return joined;
}

std::size_t clipCountForTrack(const Project& project, const std::string& trackId)
{
    std::size_t count = 0;
    for (const auto& clip : project.clips()) {
        if (clip.trackId == trackId) {
            ++count;
        }
    }
    return count;
}

std::string rowSummary(const AppTrackListRow& row)
{
    return row.typeLabel + " - " + std::to_string(row.clipCount)
        + " 个片段 - " + joinedStateLabels(row.stateLabels);
}

}

AppTrackListStatus describeAppTrackList(const Project& project)
{
    AppTrackListStatus status;
    status.emptyMessage = "轨道区：暂无轨道。点击“添加乐器轨”创建第一条 MIDI 乐器轨。";

    const auto& tracks = project.tracks();
    status.rows.reserve(tracks.size());

    for (std::size_t index = 0; index < tracks.size(); ++index) {
        const auto& track = tracks[index];

        AppTrackListRow row;
        row.number = index + 1;
        row.trackId = track.id;
        row.name = track.name;
        row.typeLabel = trackTypeLabel(track.type);
        row.clipCount = clipCountForTrack(project, track.id);
        row.stateLabels = trackStateLabels(track);
        row.summary = rowSummary(row);
        status.rows.push_back(std::move(row));
    }

    return status;
}

}
