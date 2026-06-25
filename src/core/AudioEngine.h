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

    // 清零是当前的安全输出策略：没有音源时也不能让随机内存进入声卡。
    void clear();

private:
    float* samples_ = nullptr;
    int channelCount_ = 0;
    int frameCount_ = 0;
};

// AudioEngine 是最小实时渲染骨架。
// 当前只输出静音并推进 Transport，不打开设备、不解码文件、不运行插件。
class AudioEngine {
public:
    bool prepare(double sampleRate, int channelCount, int maxBlockFrames);

    bool isPrepared() const;
    double sampleRate() const;
    int channelCount() const;
    int maxBlockFrames() const;

    // 渲染函数为未来音频线程准备：不分配内存，不访问文件系统，不调用外部服务。
    bool renderNextBlock(Transport& transport, AudioBlock block);

private:
    bool prepared_ = false;
    double sampleRate_ = 0.0;
    int channelCount_ = 0;
    int maxBlockFrames_ = 0;
};

}
