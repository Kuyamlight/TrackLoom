#include "ProjectSerializer.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace trackloom {
namespace {

bool startsWith(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

bool parseFlagToken(const std::string& token, std::string_view key, bool& value)
{
    const std::string expectedPrefix = std::string(key) + "=";
    if (!startsWith(token, expectedPrefix)) {
        return false;
    }

    const auto rawValue = token.substr(expectedPrefix.size());
    if (rawValue == "0") {
        value = false;
        return true;
    }
    if (rawValue == "1") {
        value = true;
        return true;
    }

    return false;
}

bool parseFloatToken(const std::string& token, std::string_view key, float& value)
{
    const std::string expectedPrefix = std::string(key) + "=";
    if (!startsWith(token, expectedPrefix)) {
        return false;
    }

    const auto rawValue = token.substr(expectedPrefix.size());
    std::istringstream input(rawValue);
    input >> value;

    // 必须完整消费 token，避免 "0.5abc" 这类坏值被误认为合法音量。
    return input && input.eof();
}

bool parseInt64Value(const std::string& token, std::int64_t& value)
{
    std::istringstream input(token);
    input >> value;

    // 必须完整消费 token，避免 "960abc" 这类坏时间值被误认为合法 tick。
    return input && input.eof();
}

bool parseDoubleValue(const std::string& token, double& value)
{
    std::istringstream input(token);
    input >> value;

    // 必须完整消费 token，避免 "120bpm" 这类坏速度值被误认为合法 BPM。
    return input && input.eof();
}

bool parseIntValue(const std::string& token, int& value)
{
    std::istringstream input(token);
    input >> value;

    // 必须完整消费 token，避免 "4/4" 或 "4abc" 被误认为合法拍号字段。
    return input && input.eof();
}

}

LoadProjectResult LoadProjectResult::ok(Project project)
{
    return { std::move(project), "" };
}

LoadProjectResult LoadProjectResult::fail(std::string message)
{
    return { std::nullopt, std::move(message) };
}

std::string saveProjectToText(const Project& project)
{
    std::ostringstream output;
    output << "trackloom_project " << project.formatVersion() << '\n';
    output << "name " << project.name() << '\n';

    for (const auto& event : project.tempoEvents()) {
        output << "tempo " << event.id
               << ' ' << event.tick
               << ' ' << event.beatsPerMinute
               << '\n';
    }

    for (const auto& event : project.timeSignatureEvents()) {
        output << "time_signature " << event.id
               << ' ' << event.tick
               << ' ' << event.numerator
               << ' ' << event.denominator
               << '\n';
    }

    for (const auto& track : project.tracks()) {
        output << "track " << track.id << ' ' << toString(track.type) << ' ' << track.name << '\n';
        output << "track_playback_state " << track.id
               << " muted=" << (track.playback.muted ? 1 : 0)
               << " soloed=" << (track.playback.soloed ? 1 : 0)
               << " disabled=" << (track.playback.disabled ? 1 : 0)
               << '\n';
        output << "track_mix_state " << track.id
               << " gain=" << track.mix.gain
               << " pan=" << track.mix.pan
               << '\n';
        output << "track_view_state " << track.id
               << " hidden=" << (track.view.hidden ? 1 : 0)
               << " collapsed=" << (track.view.collapsed ? 1 : 0)
               << '\n';
    }

    for (const auto& clip : project.clips()) {
        output << "clip " << clip.id
               << ' ' << clip.trackId
               << ' ' << toString(clip.type)
               << ' ' << clip.startTick
               << ' ' << clip.lengthTick
               << ' ' << clip.name
               << '\n';
        for (const auto& note : clip.midiNotes) {
            output << "midi_note " << clip.id
                   << ' ' << note.id
                   << ' ' << note.startTick
                   << ' ' << note.lengthTick
                   << ' ' << note.noteNumber
                   << ' ' << note.velocity
                   << ' ' << note.channel
                   << '\n';
        }
    }

    for (const auto& marker : project.markers()) {
        output << "marker " << marker.id
               << ' ' << marker.tick
               << ' ' << marker.name
               << '\n';
    }

    return output.str();
}

LoadProjectResult loadProjectFromText(const std::string& text)
{
    std::istringstream input(text);
    std::string line;
    std::string keyword;
    int version = 0;

    if (!std::getline(input, line)) {
        return LoadProjectResult::fail("Project header is missing.");
    }

    std::istringstream header(line);
    // v2 增加轨道播放状态；v3 增加轨道混音 gain；v4 增加 pan；v5 增加时间线片段；v6 增加轨道显示状态；v7 增加时间线标记；v8 增加速度图；v9 增加拍号图；v10 增加 MIDI 音符。
    // 旧版本读取后使用当前内存默认值，避免老工程因为新增字段无法打开。
    if (!(header >> keyword >> version) || keyword != "trackloom_project" || version < 1 || version > Project::currentFormatVersion) {
        return LoadProjectResult::fail("Unsupported or invalid project header.");
    }

    if (!std::getline(input, line) || !startsWith(line, "name ")) {
        return LoadProjectResult::fail("Project name is missing.");
    }

    const std::string projectName = line.substr(std::string_view("name ").size());
    Project project(projectName);
    bool loadedDefaultTempoRecord = false;
    bool loadedDefaultTimeSignatureRecord = false;

    while (std::getline(input, line)) {
        if (line.empty()) {
            continue;
        }

        if (startsWith(line, "tempo ")) {
            if (version < 8) {
                return LoadProjectResult::fail("Tempo event requires project version 8.");
            }

            std::istringstream tempoLine(line);
            TempoEvent event;
            std::string tickToken;
            std::string bpmToken;

            if (!(tempoLine >> keyword >> event.id >> tickToken >> bpmToken)) {
                return LoadProjectResult::fail("Invalid tempo record.");
            }
            if (!parseInt64Value(tickToken, event.tick) || !parseDoubleValue(bpmToken, event.beatsPerMinute)) {
                return LoadProjectResult::fail("Invalid tempo value.");
            }

            const auto existingEvent = project.findTempoEventById(event.id);
            if (existingEvent.has_value()) {
                if (event.id != "tempo-1" || loadedDefaultTempoRecord || event.tick != 0) {
                    return LoadProjectResult::fail("Duplicate or invalid tempo event.");
                }
                if (!project.setTempoEventBpm(event.id, event.beatsPerMinute)) {
                    return LoadProjectResult::fail("Invalid default tempo value.");
                }
                loadedDefaultTempoRecord = true;
                continue;
            }

            if (!project.insertExistingTempoEvent(event)) {
                return LoadProjectResult::fail("Duplicate or invalid tempo event.");
            }

            continue;
        }

        if (startsWith(line, "time_signature ")) {
            if (version < 9) {
                return LoadProjectResult::fail("Time signature event requires project version 9.");
            }

            std::istringstream timeSignatureLine(line);
            TimeSignatureEvent event;
            std::string tickToken;
            std::string numeratorToken;
            std::string denominatorToken;

            if (!(timeSignatureLine >> keyword >> event.id >> tickToken >> numeratorToken >> denominatorToken)) {
                return LoadProjectResult::fail("Invalid time signature record.");
            }
            if (!parseInt64Value(tickToken, event.tick)
                || !parseIntValue(numeratorToken, event.numerator)
                || !parseIntValue(denominatorToken, event.denominator)) {
                return LoadProjectResult::fail("Invalid time signature value.");
            }

            const auto existingEvent = project.findTimeSignatureEventById(event.id);
            if (existingEvent.has_value()) {
                if (event.id != "meter-1" || loadedDefaultTimeSignatureRecord || event.tick != 0) {
                    return LoadProjectResult::fail("Duplicate or invalid time signature event.");
                }
                if (!project.setTimeSignature(event.id, event.numerator, event.denominator)) {
                    return LoadProjectResult::fail("Invalid default time signature value.");
                }
                loadedDefaultTimeSignatureRecord = true;
                continue;
            }

            if (!project.insertExistingTimeSignatureEvent(event)) {
                return LoadProjectResult::fail("Duplicate or invalid time signature event.");
            }

            continue;
        }

        // 播放状态单独成行，避免破坏 track 行里“轨道名可以包含空格”的规则。
        if (startsWith(line, "track_playback_state ")) {
            if (version < 2) {
                return LoadProjectResult::fail("Track playback state requires project version 2.");
            }

            std::istringstream stateLine(line);
            std::string trackId;
            std::string mutedToken;
            std::string soloedToken;
            std::string disabledToken;
            TrackPlaybackState state;

            if (!(stateLine >> keyword >> trackId >> mutedToken >> soloedToken >> disabledToken)) {
                return LoadProjectResult::fail("Invalid track playback state record.");
            }
            if (!parseFlagToken(mutedToken, "muted", state.muted)
                || !parseFlagToken(soloedToken, "soloed", state.soloed)
                || !parseFlagToken(disabledToken, "disabled", state.disabled)) {
                return LoadProjectResult::fail("Invalid track playback state value.");
            }
            if (!project.setTrackPlaybackState(trackId, state)) {
                return LoadProjectResult::fail("Track playback state references unknown track.");
            }

            continue;
        }

        if (startsWith(line, "track_mix_state ")) {
            if (version < 3) {
                return LoadProjectResult::fail("Track mix state requires project version 3.");
            }

            std::istringstream mixLine(line);
            std::string trackId;
            std::string gainToken;
            std::string panToken;
            TrackMixState state;

            if (!(mixLine >> keyword >> trackId >> gainToken)) {
                return LoadProjectResult::fail("Invalid track mix state record.");
            }
            if (!parseFloatToken(gainToken, "gain", state.gain) || !isValidTrackMixState(state)) {
                return LoadProjectResult::fail("Invalid track mix state value.");
            }
            if (version >= 4) {
                if (!(mixLine >> panToken) || !parseFloatToken(panToken, "pan", state.pan) || !isValidTrackMixState(state)) {
                    return LoadProjectResult::fail("Invalid track mix state value.");
                }
            }
            if (!project.setTrackMixState(trackId, state)) {
                return LoadProjectResult::fail("Track mix state references unknown track.");
            }

            continue;
        }

        if (startsWith(line, "track_view_state ")) {
            if (version < 6) {
                return LoadProjectResult::fail("Track view state requires project version 6.");
            }

            std::istringstream viewLine(line);
            std::string trackId;
            std::string hiddenToken;
            std::string collapsedToken;
            TrackViewState state;

            if (!(viewLine >> keyword >> trackId >> hiddenToken >> collapsedToken)) {
                return LoadProjectResult::fail("Invalid track view state record.");
            }
            if (!parseFlagToken(hiddenToken, "hidden", state.hidden)
                || !parseFlagToken(collapsedToken, "collapsed", state.collapsed)) {
                return LoadProjectResult::fail("Invalid track view state value.");
            }
            if (!project.setTrackViewState(trackId, state)) {
                return LoadProjectResult::fail("Track view state references unknown or incompatible track.");
            }

            continue;
        }

        if (startsWith(line, "clip ")) {
            if (version < 5) {
                return LoadProjectResult::fail("Timeline clip requires project version 5.");
            }

            std::istringstream clipLine(line);
            TimelineClip clip;
            std::string typeName;
            std::string startTickToken;
            std::string lengthTickToken;

            if (!(clipLine >> keyword >> clip.id >> clip.trackId >> typeName >> startTickToken >> lengthTickToken)) {
                return LoadProjectResult::fail("Invalid clip record.");
            }

            const auto type = clipTypeFromString(typeName);
            if (!type.has_value()
                || !parseInt64Value(startTickToken, clip.startTick)
                || !parseInt64Value(lengthTickToken, clip.lengthTick)) {
                return LoadProjectResult::fail("Invalid clip value.");
            }

            std::getline(clipLine, clip.name);
            if (!clip.name.empty() && clip.name.front() == ' ') {
                clip.name.erase(0, 1);
            }
            if (clip.name.empty()) {
                return LoadProjectResult::fail("Clip name is missing.");
            }

            clip.type = *type;
            if (!project.insertExistingClip(clip)) {
                return LoadProjectResult::fail("Duplicate, invalid, or incompatible clip.");
            }

            continue;
        }

        if (startsWith(line, "midi_note ")) {
            if (version < 10) {
                return LoadProjectResult::fail("MIDI note requires project version 10.");
            }

            std::istringstream noteLine(line);
            MidiNoteEvent note;
            std::string clipId;
            std::string startTickToken;
            std::string lengthTickToken;
            std::string noteNumberToken;
            std::string velocityToken;
            std::string channelToken;

            if (!(noteLine >> keyword
                    >> clipId
                    >> note.id
                    >> startTickToken
                    >> lengthTickToken
                    >> noteNumberToken
                    >> velocityToken
                    >> channelToken)) {
                return LoadProjectResult::fail("Invalid MIDI note record.");
            }

            if (!parseInt64Value(startTickToken, note.startTick)
                || !parseInt64Value(lengthTickToken, note.lengthTick)
                || !parseIntValue(noteNumberToken, note.noteNumber)
                || !parseIntValue(velocityToken, note.velocity)
                || !parseIntValue(channelToken, note.channel)) {
                return LoadProjectResult::fail("Invalid MIDI note value.");
            }

            if (!project.insertExistingMidiNote(clipId, note)) {
                return LoadProjectResult::fail("Duplicate, invalid, or incompatible MIDI note.");
            }

            continue;
        }

        if (startsWith(line, "marker ")) {
            if (version < 7) {
                return LoadProjectResult::fail("Timeline marker requires project version 7.");
            }

            std::istringstream markerLine(line);
            TimelineMarker marker;
            std::string tickToken;

            if (!(markerLine >> keyword >> marker.id >> tickToken)) {
                return LoadProjectResult::fail("Invalid marker record.");
            }

            if (!parseInt64Value(tickToken, marker.tick)) {
                return LoadProjectResult::fail("Invalid marker value.");
            }

            std::getline(markerLine, marker.name);
            if (!marker.name.empty() && marker.name.front() == ' ') {
                marker.name.erase(0, 1);
            }
            if (marker.name.empty()) {
                return LoadProjectResult::fail("Marker name is missing.");
            }

            if (!project.insertExistingMarker(marker)) {
                return LoadProjectResult::fail("Duplicate or invalid marker.");
            }

            continue;
        }

        if (startsWith(line, "track ")) {
            std::istringstream trackLine(line);
            Track track;
            std::string typeName;
            if (!(trackLine >> keyword >> track.id >> typeName)) {
                return LoadProjectResult::fail("Invalid track record.");
            }

            std::getline(trackLine, track.name);
            if (!track.name.empty() && track.name.front() == ' ') {
                track.name.erase(0, 1);
            }
            if (track.name.empty()) {
                return LoadProjectResult::fail("Track name is missing.");
            }

            const auto type = trackTypeFromString(typeName);
            if (!type.has_value()) {
                return LoadProjectResult::fail("Unknown track type.");
            }

            track.type = *type;
            if (!project.insertExistingTrack(track)) {
                return LoadProjectResult::fail("Duplicate or invalid track id.");
            }

            continue;
        }

        return LoadProjectResult::fail("Unknown project record.");
    }

    return LoadProjectResult::ok(std::move(project));
}

}
