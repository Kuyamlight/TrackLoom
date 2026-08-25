#include "LoopRange.h"
#include "PlaybackTimeConversion.h"
#include "Command.h"
#include "Project.h"
#include "support/TestFailureOutput.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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

void newProjectStartsWithoutPlaybackLoopRange()
{
    trackloom::Project project;

    require(!project.playbackLoopRange().has_value(),
        "a new project must not enable a playback loop range");
}

void projectPlaybackLoopRangeSetterSetsModifiesAndClearsRange()
{
    trackloom::Project project;
    const trackloom::PlaybackLoopRange firstRange { 960, 3840 };
    const trackloom::PlaybackLoopRange secondRange { 1920, 7680 };

    require(project.setPlaybackLoopRange(firstRange),
        "the project loop setter should accept a valid first range");
    require(project.playbackLoopRange() == firstRange,
        "the project loop setter should retain the first valid range");
    require(project.setPlaybackLoopRange(secondRange),
        "the project loop setter should accept a valid replacement range");
    require(project.playbackLoopRange() == secondRange,
        "the project loop setter should replace the old valid range");
    require(project.setPlaybackLoopRange(secondRange),
        "the project loop setter should allow assigning the existing valid range");
    require(project.playbackLoopRange() == secondRange,
        "assigning the existing valid range should retain the project loop range");
    require(project.setPlaybackLoopRange(std::nullopt),
        "the project loop setter should accept an explicit clear");
    require(!project.playbackLoopRange().has_value(),
        "clearing the project loop setter should remove the stored range");
}

void projectPlaybackLoopRangeSetterRejectsInvalidRangeWithoutChangingExistingRange()
{
    trackloom::Project project;
    const trackloom::PlaybackLoopRange preservedRange { 960, 3840 };

    require(project.setPlaybackLoopRange(preservedRange),
        "the valid range should be installed before testing invalid replacements");
    require(!project.setPlaybackLoopRange(trackloom::PlaybackLoopRange { 3840, 3840 }),
        "the project loop setter must reject an empty range");
    require(project.playbackLoopRange() == preservedRange,
        "an empty range must not replace the existing project loop range");
    require(!project.setPlaybackLoopRange(trackloom::PlaybackLoopRange { -1, 3840 }),
        "the project loop setter must reject a negative range start");
    require(project.playbackLoopRange() == preservedRange,
        "a negative range must not replace the existing project loop range");
}

void playbackLoopCommandSetsRangeAndUndoRedoRestoresNoRange()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const trackloom::PlaybackLoopRange range { 960, 3840 };

    const auto execution = commands.execute(
        project,
        std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(range));

    require(execution.success, "setting a valid project loop command should succeed");
    require(project.playbackLoopRange() == range,
        "setting a valid project loop command should store its range");
    require(commands.canUndo(), "a successful loop command should enter undo history");
    require(commands.undo(project), "a set loop command should be undoable");
    require(!project.playbackLoopRange().has_value(),
        "undoing the first loop command should restore no project loop range");
    require(commands.canRedo(), "undoing a loop command should enter redo history");
    require(commands.redo(project), "a set loop command should be redoable");
    require(project.playbackLoopRange() == range,
        "redoing the first loop command should restore its range");
}

void playbackLoopCommandModifiesRangeAndUndoRedoRestoresPriorRange()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const trackloom::PlaybackLoopRange originalRange { 960, 3840 };
    const trackloom::PlaybackLoopRange replacementRange { 1920, 7680 };

    require(project.setPlaybackLoopRange(originalRange),
        "the original valid range should be installed before command modification");
    const auto execution = commands.execute(
        project,
        std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(replacementRange));

    require(execution.success, "replacing a project loop range through a command should succeed");
    require(project.playbackLoopRange() == replacementRange,
        "the replacement command should store the new loop range");
    require(commands.undo(project), "a loop range replacement should be undoable");
    require(project.playbackLoopRange() == originalRange,
        "undoing a loop range replacement should restore the prior range");
    require(commands.redo(project), "a loop range replacement should be redoable");
    require(project.playbackLoopRange() == replacementRange,
        "redoing a loop range replacement should restore the replacement range");
}

