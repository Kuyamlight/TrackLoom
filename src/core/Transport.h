#pragma once

#include <cstdint>

namespace trackloom {

// Transport 只负责播放头状态，不负责打开声卡或生成声音。
// 后续音频设备回调可以调用它，但设备接入会放在独立音频引擎模块。
class Transport {
public:
    Transport();

    bool isPlaying() const;
    void play();
    void stop();

    double sampleRate() const;
    bool setSampleRate(double sampleRate);

    std::int64_t currentSample() const;
    double currentSeconds() const;

    bool seekToSample(std::int64_t sample);
    bool seekToSeconds(double seconds);

    // 按音频块推进播放头。停止状态下接受合法块长度，但不移动位置。
    // 该函数为未来实时音频线程准备，不做分配、文件访问或网络请求。
    bool advanceBySamples(std::int64_t sampleCount);

private:
    bool playing_ = false;
    double sampleRate_ = 44100.0;
    std::int64_t currentSample_ = 0;
};

}
