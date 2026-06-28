#pragma once

#include "AudioEngine.h"
#include "AudioProjectGraph.h"
#include "MidiOutputSession.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace trackloom {

// ProjectPlaybackBlockResult 把一次 block 播放拆成两个结果域。
// renderSucceeded 描述音频引擎是否完成；midiDispatch 描述渲染成功后 MIDI 输出是否送达。
struct ProjectPlaybackBlockResult {
    bool renderSucceeded = false;
    AudioEngineRenderResult renderResult;
    MidiDispatchResult midiDispatch;
};

// ProjectPlaybackControlFailureReason 是会话层播放控制失败的稳定原因码。
// UI 和 AI 工具应读取这个枚举做分支判断，不要解析给人看的 message 字符串。
enum class ProjectPlaybackControlFailureReason {
    None,
    SessionNotPrepared,
    InvalidTargetSample,
    MidiReleaseFailed,
    TransportRejected
};

// ProjectPlaybackControlResult 记录停止、跳转等播放控制命令的结果。
// midiRelease 单独暴露，方便调用方区分“释放失败”和“Transport 改变失败”。
struct ProjectPlaybackControlResult {
    bool success = false;
    ProjectPlaybackControlFailureReason failureReason = ProjectPlaybackControlFailureReason::None;
    MidiDispatchResult midiRelease;
    bool transportChanged = false;
};

// ProjectPlaybackMidiOutputRebuildFailureReason 是安全重建 MIDI 输出的稳定失败原因。
// 它把“旧音符没释放”和“新绑定被拒绝”分开，避免调用方只能看到一个 false。
enum class ProjectPlaybackMidiOutputRebuildFailureReason {
    None,
    SessionNotPrepared,
    MidiReleaseFailed,
    OutputBindingRejected
};

// ProjectPlaybackMidiOutputRebuildResult 记录安全切换 MIDI 输出的结果。
// midiRelease 表示旧输出释放是否成功；midiOutputChanged 表示新绑定是否真正生效。
struct ProjectPlaybackMidiOutputRebuildResult {
    bool success = false;
    ProjectPlaybackMidiOutputRebuildFailureReason failureReason =
        ProjectPlaybackMidiOutputRebuildFailureReason::None;
    MidiDispatchResult midiRelease;
    bool midiOutputChanged = false;
};

// ProjectPlaybackSession 是项目播放的核心协调层。
// 它拥有音频引擎、项目音频图和 MIDI 输出会话，但仍不打开真实声卡、MIDI 端口或插件。
class ProjectPlaybackSession final {
public:
    // prepare 必须先完成，后续重建和渲染才有确定的采样率、声道数和最大 block 大小。
    bool prepare(double sampleRate, int channelCount, int maxBlockFrames);

    bool isPrepared() const;
    double sampleRate() const;
    int channelCount() const;
    int maxBlockFrames() const;

    // 音频图和 MIDI 输出分开重建，因为二者属于不同失败域，不伪装成同一个事务。
    bool rebuildAudioGraph(const Project& project, const std::vector<TrackAudioSourceBinding>& bindings);
    bool rebuildMidiOutput(const Project& project, const std::vector<MidiTrackReceiverBinding>& bindings);

    // rebuildMidiOutputSafely 先通过旧输出释放活动音符，再应用新的 MIDI 输出绑定。
    // 释放失败时不会换路由；新绑定无效时继续保留旧路由。
    ProjectPlaybackMidiOutputRebuildResult rebuildMidiOutputSafely(
        const Project& project,
        const std::vector<MidiTrackReceiverBinding>& bindings,
        int releaseSampleOffset);

    std::size_t audioSourceCount() const;
    std::size_t midiReceiverCount() const;
    std::size_t activeMidiNoteCount() const;

    // 外部发生 seek、重新起播或手动释放后，可请求下一次正在播放的 block 做一次 MIDI chase。
    bool requestMidiChaseOnNextBlock();

    // stopPlayback 会先释放活动 MIDI 音符，再停止 Transport；释放失败时不会停止播放头。
    ProjectPlaybackControlResult stopPlayback(Transport& transport, int releaseSampleOffset);

    // seekPlaybackToSample 会先释放活动 MIDI 音符，再移动 Transport；seek 成功后下一帧会做一次 chase。
    ProjectPlaybackControlResult seekPlaybackToSample(
        Transport& transport,
        std::int64_t targetSample,
        int releaseSampleOffset);

    // renderNextBlock 先渲染音频和收集 scheduled MIDI，再在渲染成功后分发 MIDI。
    // MIDI 分发失败会写入返回值，但不会回滚已经完成的音频 block 或 Transport 推进。
    ProjectPlaybackBlockResult renderNextBlock(
        Transport& transport,
        AudioBlock block,
        const Project& project);

    // renderNextLoopedBlock 显式使用循环 MIDI 调度。
    // 它复用同一个音频图和 MIDI 输出会话，但不会改变普通 renderNextBlock 的行为。
    ProjectPlaybackBlockResult renderNextLoopedBlock(
        Transport& transport,
        AudioBlock block,
        const Project& project,
        const PlaybackLoopRange& loopRange);

    // 停止播放、切换 MIDI 目标或销毁输出前，用这个入口释放会话内仍活动的 MIDI 音符。
    MidiDispatchResult releaseActiveMidiNotes(int sampleOffset);

private:
    bool prepared_ = false;
    AudioEngine audioEngine_;
    ProjectPlaybackGraph audioGraph_;
    MidiOutputSession midiOutput_;
    bool chaseNextMidiBlock_ = true;
};

}
