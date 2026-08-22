#pragma once

// T6.7 deterministic-input helper: a fixed, documented MsgInput stream that BOTH
// the server's authoritative simulation and the client's local prediction
// consume, so the two simulations run the SAME inputs and must therefore
// produce the SAME state (the deterministic-simulation acceptance proof).
//
// The script encodes a deterministic data-driven controller:
//   0..59    hold forward (moveY = +1; the sample scripts' InputAxis("forward")
//            reads it as +1 and advance the controlled entity's z by 1/tick)
//   60       release forward + press jump (button bit0 / Space; scripts that
//            listen to InputKey("space") see exactly one held frame)
//   61..89   idle
//   90       press interact (button bit3 / F; scripts that listen to
//            InputKey("f") see exactly one held frame)
//   91..end  idle
//
// Ticks past the script's last explicit entry hold no input (all axes/buttons
// zero). GameServer::SetScriptedInputs consumes the same helper as the client
// prediction drivers in the determinism tests, so both sides share the stream
// verbatim. The MsgInput.seq field carries the tick index for traceability.

#include <cstdint>
#include <vector>

#include "neon/net/protocol.hpp"
#include "net_input.hpp"

namespace neon::server {

// One scripted input report: the input that drives the fixed-sim `tick`
// (0-based step index). GameServer::ApplyControllerInput reads the entry whose
// tick equals the tick about to be simulated.
struct ScriptedInput {
    uint32_t tick = 0;
    net::MsgInput input;
};

// The fixed scripted-input sequence (see the file comment for the pattern).
// Explicit entries only: ticks 0..59 forward, tick 60 jump, tick 90 interact;
// every other tick is implicitly idle (InputForTick returns nullptr).
inline std::vector<ScriptedInput> ScriptedInputs() {
    std::vector<ScriptedInput> seq;
    seq.reserve(62);
    auto push = [&seq](uint32_t tick, uint8_t buttons, float moveX, float moveY) {
        net::MsgInput in;
        in.seq = tick;
        in.buttons = buttons;
        in.moveX = moveX;
        in.moveY = moveY;
        seq.push_back(ScriptedInput{tick, in});
    };
    for (uint32_t t = 0; t < 60; ++t) push(t, 0, 0.0f, 1.0f); // hold forward
    push(60, NetInput::kButtonJump, 0.0f, 0.0f);              // stop + jump tap
    push(90, NetInput::kButtonInteract, 0.0f, 0.0f);          // interact tap
    return seq;
}

// The input that drives `tick`, or nullptr when the tick holds no scripted
// input (ticks beyond the sequence are idle). Callers fall back to zero input.
inline const net::MsgInput* InputForTick(const std::vector<ScriptedInput>& seq,
                                         uint32_t tick) {
    for (const ScriptedInput& s : seq)
        if (s.tick == tick) return &s.input;
    return nullptr;
}

} // namespace neon::server
