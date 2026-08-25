#pragma once

#include <cstdint>

namespace trackloom {

class Project;

namespace detail {

// 该原语是播放 tick 换算的实现细节；不要把它作为应用层稳定 API 使用。
[[nodiscard]] bool tryRoundPlaybackSamplePosition(
    double scaledSamplePosition,
    std::int64_t& samplePosition);

}

[[nodiscard]] bool tryConvertPlaybackTickToSample(
    const Project& project,
    std::int64_t tick,
    double sampleRate,
    std::int64_t& samplePosition);

}
