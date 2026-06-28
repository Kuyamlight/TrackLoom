#include "MidiOutputDevice.h"

namespace trackloom {

MidiOutputDevice::MidiOutputDevice(MidiOutputDevicePort& port)
    : port_(port)
{
}

const MidiOutputDeviceInfo& MidiOutputDevice::info() const
{
    return port_.info();
}

bool MidiOutputDevice::open()
{
    if (!port_.open()) {
        lastFailureReason_ = MidiOutputDeviceFailureReason::OpenRejected;
        return false;
    }

    lastFailureReason_ = MidiOutputDeviceFailureReason::None;
    return true;
}

void MidiOutputDevice::close()
{
    port_.close();
    lastFailureReason_ = MidiOutputDeviceFailureReason::None;
}

bool MidiOutputDevice::isOpen() const
{
    return port_.isOpen();
}

MidiOutputDeviceFailureReason MidiOutputDevice::lastFailureReason() const
{
    return lastFailureReason_;
}

std::size_t MidiOutputDevice::sentMessageCount() const
{
    return sentMessageCount_;
}

bool MidiOutputDevice::receiveMidiEvent(
    const ScheduledMidiPlaybackEvent&,
    const MidiOutputMessage& message)
{
    if (!port_.isOpen()) {
        lastFailureReason_ = MidiOutputDeviceFailureReason::DeviceNotOpen;
        return false;
    }

    if (!port_.sendMidiMessage(message)) {
        lastFailureReason_ = MidiOutputDeviceFailureReason::SendRejected;
        return false;
    }

    ++sentMessageCount_;
    lastFailureReason_ = MidiOutputDeviceFailureReason::None;
    return true;
}

}