void playbackLoopCommandUndoRetainsFirstCapturedRangeAfterExternalRangeChange()
{
    trackloom::Project project;
    const trackloom::PlaybackLoopRange firstRange { 960, 3840 };
    const trackloom::PlaybackLoopRange replacementRange { 1920, 7680 };
    const trackloom::PlaybackLoopRange externalRange { 3840, 11520 };
    trackloom::SetProjectPlaybackLoopCommand command(replacementRange);

    require(project.setPlaybackLoopRange(firstRange),
        "the first valid range should be installed before executing the loop command");
    require(command.execute(project).success,
        "the loop command should first replace the project range");
    require(project.playbackLoopRange() == replacementRange,
        "the first execution should store the replacement range");
    command.undo(project);
    require(project.playbackLoopRange() == firstRange,
        "the first undo should restore the initially captured range");

    require(project.setPlaybackLoopRange(externalRange),
        "an external valid range change should be accepted between command executions");
    require(command.execute(project).success,
        "the same loop command should execute again after an external range change");
    require(project.playbackLoopRange() == replacementRange,
        "the second execution should restore the replacement range");
    command.undo(project);
    require(project.playbackLoopRange() == firstRange,
        "the second undo must restore the first captured range rather than the external range");
}

void playbackLoopCommandClearsRangeAndUndoRedoRestoresPriorRange()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const trackloom::PlaybackLoopRange originalRange { 960, 3840 };

    require(project.setPlaybackLoopRange(originalRange),
        "the valid range should be installed before command clear");
    const auto execution = commands.execute(
        project,
        std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(std::nullopt));

    require(execution.success, "clearing a present project loop range through a command should succeed");
    require(!project.playbackLoopRange().has_value(),
        "the clear loop command should remove the stored range");
    require(commands.undo(project), "a loop range clear should be undoable");
    require(project.playbackLoopRange() == originalRange,
        "undoing a loop range clear should restore the prior range");
    require(commands.redo(project), "a loop range clear should be redoable");
    require(!project.playbackLoopRange().has_value(),
        "redoing a loop range clear should remove the range again");
}

void rejectedPlaybackLoopCommandsPreserveProjectAndCommandHistory()
{
    trackloom::Project project;
    trackloom::CommandStack commands;
    const trackloom::PlaybackLoopRange originalRange { 960, 3840 };
    const trackloom::PlaybackLoopRange redoRange { 1920, 7680 };

    require(project.setPlaybackLoopRange(originalRange),
        "the original valid range should be installed before rejected command checks");
    require(commands.execute(
                project,
                std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(redoRange)).success,
        "the setup loop command should succeed before it creates redo history");
    require(commands.undo(project), "the setup loop command should create redo history when undone");
    require(project.playbackLoopRange() == originalRange,
        "undoing the setup command should restore the original range");
    require(!commands.canUndo() && commands.canRedo(),
        "the setup undo should leave exactly redo history before rejected commands");

    const auto invalid = commands.execute(
        project,
        std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(
            trackloom::PlaybackLoopRange { 3840, 3840 }));
    require(!invalid.success, "an empty playback loop command must be rejected");
    require(project.playbackLoopRange() == originalRange,
        "a rejected invalid loop command must preserve the project loop range");
    require(!commands.canUndo() && commands.canRedo(),
        "a rejected invalid loop command must not change undo or redo history");

    const auto sameRange = commands.execute(
        project,
        std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(originalRange));
    require(!sameRange.success, "a playback loop command for the existing range must be rejected");
    require(project.playbackLoopRange() == originalRange,
        "a rejected same-range command must preserve the project loop range");
    require(!commands.canUndo() && commands.canRedo(),
        "a rejected same-range command must not change undo or redo history");

    require(commands.redo(project), "rejected commands must leave prior redo history usable");
    require(project.playbackLoopRange() == redoRange,
        "redo should still restore the setup command range after rejected commands");

    trackloom::Project emptyProject;
    trackloom::CommandStack emptyCommands;
    const auto clearWithoutRange = emptyCommands.execute(
        emptyProject,
        std::make_unique<trackloom::SetProjectPlaybackLoopCommand>(std::nullopt));
    require(!clearWithoutRange.success,
        "clearing a project without a loop range must be rejected as a no-op");
    require(!emptyProject.playbackLoopRange().has_value(),
        "a rejected empty clear command must preserve no project loop range");
    require(!emptyCommands.canUndo() && !emptyCommands.canRedo(),
        "a rejected empty clear command must not enter command history");
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
        newProjectStartsWithoutPlaybackLoopRange();
        projectPlaybackLoopRangeSetterSetsModifiesAndClearsRange();
        projectPlaybackLoopRangeSetterRejectsInvalidRangeWithoutChangingExistingRange();
        playbackLoopCommandSetsRangeAndUndoRedoRestoresNoRange();
        playbackLoopCommandModifiesRangeAndUndoRedoRestoresPriorRange();
        playbackLoopCommandUndoRetainsFirstCapturedRangeAfterExternalRangeChange();
        playbackLoopCommandClearsRangeAndUndoRedoRestoresPriorRange();
        rejectedPlaybackLoopCommandsPreserveProjectAndCommandHistory();
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
