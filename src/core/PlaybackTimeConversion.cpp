#include "PlaybackTimeConversion.h"

#include "Project.h"

#include <cmath>
#include <limits>

namespace trackloom {

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
    const auto safeMinimum = static_cast<double>(std::numeric_limits<std::int64_t>::min());
    const auto exclusiveMaximum = -safeMinimum;
    if (!std::isfinite(seconds) || !std::isfinite(scaled)
        || scaled < safeMinimum || scaled >= exclusiveMaximum) {
        return false;
    }

    samplePosition = static_cast<std::int64_t>(std::llround(scaled));
    return true;
}

}
