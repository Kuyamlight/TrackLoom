#include "MidiDispatch.h"

namespace trackloom {
namespace {

bool isValidMidiChannel(int channel)
{
    return channel >= 1 && channel <= 16;
}

bool isValidMidiDataByte(int value)
{
    return value >= 0 && value <= 127;
}

std::optional<std::uint8_t> statusByteForEvent(const MidiPlaybackEvent& event)
{
    const auto channelOffset = static_cast<std::uint8_t>(event.channel - 1);
    switch (event.type) {
    case MidiPlaybackEventType::NoteOn:
        return static_cast<std::uint8_t>(0x90 | channelOffset);
    case MidiPlaybackEventType::NoteOff:
        return static_cast<std::uint8_t>(0x80 | channelOffset);
    }

    return std::nullopt;
}

}

std::optional<MidiOutputMessage> midiOutputMessageForEvent(
    const ScheduledMidiPlaybackEvent& event)
{
    if (event.sampleOffset < 0) {
        return std::nullopt;
    }

    const auto& midiEvent = event.event;
    if (!isValidMidiChannel(midiEvent.channel)
        || !isValidMidiDataByte(midiEvent.noteNumber)
        || !isValidMidiDataByte(midiEvent.velocity)) {
        return std::nullopt;
    }

    const auto statusByte = statusByteForEvent(midiEvent);
    if (!statusByte.has_value()) {
        return std::nullopt;
    }

    return MidiOutputMessage {
        event.sampleOffset,
        *statusByte,
        static_cast<std::uint8_t>(midiEvent.noteNumber),
        static_cast<std::uint8_t>(midiEvent.velocity)
    };
}

MidiDispatchResult dispatchScheduledMidiEvents(
    const std::vector<ScheduledMidiPlaybackEvent>& events,
    MidiEventReceiver& receiver)
{
    MidiDispatchResult result;

    for (std::size_t index = 0; index < events.size(); ++index) {
        ++result.attemptedEventCount;

        const auto message = midiOutputMessageForEvent(events[index]);
        if (!message.has_value()) {
            result.success = false;
            result.failedEventIndex = static_cast<int>(index);
            return result;
        }

        if (!receiver.receiveMidiEvent(events[index], *message)) {
            result.success = false;
            result.failedEventIndex = static_cast<int>(index);
            return result;
        }

        ++result.deliveredEventCount;
    }

    return result;
}

}
