#include "LoopRange.h"
#include "PlaybackTimeConversion.h"
#include "Project.h"
#include "support/TestFailureOutput.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) {
        std::cerr << message << '\n';
        throw std::runtime_error(message);
    }
}

void playbackLoopRangeAcceptsEveryValidInt64TickBoundary()
{
    using trackloom::isValidPlaybackLoopRange;

    require(isValidPlaybackLoopRange({0, 1}), "[0, 1) should be a valid loop range");
    require(isValidPlaybackLoopRange({0, std::numeric_limits<std::int64_t>::max()}),
        "[0, INT64_MAX) should be a valid loop range");
    require(isValidPlaybackLoopRange({std::numeric_limits<std::int64_t>::max() - 1,
                std::numeric_limits<std::int64_t>::max()}),
        "[INT64_MAX - 1, INT64_MAX) should be a valid loop range");
}

void playbackLoopRangeRejectsNegativeOrEmptyOrReversedRanges()
{
    using trackloom::isValidPlaybackLoopRange;

    require(!isValidPlaybackLoopRange({-1, 1}), "negative loop starts must be rejected");
    require(!isValidPlaybackLoopRange({0, 0}), "empty loop ranges must be rejected");
    require(!isValidPlaybackLoopRange({10, 9}), "reversed loop ranges must be rejected");
}

void playbackTickConversionConvertsDefault120BpmTicksToSamples()
{
    trackloom::Project project;
    std::int64_t samplePosition = -1;

    const auto converted = trackloom::tryConvertPlaybackTickToSample(
        project, 1920, 48000.0, samplePosition);

    require(converted, "two quarter notes at 120 BPM should convert");
    require(samplePosition == 48000,
        "two quarter notes at 120 BPM and 48 kHz should be 48000 samples");
}

void playbackTickConversionRejectsInvalidSampleRatesWithoutWritingOutput()
{
    trackloom::Project project;
    const double invalidRates[] = {
        0.0,
        -48000.0,
        std::numeric_limits<double>::quiet_NaN(),
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };

    for (const auto sampleRate : invalidRates) {
        std::int64_t samplePosition = 12345;
        const auto converted = trackloom::tryConvertPlaybackTickToSample(
            project, 960, sampleRate, samplePosition);
        require(!converted, "zero, negative, and non-finite sample rates must be rejected");
        require(samplePosition == 12345, "failed conversion must preserve the output parameter");
    }
}

void playbackTickConversionKeepsZeroAtTheLowerSampleBoundary()
{
    trackloom::Project project;
    std::int64_t samplePosition = 12345;

    const auto converted = trackloom::tryConvertPlaybackTickToSample(
        project, 0, 48000.0, samplePosition);

    require(converted, "tick zero should convert at a valid sample rate");
    require(samplePosition == 0, "tick zero should convert to the lower sample boundary");
}

void playbackSampleRoundingAcceptsTheInt64LowerBoundary()
{
    std::int64_t samplePosition = 12345;

    const auto converted = trackloom::detail::tryRoundPlaybackSamplePosition(
        -9223372036854775808.0, samplePosition);

    require(converted, "the inclusive -2^63 llround boundary should convert");
    require(samplePosition == std::numeric_limits<std::int64_t>::min(),
        "the inclusive -2^63 boundary should produce INT64_MIN");
}

void playbackSampleRoundingRejectsValuesBelowTheInt64LowerBoundary()
{
    std::int64_t samplePosition = 12345;
    const auto belowMinimum = std::nextafter(
        -9223372036854775808.0, -std::numeric_limits<double>::infinity());

    const auto converted = trackloom::detail::tryRoundPlaybackSamplePosition(
        belowMinimum, samplePosition);

    require(!converted, "values below -2^63 must be rejected before llround");
    require(samplePosition == 12345, "lower-bound failure must preserve the output parameter");
}

void playbackTickConversionAcceptsTheLargestRepresentableRoundedSample()
{
    trackloom::Project project;
    std::int64_t samplePosition = -1;

    const auto converted = trackloom::tryConvertPlaybackTickToSample(
        project, 960, 18446744073709549568.0, samplePosition);

    require(converted, "the largest finite sample position below the exclusive upper bound should convert");
    require(samplePosition == 9223372036854774784LL,
        "the largest representable rounded sample below 2^63 should be retained");
}

void playbackTickConversionRejectsTheExclusiveLlroundUpperBoundary()
{
    trackloom::Project project;
    std::int64_t samplePosition = 12345;

    const auto converted = trackloom::tryConvertPlaybackTickToSample(
        project, 960, 18446744073709551616.0, samplePosition);

    require(!converted, "the exclusive 2^63 llround boundary must be rejected");
    require(samplePosition == 12345, "upper-bound failure must preserve the output parameter");
}

void playbackTickConversionRejectsNonFiniteScaledSamples()
{
    trackloom::Project project;
    std::int64_t samplePosition = 12345;

    const auto converted = trackloom::tryConvertPlaybackTickToSample(
        project, std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<double>::max(), samplePosition);

    require(!converted, "non-finite tick-to-sample products must be rejected");
    require(samplePosition == 12345, "overflow failure must preserve the output parameter");
}

}

int main()
{
    trackloom::test::configureTestFailureOutput();

    try {
        playbackLoopRangeAcceptsEveryValidInt64TickBoundary();
        playbackLoopRangeRejectsNegativeOrEmptyOrReversedRanges();
        playbackTickConversionConvertsDefault120BpmTicksToSamples();
        playbackTickConversionRejectsInvalidSampleRatesWithoutWritingOutput();
        playbackTickConversionKeepsZeroAtTheLowerSampleBoundary();
        playbackSampleRoundingAcceptsTheInt64LowerBoundary();
        playbackSampleRoundingRejectsValuesBelowTheInt64LowerBoundary();
        playbackTickConversionAcceptsTheLargestRepresentableRoundedSample();
        playbackTickConversionRejectsTheExclusiveLlroundUpperBoundary();
        playbackTickConversionRejectsNonFiniteScaledSamples();
    } catch (const std::exception& error) {
        std::cerr << "Test failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "All loop core tests passed.\n";
    return 0;
}
