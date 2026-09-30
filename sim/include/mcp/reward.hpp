// Per-tick rewards for a duel, for both players (docs/REWARD.md).
//
//   r_i = damageDealt * (health the opponent lost)
//       - damageTaken * (health player i lost)
//       + win / -loss on the tick the episode ends
//       + reachShaping * (gamma * phi_i(s') - phi_i(s))
//
// phi_i is the reach potential: +1 if a perfectly aimed click by i would land
// next tick, -1 if one by the opponent would land on i, both or neither 0.
// "Would land" is measured the way the game decides it: from the player's eye
// to the hitbox of the opponent *as that player's client sees it* (the
// interpolated remote player), strictly inside the 3.0 pick range. Standing
// one block lower makes the low player's reach line horizontal and the high
// player's diagonal, so the height advantage shows up here without any
// special case.
//
// The reach term is potential-based shaping (Ng, Harada and Russell 1999):
// over an episode it telescopes to -phi(start) (phi is 0 at the terminal
// state), so it cannot change which policy is optimal. It pays for getting
// into range and charges for letting the opponent in, but staying in range
// earns (gamma - 1) * phi <= 0 per tick: there is nothing to farm by hovering.
// `gamma` must be the discount the learner uses for that guarantee to hold.
#pragma once

#include "mcp/duel.hpp"
#include "mcp/reach.hpp"

#include <cmath>

namespace mcp {

struct RewardWeights {
    float damageDealt = 1.0F;
    float damageTaken = 1.0F;
    float win = 10.0F;
    float loss = 10.0F;
    float reachShaping = 0.1F;
    float gamma = 0.99F;
    float aimShaping = 0.2F;
    // Dense training aids, paid every tick: `aimDense` x the aim potential and
    // `reachDense` while a perfectly aimed click would land ("time in hit
    // range"). Unlike the potential-based terms these change what is optimal
    // and can be farmed, so the trainer anneals them to 0 (docs/REWARD.md).
    float aimDense = 0.0F;
    float reachDense = 0.0F;
};

// Reach as each client would resolve a click at the start of the next tick:
// its own eye (the client copy, which the pick uses) against its view of the
// opponent. Call after Duel::deliver.
MCP_HD inline reach::ReachState reachOf(const Duel& d, int32_t i) {
    const DuelPlayer& me = d.p[i];
    const DuelPlayer& op = d.p[1 - i];
    double eye = static_cast<double>(CombatConstants::kEyeHeightStanding);
    Vec3 myEye{me.client.x, me.client.y + eye, me.client.z};
    Vec3 opEye{op.client.x, op.client.y + eye, op.client.z};
    return reach::ReachState{reach::eyeToBox(myEye, duel::viewBox(me.view)),
                             reach::eyeToBox(opEye, duel::viewBox(op.view))};
}

MCP_HD inline float reachPotential(const reach::ReachState& r) { return static_cast<float>(r.advantage()); }

// Aim potential: 1 when the crosshair points at the chest of the opponent as
// this client sees it, falling linearly to 0 at 180 degrees off (it must slope
// everywhere: flat beyond 90 degrees, a random early policy that has turned
// away gets no signal which way to turn back). Also potential-based, so it
// teaches pointing at the opponent without changing which policy is optimal.
inline float aimPotential(const Duel& d, int32_t i) {
    const DuelPlayer& me = d.p[i];
    double eye = static_cast<double>(CombatConstants::kEyeHeightStanding);
    double dx = me.view.pos.x - me.client.x, dz = me.view.pos.z - me.client.z;
    double dy = me.view.pos.y + 0.9 - (me.client.y + eye);
    float yaw = static_cast<float>(std::atan2(-dx, dz) * 57.29577951308232);
    float pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 57.29577951308232);
    float ey = duel::wrapDegrees(yaw - me.client.yRot), ep = pitch - me.client.xRot;
    float err = std::sqrt(ey * ey + ep * ep);
    return 1.0F - (err < 180.0F ? err : 180.0F) / 180.0F;
}

// Remembers the previous state's health and potentials between ticks.
struct DuelRewards {
    float health[2] = {0.0F, 0.0F};
    float phi[2] = {0.0F, 0.0F};
    float aim[2] = {0.0F, 0.0F};

    // At the start of an episode (after Duel::reset).
    MCP_HD void reset(const Duel& d) {
        for (int32_t i = 0; i < 2; ++i) {
            health[i] = d.p[i].server.health;
            phi[i] = reachPotential(reachOf(d, i));
            aim[i] = aimPotential(d, i);
        }
    }

    // After Duel::step (and Duel::deliver unless the episode ended): the
    // rewards for the transition just taken.
    MCP_HD void step(const Duel& d, const RewardWeights& w, float out[2]) { step(d, w, out, d.done(), d.winner()); }

    // The same with the episode's outcome given: an environment rule (such as
    // leaving the arena) can end an episode the duel itself has not.
    MCP_HD void step(const Duel& d, const RewardWeights& w, float out[2], bool done, int32_t winner) {
        float lost[2];
        for (int32_t i = 0; i < 2; ++i) {
            lost[i] = health[i] - d.p[i].server.health;
            health[i] = d.p[i].server.health;
        }
        for (int32_t i = 0; i < 2; ++i) {
            float r = w.damageDealt * lost[1 - i] - w.damageTaken * lost[i];
            if (done) r += winner == i ? w.win : (winner == 1 - i ? -w.loss : 0.0F);
            float next = done ? 0.0F : reachPotential(reachOf(d, i));  // phi(terminal) = 0
            r += w.reachShaping * (w.gamma * next - phi[i]);
            phi[i] = next;
            float nextAim = done ? 0.0F : aimPotential(d, i);
            r += w.aimShaping * (w.gamma * nextAim - aim[i]);
            aim[i] = nextAim;
            if (!done) {
                r += w.aimDense * nextAim;
                r += w.reachDense * (reachOf(d, i).canHit() ? 1.0F : 0.0F);
            }
            out[i] = r;
        }
    }
};

}  // namespace mcp
