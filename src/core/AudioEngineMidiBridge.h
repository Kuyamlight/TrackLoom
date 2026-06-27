#pragma once

#include "AudioEngine.h"
#include "MidiDispatch.h"

namespace trackloom {

// dispatchAudioEngineMidiEvents 是 AudioEngine 与 MIDI 输出层之间的显式桥接点。
// AudioEngine 只负责产出 block 内的 scheduled 事件；这里再把这些事件交给抽象接收器。
// 调用方应只在 AudioEngine 渲染成功后调用它，避免发送失败渲染留下的旧结果。
MidiDispatchResult dispatchAudioEngineMidiEvents(
    const AudioEngineRenderResult& result,
    MidiEventReceiver& receiver);

}
