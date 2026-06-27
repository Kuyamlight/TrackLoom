#include "AudioEngineMidiBridge.h"

namespace trackloom {

MidiDispatchResult dispatchAudioEngineMidiEvents(
    const AudioEngineRenderResult& result,
    MidiEventReceiver& receiver)
{
    // 复用 MidiDispatch 的转换和失败处理，避免桥接层再维护一套 MIDI 消息规则。
    return dispatchScheduledMidiEvents(result.scheduledMidiEvents, receiver);
}

}
