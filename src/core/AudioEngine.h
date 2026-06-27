#pragma once

#include "PlaybackClock.h"
#include "Project.h"
#include "Transport.h"

#include <vector>

namespace trackloom {

// AudioBlock 是非拥有型音频缓冲区视图。
// 它不分配也不释放内存，只解释调用方传入的 float 样本区域。
class AudioBlock {
public:
    AudioBlock(float* samples, int channelCount, int frameCount);

    bool isValid() const;
    int channelCount() const;
    int frameCount() const;

    float& sampleAt(int channel, int frame);
    const float& sampleAt(int channel, int frame) const;

    // 没有音源或渲染停止时仍要清零，避免随机内存被送进后续音频输出。
    void clear();

private:
    float* samples_ = nullptr;
    int channelCount_ = 0;
    int frameCount_ = 0;
};

// AudioSource 是音频引擎的最小音源接口。
// 后续轨道、采样器或插件宿主都可以实现它，但接口本身不绑定具体功能。
class AudioSource {
public:
    virtual ~AudioSource() = default;

    // render 只写入调用方提供的缓冲区，不拥有内存，也不执行文件或网络操作。
    virtual bool render(AudioBlock block, double sampleRate) = 0;
};

// SineToneSource 是确定性测试音源，不是正式乐器功能。
// 它用于证明渲染链路能产生可预测的非静音样本。
class SineToneSource final : public AudioSource {
public:
    bool setFrequency(double frequencyHz);
    bool setGain(float gain);

    bool render(AudioBlock block, double sampleRate) override;

private:
    double frequencyHz_ = 440.0;
    float gain_ = 0.1f;
    double phaseRadians_ = 0.0;
};

// AudioEngineRenderResult 保存一次音频 block 渲染附带的非音频结果。
// 当前只暴露 MIDI 播放事件；后续如果要加入计量、诊断或插件延迟信息，也应放在这里。
struct AudioEngineRenderResult {
    std::vector<MidiPlaybackEvent> midiEvents;
    std::vector<ScheduledMidiPlaybackEvent> scheduledMidiEvents;
};

// AudioEngine 是最小实时渲染骨架。
// 当前不打开设备、不解码文件、不运行插件，只负责清理缓冲区、调用音源并推进 Transport。
class AudioEngine {
public:
    bool prepare(double sampleRate, int channelCount, int maxBlockFrames);

    bool isPrepared() const;
    double sampleRate() const;
    int channelCount() const;
    int maxBlockFrames() const;

    // 默认无音源渲染保持静音，兼容上一阶段行为。
    bool renderNextBlock(Transport& transport, AudioBlock block);

    // 带音源的渲染只在 Transport 播放时写入非静音样本。
    bool renderNextBlock(Transport& transport, AudioBlock block, AudioSource* source);

    // MIDI-aware 渲染在推进 Transport 前收集当前 block 的 MIDI 事件。
    // 它只返回事件，不发送到设备或插件；旧的 renderNextBlock 行为保持不变。
    bool renderNextBlockWithMidi(
        Transport& transport,
        AudioBlock block,
        const Project& project,
        AudioEngineRenderResult& result);

    // 显式 chase 版本供上层播放会话在起播或 seek 后使用；普通连续播放应保持 Disabled。
    bool renderNextBlockWithMidi(
        Transport& transport,
        AudioBlock block,
        const Project& project,
        MidiChaseMode chaseMode,
        AudioEngineRenderResult& result);

    // 带音源版本用于后续把项目播放图和 MIDI 调度放在同一个 block 生命周期内。
    bool renderNextBlockWithMidi(
        Transport& transport,
        AudioBlock block,
        AudioSource* source,
        const Project& project,
        AudioEngineRenderResult& result);

    bool renderNextBlockWithMidi(
        Transport& transport,
        AudioBlock block,
        AudioSource* source,
        const Project& project,
        MidiChaseMode chaseMode,
        AudioEngineRenderResult& result);

    // loop-aware MIDI 渲染显式使用调用方传入的循环范围。
    // 它不会修改普通播放入口，也不代表音频素材已经具备完整循环回放能力。
    bool renderNextBlockWithLoopedMidi(
        Transport& transport,
        AudioBlock block,
        const Project& project,
        const PlaybackLoopRange& loopRange,
        AudioEngineRenderResult& result);

    bool renderNextBlockWithLoopedMidi(
        Transport& transport,
        AudioBlock block,
        const Project& project,
        const PlaybackLoopRange& loopRange,
        MidiChaseMode chaseMode,
        AudioEngineRenderResult& result);

    // 带音源版本沿用同一 block 生命周期：先收集循环 MIDI，再渲染音源，最后推进 Transport。
    bool renderNextBlockWithLoopedMidi(
        Transport& transport,
        AudioBlock block,
        AudioSource* source,
        const Project& project,
        const PlaybackLoopRange& loopRange,
        AudioEngineRenderResult& result);

    bool renderNextBlockWithLoopedMidi(
        Transport& transport,
        AudioBlock block,
        AudioSource* source,
        const Project& project,
        const PlaybackLoopRange& loopRange,
        MidiChaseMode chaseMode,
        AudioEngineRenderResult& result);

private:
    bool prepared_ = false;
    double sampleRate_ = 0.0;
    int channelCount_ = 0;
    int maxBlockFrames_ = 0;
};

}
