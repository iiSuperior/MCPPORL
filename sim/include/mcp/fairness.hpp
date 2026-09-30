// The fairness policy (docs/ARCHITECTURE.md, "Input plausibility"): every
// agent action passes through this gate before it reaches the duel.
//
// Aim to hit is not a check here but structural: the duel resolves a click
// with the vanilla client's crosshair pick (duel.hpp, pick.hpp), so an attack
// packet only exists when the aim ray hits the target within reach, and a
// click that misses costs what it costs a human (a punch that resets the
// server's attack strength, and the whiff lockout while the key is held).
//
// The gate adds what a human hand cannot exceed:
//   - turn rate: the rotation change per tick is capped (default 200 degrees,
//     enough for the 180 hit, not for snapping away and back);
//   - clicks: at most one per tick (DuelInput carries one by construction);
//   - key state: the attack key can only be held if it was pressed this tick
//     or held on the previous one.
// Reaction time is not limited.
#pragma once

#include <cstdint>

#include "mcp/duel.hpp"

namespace mcp {

// Mirrors trainer/mcporl/config.py FairnessCaps (a contract setting).
struct FairnessCaps {
    float maxTurnDegPerTick = 200.0F;
};

// What the policy asks for on one tick.
struct AgentAction {
    Keys keys{};
    float yaw = 0.0F, pitch = 0.0F;  // desired absolute rotation
    bool click = false;              // press the attack key (at most one click per tick)
    bool holdAttack = false;         // keep the attack key down while the tick samples it
};

struct FairnessStats {
    int64_t turnsClamped = 0;
    int64_t holdsRejected = 0;
};

// Turn the action into inputs a human could have produced, given where the
// player's view currently is and whether the attack key was down last tick.
MCP_HD inline DuelInput applyFairness(const FairnessCaps& caps, float fromYaw, float fromPitch, bool heldLastTick,
                                      const AgentAction& a, FairnessStats& stats) {
    DuelInput in;
    in.keys = a.keys;
    // Pitch is clamped to [-90, 90] as Entity.turn does. Yaw is unbounded in
    // vanilla; turn by the shortest way round from the current yaw.
    float targetPitch = mth::clamp(a.pitch, -90.0F, 90.0F);
    float dYaw = duel::wrapDegrees(a.yaw - fromYaw);
    float dPitch = targetPitch - fromPitch;
    float angle = static_cast<float>(::sqrt(static_cast<double>(dYaw * dYaw + dPitch * dPitch)));
    if (angle > caps.maxTurnDegPerTick) {
        float s = caps.maxTurnDegPerTick / angle;
        dYaw *= s;
        dPitch *= s;
        stats.turnsClamped++;
    }
    in.yaw = fromYaw + dYaw;
    in.pitch = mth::clamp(fromPitch + dPitch, -90.0F, 90.0F);
    in.attack = a.click;
    in.attackHeld = a.holdAttack && (a.click || heldLastTick);
    if (a.holdAttack && !in.attackHeld) stats.holdsRejected++;
    return in;
}

// Convenience: gate both players' actions and step the duel.
template <typename World>
MCP_HD void stepFair(Duel& d, const FairnessCaps& caps, const AgentAction& a, const AgentAction& b, const World& w,
                     const float* sinTab, FairnessStats& stats) {
    DuelInput ia = applyFairness(caps, d.p[0].client.yRot, d.p[0].client.xRot, d.p[0].lastAttackHeld, a, stats);
    DuelInput ib = applyFairness(caps, d.p[1].client.yRot, d.p[1].client.xRot, d.p[1].lastAttackHeld, b, stats);
    d.step(ia, ib, w, sinTab);
}

}  // namespace mcp
