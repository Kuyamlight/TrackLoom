#include "Transport.h"

#include <cmath>

namespace trackloom {
namespace {

bool isValidNonNegative(double value)
{
    return std::isfinite(value) && value >= 0.0;
}

bool isValidSampleRate(double value)
{
    return std::isfinite(value) && value > 0.0;
}

}

Transport::Transport() = default;

bool Transport::isPlaying() const
{
    return playing_;
}

void Transport::play()
{
    playing_ = true;
}

void Transport::stop()
{
    playing_ = false;
}

double Transport::sampleRate() const
{
    return sampleRate_;
}

bool Transport::setSampleRate(double sampleRate)
{
    if (!isValidSampleRate(sampleRate)) {
        return false;
    }

    sampleRate_ = sampleRate;
    return true;
}

std::int64_t Transport::currentSample() const
{
    return currentSample_;
}

double Transport::currentSeconds() const
{
    return static_cast<double>(currentSample_) / sampleRate_;
}

bool Transport::seekToSample(std::int64_t sample)
{
    if (sample < 0) {
        return false;
    }

    currentSample_ = sample;
    return true;
}

bool Transport::seekToSeconds(double seconds)
{
    if (!isValidNonNegative(seconds)) {
        return false;
    }

    currentSample_ = static_cast<std::int64_t>(std::llround(seconds * sampleRate_));
    return true;
}

bool Transport::advanceBySamples(std::int64_t sampleCount)
{
    if (sampleCount < 0) {
        return false;
    }

    if (playing_) {
        currentSample_ += sampleCount;
    }

    return true;
}

}
