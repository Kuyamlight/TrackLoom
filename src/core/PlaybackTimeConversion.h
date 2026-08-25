#pragma once

#include <cstdint>

namespace trackloom {

class Project;

[[nodiscard]] bool tryConvertPlaybackTickToSample(
    const Project& project,
    std::int64_t tick,
    double sampleRate,
    std::int64_t& samplePosition);

}
