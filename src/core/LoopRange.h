#pragma once

#include <cstdint>

namespace trackloom {

struct PlaybackLoopRange {
    std::int64_t startTick = 0;
    std::int64_t endTick = 0;

    bool operator==(const PlaybackLoopRange&) const = default;
};

[[nodiscard]] constexpr bool isValidPlaybackLoopRange(
    const PlaybackLoopRange& range) noexcept
{
    return range.startTick >= 0 && range.endTick > range.startTick;
}

}
