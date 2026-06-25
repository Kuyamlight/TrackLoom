#pragma once

#include "Transport.h"

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
    void resetPhase();

    bool render(AudioBlock block, double sampleRate) override;

private:
    double frequencyHz_ = 440.0;
    float gain_ = 0.1f;
    double phaseRadians_ = 0.0;
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

private:
    bool prepared_ = false;
    double sampleRate_ = 0.0;
    int channelCount_ = 0;
    int maxBlockFrames_ = 0;
};

}
