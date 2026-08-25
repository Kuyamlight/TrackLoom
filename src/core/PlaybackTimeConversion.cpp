#include "PlaybackTimeConversion.h"

#include "Project.h"

#include <cmath>
#include <limits>

namespace trackloom {

namespace detail {

bool tryRoundPlaybackSamplePosition(
    double scaledSamplePosition,
    std::int64_t& samplePosition)
{
    const auto safeMinimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    const auto exclusiveMaximum = -safeMinimum;
    if (!std::isfinite(scaledSamplePosition)
        || scaledSamplePosition < safeMinimum || scaledSamplePosition >= exclusiveMaximum) {
        return false;
    }

    samplePosition = static_cast<std::int64_t>(std::llround(scaledSamplePosition));
    return true;
}

}

bool tryConvertPlaybackTickToSample(
    const Project& project,
    std::int64_t tick,
    double sampleRate,
    std::int64_t& samplePosition)
{
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return false;
    }

    const auto seconds = project.tickToSeconds(tick);
    const auto scaled = static_cast<double>(seconds * sampleRate);
    if (!std::isfinite(seconds)) {
        return false;
    }

    return detail::tryRoundPlaybackSamplePosition(scaled, samplePosition);
}

}
