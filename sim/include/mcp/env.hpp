// A batch of duels for reinforcement learning (trainer/mcporl/env.py).
//
// Each duel has two agent slots (0 and 1). Every step takes one action per
// slot, gates it through the fairness policy (fairness.hpp), steps the duel,
// and returns observations, rewards (reward.hpp) and episode ends. Finished
// duels restart at once with a fresh random start (auto-reset); the
// observation returned for them is the new episode's first.
//
// Observations only use what the player's client knows: its own state (and
// where the arena's edge is, as walls would show), and
// the opponent as its client sees it (the interpolated remote player, see
// tracker.hpp), plus what a human can see of the opponent (its held item and
// its hurt flash). The opponent's health is not visible and not observed.
//
// Any duel can be recorded: its starts and the exact post-fairness inputs of
// every tick, exportable as an oracle combat scenario so the episode can be
// replayed against the real game (tools/export_replay, docs/TRAINING.md).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "mcp/fairness.hpp"
#include "mcp/reward.hpp"

namespace mcp {

struct EnvConfig {
    int32_t maxTicks = 600;         // 30 s; longer episodes are truncated (a draw)
    // The arena: a barrier ring at +-arenaRadius (world.hpp ArenaWorld), well
    // inside the oracle's loaded area (+-128).
    int32_t arenaRadius = 24;
    double minStartDist = 3.0, maxStartDist = 10.0;
    uint32_t weaponMask = 1u << static_cast<uint32_t>(Weapon::Hand);  // weapons drawn at each start
    bool sameWeapon = true;          // both players get the same weapon
    RewardWeights reward{};
    FairnessCaps caps{};
};

// Per-slot action, as floats: forward/back (-1, 0, 1), left/right (-1, 0, 1),
// jump, sprint, click (0 or 1, a tap), the turn this tick in degrees (yaw,
// pitch; the fairness cap still applies), a hotbar key (-1 none, else 0-8)
// and the use key (0 or 1, held while 1).
constexpr int32_t kActionSize = 9;
// Expert-panel cheats for scripted opponents (BatchEnv::tactician).
constexpr uint32_t kCheatSnapAim = 1, kCheatTrueSight = 2, kCheatRangeHit = 4;
constexpr int32_t kObsSize = 50;
// Per finished episode: winner (-1 draw), ticks, truncated, then per slot the counters
// of EpisodeStats::Slot, in order.
constexpr int32_t kSlotStats = 18;
constexpr int32_t kEpisodeStatsSize = 3 + 2 * kSlotStats;

struct EpisodeStats {
    struct Slot {
        float clicks = 0, attacks = 0, hits = 0, damageDealt = 0, damageTaken = 0;
        float aimErrorSum = 0, aimTicks = 0, advantageTicks = 0, disadvantageTicks = 0;
        // Hotbar and shield play: swaps (a hotbar key that changed the held
        // item), swaps to an axe while the opponent's shield was up / down as this
        // player saw it, shields disabled, hits the opponent's shield blocked,
        // damaging hits with a non-axe while the shield was down, attacks sent
        // with an axe, swaps from an axe while the opponent's shield was disabled,
        // and ticks holding the hotbar's highest-damage item.
        float swaps = 0, axeSwapsRaised = 0, axeSwapsLowered = 0, disables = 0, blockedHits = 0, swordHitsLowered = 0,
              axeAttacks = 0, swapBacks = 0, bestTicks = 0;
    };
    Slot s[2];
};

namespace env {

MCP_HD inline float rad(float deg) { return deg * 0.017453292F; }

// Direction a player would have to look to aim at a point (MC: yaw 0 is +z,
// positive pitch looks down).
inline void aimAt(const Vec3& eye, const Vec3& target, float& yaw, float& pitch) {
    double dx = target.x - eye.x, dy = target.y - eye.y, dz = target.z - eye.z;
    yaw = static_cast<float>(std::atan2(-dx, dz) * 57.29577951308232);
    pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 57.29577951308232);
}

inline Vec3 eyeOf(const Player& p) { return Vec3{p.x, p.y + static_cast<double>(CombatConstants::kEyeHeightStanding), p.z}; }
inline Vec3 chestOf(const RemoteView& v) { return Vec3{v.pos.x, v.pos.y + 0.9, v.pos.z}; }

// Angle between where a player looks and its view of the opponent's chest.
inline float aimError(const DuelPlayer& me) {
    float yaw, pitch;
    aimAt(eyeOf(me.client), chestOf(me.view), yaw, pitch);
    float dy = duel::wrapDegrees(yaw - me.client.yRot), dp = pitch - me.client.xRot;
    return std::sqrt(dy * dy + dp * dp);
}

inline bool isAxe(Weapon w) { return stats(w).disableSeconds > 0.0F; }

// A scripted action with nothing pressed (no hotbar key).
inline void clearAction(float* a) {
    for (int32_t j = 0; j < kActionSize; ++j) a[j] = 0.0F;
    a[7] = -1.0F;
}

}  // namespace env

struct BatchEnv {
    EnvConfig cfg;
    ArenaWorld world;
    std::vector<float> sinTab;
    std::vector<Duel> duels;
    std::vector<DuelRewards> rewards;
    std::vector<int32_t> ticks;
    std::vector<EpisodeStats> stats;
    std::vector<DuelStart> starts;  // [2 * n]
    SplitMix64 rng{1};
    FairnessStats fairness;
    // Recording: per recorded duel, the gated inputs of both slots per tick.
    // recording[i]: 0 off, 1 recording the current episode, 2 episode kept.
    std::vector<uint8_t> recording;
    // Scripted-opponent cheats requested for the next step, per slot
    // (kCheat* bits, set by scripted()), and whether the recorded episode of
    // a duel used one that vanilla cannot replay.
    std::vector<uint8_t> cheats;
    std::vector<uint8_t> recordedCheat;
    // Tactician state per slot (see tactician()).
    struct Tactic {
        int32_t strafeDir = 1, strafeTicks = 0, jumpCooldown = 0, attackCooldown = 0, critFallTicks = 0;
        bool wtap = false;
        bool critPhase = false;  // going for a crit: sprint dropped until the swing
        int32_t shieldDown = 0;  // shield user: ticks left with the shield voluntarily lowered
        int32_t stunned = 0;     // shield user: reaction ticks left after its shield was disabled
        bool sawDisable = false;
    };
    std::vector<Tactic> tactics;
    std::vector<std::vector<DuelInput>> recorded;  // [2 * n]
    std::vector<DuelStart> recordedStarts;         // [2 * n], the recorded episode's starts
    std::vector<uint8_t> recordedDone;             // the recorded episode finished (not truncated)
    std::vector<int32_t> recordedNonParity;        // random draws vanilla makes differently (duel.hpp)
    // Per-slot loadouts ([2n]): when set, the slot starts every episode with
    // this hotbar and off hand instead of a weapon drawn from weaponMask.
    std::vector<uint8_t> hasLoadout;
    std::vector<DuelStart> loadouts;
    // Ticks the opponent's shield has been up as each slot's client sees it ([2n]).
    std::vector<int32_t> viewRaisedTicks;

    BatchEnv(int32_t n, uint64_t seed, std::vector<float> table, const EnvConfig& c)
        : cfg(c), world{-60, c.arenaRadius, 4}, sinTab(std::move(table)), duels(n), rewards(n), ticks(n, 0), stats(n), starts(2 * n), rng{seed},
          recording(n, 0), cheats(2 * n, 0), recordedCheat(n, 0), tactics(2 * n), recorded(2 * n), recordedStarts(2 * n), recordedDone(n, 0),
          recordedNonParity(n, 0), hasLoadout(2 * n, 0), loadouts(2 * n), viewRaisedTicks(2 * n, 0) {
        for (int32_t i = 0; i < n; ++i) resetDuel(i);
    }

    int32_t size() const { return static_cast<int32_t>(duels.size()); }

    double uniform() { return static_cast<double>(rng.next() >> 11) * 0x1.0p-53; }

    Weapon drawWeapon() {
        int32_t options[static_cast<int32_t>(Weapon::Count)];
        int32_t k = 0;
        for (int32_t w = 0; w < static_cast<int32_t>(Weapon::Count); ++w)
            if (cfg.weaponMask & (1u << w)) options[k++] = w;
        if (k == 0) return Weapon::Hand;
        return static_cast<Weapon>(options[static_cast<int32_t>(uniform() * k) % k]);
    }

    void resetDuel(int32_t i) {
        // Random placement: a random centre near the origin, a random distance
        // and bearing between the players, each facing roughly the other.
        double cx = (uniform() - 0.5) * 16.0, cz = (uniform() - 0.5) * 16.0;
        double dist = cfg.minStartDist + uniform() * (cfg.maxStartDist - cfg.minStartDist);
        double bearing = uniform() * 6.283185307179586;
        double hx = std::sin(bearing) * dist * 0.5, hz = std::cos(bearing) * dist * 0.5;
        DuelStart a, b;
        // Coordinates on a 1/64 grid and yaws in whole degrees keep scenario
        // files exact and short.
        a.x = std::round((cx - hx) * 64.0) / 64.0;
        a.z = std::round((cz - hz) * 64.0) / 64.0;
        b.x = std::round((cx + hx) * 64.0) / 64.0;
        b.z = std::round((cz + hz) * 64.0) / 64.0;
        float face = static_cast<float>(std::atan2(-(b.x - a.x), b.z - a.z) * 57.29577951308232);
        a.yaw = std::round(face + static_cast<float>((uniform() - 0.5) * 60.0));
        b.yaw = std::round(duel::wrapDegrees(face + 180.0F) + static_cast<float>((uniform() - 0.5) * 60.0));
        a.hotbar[0] = drawWeapon();
        b.hotbar[0] = cfg.sameWeapon ? a.hotbar[0] : drawWeapon();
        for (int32_t k = 0; k < 2; ++k) {
            DuelStart& s = k == 0 ? a : b;
            if (!hasLoadout[2 * i + k]) continue;
            for (int32_t h = 0; h < 9; ++h) s.hotbar[h] = loadouts[2 * i + k].hotbar[h];
            s.offhand = loadouts[2 * i + k].offhand;
        }
        viewRaisedTicks[2 * i] = viewRaisedTicks[2 * i + 1] = 0;
        duels[i].reset(a, b, world, rng.next());
        rewards[i].reset(duels[i]);
        ticks[i] = 0;
        stats[i] = EpisodeStats{};
        tactics[2 * i] = Tactic{};
        tactics[2 * i + 1] = Tactic{};
        starts[2 * i] = a;
        starts[2 * i + 1] = b;
        if (recording[i] == 1) {  // a new episode to record
            recorded[2 * i].clear();
            recorded[2 * i + 1].clear();
            recordedStarts[2 * i] = a;
            recordedStarts[2 * i + 1] = b;
            recordedCheat[i] = 0;
        }
    }

    // Egocentric observation for slot k of duel i (kObsSize floats).
    void observe(int32_t i, int32_t k, float* o) const {
        const Duel& d = duels[i];
        const DuelPlayer& me = d.p[k];
        const DuelPlayer& op = d.p[1 - k];
        const Player& c = me.client;
        float sy = std::sin(env::rad(c.yRot)), cy = std::cos(env::rad(c.yRot));
        // World (x, z) -> (forward, right) in the player's frame; forward is (-sin, cos).
        auto fwd = [&](double x, double z) { return static_cast<float>(-x * sy + z * cy); };
        auto side = [&](double x, double z) { return static_cast<float>(-x * cy - z * sy); };
        const RemoteView& v = me.view;
        Vec3 rel{v.pos.x - c.x, v.pos.y - c.y, v.pos.z - c.z};
        Vec3 vv{v.pos.x - v.old.x, v.pos.y - v.old.y, v.pos.z - v.old.z};
        reach::ReachState r = reachOf(d, k);
        float yawTo, pitchTo;
        env::aimAt(env::eyeOf(c), env::chestOf(v), yawTo, pitchTo);
        // Which way the opponent (as seen) faces, relative to the line towards me.
        float theirYawToMe = static_cast<float>(std::atan2(-(c.x - v.pos.x), c.z - v.pos.z) * 57.29577951308232);
        float facing = env::rad(duel::wrapDegrees(v.yRot - theirYawToMe));
        float delay = attackStrengthDelay(me.server.attrWeapon);
        float strength = mth::clamp((static_cast<float>(me.clientAttackStrengthTicker) + 0.5F) / delay, 0.0F, 1.0F);
        int32_t n = 0;
        o[n++] = me.server.health / 20.0F;
        o[n++] = strength;
        o[n++] = fwd(c.vel.x, c.vel.z) * 4.0F;
        o[n++] = static_cast<float>(c.vel.y) * 2.0F;
        o[n++] = side(c.vel.x, c.vel.z) * 4.0F;
        o[n++] = c.onGround ? 1.0F : 0.0F;
        o[n++] = c.sprinting ? 1.0F : 0.0F;
        o[n++] = std::sin(env::rad(c.xRot));
        o[n++] = std::cos(env::rad(c.xRot));
        o[n++] = fwd(rel.x, rel.z) / 8.0F;
        o[n++] = static_cast<float>(rel.y) / 4.0F;
        o[n++] = side(rel.x, rel.z) / 8.0F;
        o[n++] = fwd(vv.x, vv.z) * 4.0F;
        o[n++] = static_cast<float>(vv.y) * 2.0F;
        o[n++] = side(vv.x, vv.z) * 4.0F;
        o[n++] = std::sin(facing);
        o[n++] = std::cos(facing);
        o[n++] = static_cast<float>(r.mine) / 3.0F;
        o[n++] = r.canHit() ? 1.0F : 0.0F;
        o[n++] = static_cast<float>(r.theirs) / 3.0F;
        o[n++] = r.canBeHit() ? 1.0F : 0.0F;
        o[n++] = static_cast<float>(me.missTime) / 10.0F;
        o[n++] = duel::wrapDegrees(yawTo - c.yRot) / 180.0F;
        o[n++] = (pitchTo - c.xRot) / 90.0F;
        o[n++] = static_cast<float>(me.server.hurtTime) / 10.0F;  // own hurt flash
        o[n++] = static_cast<float>(op.server.hurtTime) / 10.0F;  // the opponent's, visible on it
        o[n++] = static_cast<float>(attackDamageAttribute(me.server.attrWeapon)) / 10.0F;
        o[n++] = delay / 25.0F;
        o[n++] = static_cast<float>(attackDamageAttribute(op.server.mainHand())) / 10.0F;
        o[n++] = attackStrengthDelay(op.server.mainHand()) / 25.0F;
        o[n++] = static_cast<float>(ticks[i]) / static_cast<float>(cfg.maxTicks);
        // Where the arena is: the direction to its centre in the player's
        // frame, and how far the nearest edge is (a wall a human can see).
        double R = static_cast<double>(cfg.arenaRadius);
        o[n++] = fwd(-c.x, -c.z) / static_cast<float>(R);
        o[n++] = side(-c.x, -c.z) / static_cast<float>(R);
        o[n++] = static_cast<float>((R - std::fmax(std::fabs(c.x), std::fabs(c.z))) / R);
        // Hotbar and shields: which of the first three slots is selected, what
        // those slots hold (damage, and whether it disables shields), the own
        // shield (in the off hand, raised, its cooldown), the opponent's (raised
        // as seen, and for how long) and whether it holds an axe or a shield.
        for (int32_t h = 0; h < 3; ++h) o[n++] = me.selected == h ? 1.0F : 0.0F;
        for (int32_t h = 0; h < 3; ++h) {
            Weapon w = me.server.hotbar[h];
            o[n++] = w == Weapon::Hand ? 0.0F : static_cast<float>(attackDamageAttribute(w)) / 10.0F;
            o[n++] = env::isAxe(w) ? 1.0F : 0.0F;
        }
        o[n++] = mcp::stats(me.server.offhand).blocksAttacks ? 1.0F : 0.0F;
        o[n++] = me.usingItem ? 1.0F : 0.0F;
        o[n++] = me.cooldown > 0 ? static_cast<float>(me.cooldown) / static_cast<float>(me.cooldownDuration) : 0.0F;
        o[n++] = me.viewUsing ? 1.0F : 0.0F;
        o[n++] = std::fmin(static_cast<float>(viewRaisedTicks[2 * i + k]) / 10.0F, 1.0F);
        o[n++] = env::isAxe(op.server.mainHand()) ? 1.0F : 0.0F;
        o[n++] = mcp::stats(op.server.offhand).blocksAttacks ? 1.0F : 0.0F;
    }

    void observeAll(float* obs) const {
        for (int32_t i = 0; i < size(); ++i)
            for (int32_t k = 0; k < 2; ++k) observe(i, k, obs + (2 * i + k) * kObsSize);
    }

    static AgentAction decode(const DuelPlayer& me, const float* a) {
        AgentAction act;
        act.keys.forward = a[0] > 0.5F;
        act.keys.backward = a[0] < -0.5F;
        act.keys.right = a[1] > 0.5F;
        act.keys.left = a[1] < -0.5F;
        act.keys.jump = a[2] > 0.5F;
        act.keys.sprint = a[3] > 0.5F;
        // A click is a tap: pressed and released within the tick, as a human
        // spam-clicks. Holding the key would start mining a block the pick
        // lands on, which is outside the supported domain.
        act.click = a[4] > 0.5F;
        act.holdAttack = false;
        act.yaw = me.client.yRot + (std::isfinite(a[5]) ? a[5] : 0.0F);
        act.pitch = me.client.xRot + (std::isfinite(a[6]) ? a[6] : 0.0F);
        act.slot = std::isfinite(a[7]) && a[7] > -0.5F ? static_cast<int32_t>(std::lround(std::fmin(a[7], 8.0F))) : -1;
        act.use = a[8] > 0.5F;
        return act;
    }

    // One step of every duel. actions: [2n][kActionSize]. Outputs: obs
    // [2n][kObsSize], reward [2n], done [n] (1 finished, 2 truncated), and
    // for every duel that ended this step its stats in episodeStats
    // [n][kEpisodeStatsSize] (undefined for the others).
    void step(const float* actions, float* obs, float* reward, uint8_t* done, float* episodeStats) {
        for (int32_t i = 0; i < size(); ++i) {
            Duel& d = duels[i];
            EpisodeStats& st = stats[i];
            DuelInput in[2];
            for (int32_t k = 0; k < 2; ++k) {
                AgentAction act = decode(d.p[k], actions + (2 * i + k) * kActionSize);
                uint8_t cheat = cheats[2 * i + k];
                cheats[2 * i + k] = 0;
                if (cheat & kCheatSnapAim) {
                    // Expert panel: no turn cap (the opponent's own input path only).
                    FairnessCaps free{360.0F};
                    FairnessStats ignore;
                    in[k] = applyFairness(free, d.p[k].client.yRot, d.p[k].client.xRot, d.p[k].lastAttackHeld, act, ignore);
                } else {
                    in[k] = applyFairness(cfg.caps, d.p[k].client.yRot, d.p[k].client.xRot, d.p[k].lastAttackHeld, act, fairness);
                }
                if (cheat & kCheatRangeHit) {
                    in[k].rangeHit = true;
                    if (recording[i] == 1) recordedCheat[i] = 1;
                }
                if (recording[i] == 1) recorded[2 * i + k].push_back(in[k]);
            }
            float before[2] = {d.p[0].server.health, d.p[1].server.health};
            int32_t selBefore[2], cdBefore[2];
            bool raisedSeen[2], blockingBefore[2];
            Weapon heldBefore[2];
            for (int32_t k = 0; k < 2; ++k) {
                selBefore[k] = d.p[k].selected;
                heldBefore[k] = duel::clientInHand(d.p[k], false);
                raisedSeen[k] = d.p[k].viewUsing;
                blockingBefore[k] = d.p[k].server.blocking();
                cdBefore[k] = d.p[k].server.shieldCooldown;
            }
            d.step(in[0], in[1], world, sinTab.data());
            bool dead = d.done();
            int32_t winner = d.winner();
            // The walls keep players in; this only guards against a bug.
            bool out[2] = {false, false};
            for (int32_t k = 0; k < 2; ++k) {
                const Player& c = d.p[k].client;
                out[k] = std::fabs(c.x) > cfg.arenaRadius + 1 || std::fabs(c.z) > cfg.arenaRadius + 1;
            }
            bool left = !dead && (out[0] || out[1]);
            if (left) winner = out[0] == out[1] ? -1 : (out[0] ? 1 : 0);
            // Driving the duel outside what the simulator supports loses too, so
            // no unported corner can be used to escape a fight.
            bool bad[2] = {d.p[0].client.unsupported || d.p[0].server.body.unsupported || d.p[0].view.unsupported,
                           d.p[1].client.unsupported || d.p[1].server.body.unsupported || d.p[1].view.unsupported};
            bool broke = !dead && !left && (bad[0] || bad[1]);
            if (broke) winner = bad[0] == bad[1] ? -1 : (bad[0] ? 1 : 0);
            bool finished = dead || left || broke;
            if (!dead) d.deliver();
            ticks[i]++;
            float r[2];
            rewards[i].step(d, cfg.reward, r, finished, winner);
            // A forfeit (leaving the arena or the supported domain) is worth a
            // death: the loser is charged its remaining health and the winner
            // credited it, or running away would be cheaper than losing a fight.
            if ((left || broke) && winner >= 0) {
                float rest = d.p[1 - winner].server.health;
                r[1 - winner] -= cfg.reward.damageTaken * rest;
                r[winner] += cfg.reward.damageDealt * rest;
            }
            for (int32_t k = 0; k < 2; ++k) {
                EpisodeStats::Slot& s = st.s[k];
                s.clicks += in[k].attack ? 1.0F : 0.0F;
                s.attacks += d.p[k].sentAttack ? 1.0F : 0.0F;
                float dealt = before[1 - k] - d.p[1 - k].server.health;
                if (d.p[k].sentAttack && dealt > 0.0F) s.hits += 1.0F;
                s.damageDealt += dealt;
                s.damageTaken += before[k] - d.p[k].server.health;
                Weapon held = duel::clientInHand(d.p[k], false);
                if (d.p[k].selected != selBefore[k] && held != heldBefore[k]) {
                    s.swaps += 1.0F;
                    if (env::isAxe(held)) (raisedSeen[k] ? s.axeSwapsRaised : s.axeSwapsLowered) += 1.0F;
                    if (env::isAxe(heldBefore[k]) && !env::isAxe(held) && cdBefore[1 - k] > 0) s.swapBacks += 1.0F;
                }
                bool disabled = d.p[1 - k].server.shieldCooldown > cdBefore[1 - k];
                if (disabled) s.disables += 1.0F;
                if (d.p[k].sentAttack && dealt <= 0.0F && !disabled) r[k] -= cfg.reward.wastedAttack;
                if (d.p[k].sentAttack) {
                    if (env::isAxe(heldBefore[k])) s.axeAttacks += 1.0F;
                    if (blockingBefore[1 - k] && dealt <= 0.0F) s.blockedHits += 1.0F;
                    if (dealt > 0.0F && !raisedSeen[k] && !env::isAxe(heldBefore[k])) s.swordHitsLowered += 1.0F;
                }
                {
                    double best = 0.0;
                    for (Weapon w : d.p[k].server.hotbar) best = std::fmax(best, attackDamageAttribute(w));
                    if (attackDamageAttribute(held) >= best) s.bestTicks += 1.0F;
                }
                if (!dead) {
                    s.aimErrorSum += env::aimError(d.p[k]);
                    s.aimTicks += 1.0F;
                    reach::ReachState rs = reachOf(d, k);
                    s.advantageTicks += rs.advantage() > 0 ? 1.0F : 0.0F;
                    s.disadvantageTicks += rs.advantage() < 0 ? 1.0F : 0.0F;
                }
                reward[2 * i + k] = r[k];
            }
            bool truncated = !finished && ticks[i] >= cfg.maxTicks;
            if (truncated) {
                reward[2 * i] -= cfg.reward.draw;
                reward[2 * i + 1] -= cfg.reward.draw;
            }
            done[i] = finished ? 1 : (truncated ? 2 : 0);
            if (finished || truncated) {
                float* e = episodeStats + i * kEpisodeStatsSize;
                e[0] = finished ? static_cast<float>(winner) : -1.0F;
                e[1] = static_cast<float>(ticks[i]);
                e[2] = truncated ? 1.0F : 0.0F;
                for (int32_t k = 0; k < 2; ++k) {
                    const EpisodeStats::Slot& s = st.s[k];
                    float* q = e + 3 + k * kSlotStats;
                    q[0] = s.clicks; q[1] = s.attacks; q[2] = s.hits; q[3] = s.damageDealt; q[4] = s.damageTaken;
                    q[5] = s.aimErrorSum; q[6] = s.aimTicks; q[7] = s.advantageTicks; q[8] = s.disadvantageTicks;
                    q[9] = s.swaps; q[10] = s.axeSwapsRaised; q[11] = s.axeSwapsLowered; q[12] = s.disables;
                    q[13] = s.blockedHits; q[14] = s.swordHitsLowered; q[15] = s.axeAttacks; q[16] = s.swapBacks;
                    q[17] = s.bestTicks;
                }
                if (recording[i] == 1) {
                    recordedDone[i] = dead ? 1 : 0;  // replays end at a death; an arena exit just stops
                    recordedNonParity[i] = d.nonParityEvents() + (d.unsupported() ? 1000 : 0) + (recordedCheat[i] ? 2000 : 0);
                    recording[i] = 2;  // keep this episode; stop recording
                }
                resetDuel(i);
            }
        }
        for (int32_t s = 0; s < 2 * size(); ++s)
            viewRaisedTicks[s] = duels[s / 2].p[s % 2].viewUsing ? viewRaisedTicks[s] + 1 : 0;
        observeAll(obs);
    }

    // Scripted players, for baselines and opponents. kind 0: stands still,
    // facing the opponent. kind 1: an aim bot that walks and sprints in,
    // aims at the chest of what it sees with `noiseDeg` of jitter, turns at
    // most `turnDeg` per tick, and clicks at full attack strength when a hit
    // would land. Writes kActionSize floats for slot k of duel i.
    void scripted(int32_t i, int32_t k, int32_t kind, float noiseDeg, float turnDeg, float* a) {
        const Duel& d = duels[i];
        const DuelPlayer& me = d.p[k];
        env::clearAction(a);
        float yaw, pitch;
        env::aimAt(env::eyeOf(me.client), env::chestOf(me.view), yaw, pitch);
        yaw += static_cast<float>((uniform() - 0.5) * 2.0) * noiseDeg;
        pitch += static_cast<float>((uniform() - 0.5) * 2.0) * noiseDeg;
        float dy = duel::wrapDegrees(yaw - me.client.yRot), dp = pitch - me.client.xRot;
        float m = std::sqrt(dy * dy + dp * dp);
        if (m > turnDeg) {
            dy *= turnDeg / m;
            dp *= turnDeg / m;
        }
        a[5] = dy;
        a[6] = dp;
        if (kind == 0) return;
        reach::ReachState r = reachOf(d, k);
        a[0] = r.mine > 2.6 ? 1.0F : 0.0F;
        a[3] = r.mine > 3.5 ? 1.0F : 0.0F;
        float delay = attackStrengthDelay(me.server.attrWeapon);
        float strength = mth::clamp((static_cast<float>(me.clientAttackStrengthTicker) + 0.5F) / delay, 0.0F, 1.0F);
        a[4] = (r.canHit() && strength >= 1.0F && m < 6.0F) ? 1.0F : 0.0F;
    }

    // The tactician: a port of the melee tactics of the public-domain Fabric mod
    // "PvP Bot" (github.com/Stepan1411/PVP-bot-fabric, BotCombat and
    // BotNavigation): sprint in with a bunny-hop jump every 10 ticks, strafe
    // within 6 blocks (switching every 8-18 ticks), swing only at full attack
    // strength and then wait 10 ticks, crit by dropping sprint, jumping and
    // swinging on the 3rd falling tick, W-tap (one tick without sprint) after
    // each swing. It aims
    // at the eyes of the target with `noiseDeg` of jitter, turning at most
    // `turnDeg` per tick, and plays fair unless `cheatFlags` say otherwise:
    //   kCheatSnapAim:  no turn cap (the mod sets its rotation directly);
    //   kCheatTrueSight: aims at the opponent's true position, not the view;
    //   kCheatRangeHit: a swing lands whenever the true hitbox is in reach,
    //                   without the crosshair pick (the mod calls attack()).
    // The mod's grounded crits (it sets fallDistance) are left out on purpose:
    // they would teach the learner that crits need no jump. Cheats only ever
    // change the opponent's own inputs; the learner always plays fair.
    void tactician(int32_t i, int32_t k, float noiseDeg, float turnDeg, uint32_t cheatFlags, bool crits, float* a) {
        const Duel& d = duels[i];
        const DuelPlayer& me = d.p[k];
        const Player& c = me.client;
        Tactic& T = tactics[2 * i + k];
        env::clearAction(a);
        bool trueSight = (cheatFlags & kCheatTrueSight) != 0;
        const Player& op = d.p[1 - k].client;
        Vec3 target = trueSight ? Vec3{op.x, op.y, op.z} : me.view.pos;
        AABB box = trueSight ? op.boundingBox() : duel::viewBox(me.view);
        // Aim at the eyes.
        float yaw, pitch;
        env::aimAt(env::eyeOf(c), Vec3{target.x, target.y + 1.62, target.z}, yaw, pitch);
        yaw += static_cast<float>((uniform() - 0.5) * 2.0) * noiseDeg;
        pitch += static_cast<float>((uniform() - 0.5) * 2.0) * noiseDeg;
        float dy = duel::wrapDegrees(yaw - c.yRot), dp = pitch - c.xRot;
        float m = std::sqrt(dy * dy + dp * dp);
        bool snap = (cheatFlags & kCheatSnapAim) != 0;
        if (!snap && m > turnDeg) {
            dy *= turnDeg / m;
            dp *= turnDeg / m;
        }
        a[5] = dy;
        a[6] = dp;
        cheats[2 * i + k] = static_cast<uint8_t>(cheatFlags & (kCheatSnapAim | kCheatRangeHit));
        if (T.jumpCooldown > 0) T.jumpCooldown--;
        if (T.attackCooldown > 0) T.attackCooldown--;
        double hx = target.x - c.x, hz = target.z - c.z;
        double dist = std::sqrt(hx * hx + hz * hz);
        // Movement (BotNavigation.moveTowardPos, combat mode).
        a[0] = 1.0F;
        bool sprint = true;
        if (T.wtap && c.onGround) {  // W-tap: one grounded tick without sprint
            sprint = false;
            T.wtap = false;
        }
        a[3] = sprint ? 1.0F : 0.0F;
        if (dist < 6.0) {
            if (T.strafeTicks <= 0) {
                T.strafeDir = -T.strafeDir;
                T.strafeTicks = 8 + static_cast<int32_t>(uniform() * 11.0);
            }
            T.strafeTicks--;
            a[1] = static_cast<float>(T.strafeDir);
        }
        if (dist > 3.5 && c.onGround && T.jumpCooldown <= 0) {  // bunny hop while closing in
            a[2] = 1.0F;
            T.jumpCooldown = 10;
        }
        // Melee (BotCombat: full attack strength, own 10-tick gap, crits).
        Vec3 eye = env::eyeOf(c);
        bool inReach = reach::inRange(reach::eyeToBox(eye, box));
        float delay = attackStrengthDelay(me.server.attrWeapon);
        float strength = mth::clamp((static_cast<float>(me.clientAttackStrengthTicker) + 0.5F) / delay, 0.0F, 1.0F);
        if (inReach && T.attackCooldown <= 0 && strength >= 1.0F) {
            bool swing = false;
            if (!crits) {
                swing = true;
            } else if (c.sprinting) {
                // A crit needs the attacker not sprinting: release W for a tick
                // (sprinting stops without forward input, in the air too).
                a[0] = 0.0F;
                a[3] = 0.0F;
                T.critPhase = true;
            } else if (c.onGround) {
                a[2] = 1.0F;  // jump for the crit
                T.critPhase = true;
                T.critFallTicks = 0;
            } else if (c.vel.y < 0.0) {
                // Swing early in the fall: a jump only falls ~6 ticks on flat
                // ground (the mod's 6 relies on faking fallDistance).
                if (++T.critFallTicks >= 3) swing = true;
            }
            if (swing) {
                a[4] = 1.0F;
                T.attackCooldown = 10;
                T.critFallTicks = 0;
                T.wtap = true;
                T.critPhase = false;
            }
        }
        if (T.critPhase && !c.onGround) a[3] = 0.0F;  // no sprint key until the crit swing
    }

    // A shield user (plays fair: turn cap, the lagged view, the crosshair
    // pick): walks in to ~2.8 blocks aiming at the chest of what it sees and
    // keeps its off-hand shield raised; each tick, with probability
    // `lowerRate`, it lowers it voluntarily for 10 to 40 ticks, and swings its
    // main-hand weapon at full strength while it is down (and while the shield
    // is disabled). At or below `panicHealth` (its own health, which its player
    // sees) it panics: it never lowers the shield voluntarily again, and cuts
    // short a lowered window. lowerRate 0: never lowers it at all. When its
    // shield is disabled it takes `reactTicks` ticks to react (a human's
    // reaction time) before it swings. Needs a shield in the off hand (a loadout).
    void shielder(int32_t i, int32_t k, float noiseDeg, float turnDeg, float lowerRate, float panicHealth,
                  float reactTicks, float* a) {
        const Duel& d = duels[i];
        const DuelPlayer& me = d.p[k];
        const Player& c = me.client;
        Tactic& T = tactics[2 * i + k];
        env::clearAction(a);
        float yaw, pitch;
        env::aimAt(env::eyeOf(c), env::chestOf(me.view), yaw, pitch);
        yaw += static_cast<float>((uniform() - 0.5) * 2.0) * noiseDeg;
        pitch += static_cast<float>((uniform() - 0.5) * 2.0) * noiseDeg;
        float dy = duel::wrapDegrees(yaw - c.yRot), dp = pitch - c.xRot;
        float m = std::sqrt(dy * dy + dp * dp);
        if (m > turnDeg) {
            dy *= turnDeg / m;
            dp *= turnDeg / m;
        }
        a[5] = dy;
        a[6] = dp;
        reach::ReachState r = reachOf(d, k);
        a[0] = r.mine > 2.8 ? 1.0F : 0.0F;
        a[3] = r.mine > 3.5 ? 1.0F : 0.0F;
        bool panicking = me.server.health <= panicHealth;
        if (panicking) {
            T.shieldDown = 0;
        } else if (T.shieldDown > 0) {
            T.shieldDown--;
        } else if (uniform() < static_cast<double>(lowerRate)) {
            T.shieldDown = 10 + static_cast<int32_t>(uniform() * 31.0);
        }
        bool disabled = me.cooldown > 0;
        if (disabled && !T.sawDisable) T.stunned = static_cast<int32_t>(reactTicks);
        T.sawDisable = disabled;
        bool down = T.shieldDown > 0 || disabled;
        a[8] = down ? 0.0F : 1.0F;
        if (T.stunned > 0) {
            T.stunned--;
        } else if (down && !me.usingItem) {
            float delay = attackStrengthDelay(me.server.attrWeapon);
            float strength = mth::clamp((static_cast<float>(me.clientAttackStrengthTicker) + 0.5F) / delay, 0.0F, 1.0F);
            a[4] = (r.canHit() && strength >= 1.0F && m < 6.0F) ? 1.0F : 0.0F;
        }
    }

    // Record duel i's next episode from its start (the duel restarts now).
    void record(int32_t i) {
        recording[i] = 1;
        resetDuel(i);
    }

    // The recorded episode of duel i as an oracle combat scenario.
    std::string scenario(int32_t i, const char* title) const {
        std::string out = "# ";
        out += title;
        out += "\n";
        char buf[256];
        for (int32_t k = 0; k < 2; ++k) {
            const DuelStart& s = recordedStarts[2 * i + k];
            std::snprintf(buf, sizeof buf, "start %s x=%.17g z=%.17g yaw=%.9g health=%.9g", k == 0 ? "A" : "B", s.x, s.z,
                          static_cast<double>(s.yaw), static_cast<double>(s.health));
            out += buf;
            int32_t last = 0;  // the last non-empty hotbar slot
            for (int32_t h = 0; h < 9; ++h)
                if (s.hotbar[h] != Weapon::Hand) last = h;
            if (last == 0) {
                if (s.hotbar[0] != Weapon::Hand) (out += " item=") += mcp::stats(s.hotbar[0]).name;
            } else {
                out += " hotbar=";
                for (int32_t h = 0; h <= last; ++h) {
                    if (h > 0) out += ",";
                    out += s.hotbar[h] == Weapon::Hand ? "-" : mcp::stats(s.hotbar[h]).name;
                }
            }
            if (s.offhand != Weapon::Hand) (out += " offhand=") += mcp::stats(s.offhand).name;
            out += "\n";
        }
        const auto& ra = recorded[2 * i];
        const auto& rb = recorded[2 * i + 1];
        for (size_t t = 0; t < ra.size() && t < rb.size(); ++t) {
            out += "1";
            for (int32_t k = 0; k < 2; ++k) {
                const DuelInput& in = (k == 0 ? ra : rb)[t];
                if (k == 1) out += " |";
                if (in.keys.forward) out += " w";
                if (in.keys.backward) out += " s";
                if (in.keys.left) out += " a";
                if (in.keys.right) out += " d";
                if (in.keys.jump) out += " jump";
                if (in.keys.sprint) out += " sprint";
                if (in.attack && in.attackHeld) out += " attack";
                else if (in.attack) out += " tap";
                else if (in.attackHeld) out += " hold";
                if (in.slot >= 0) out += " slot=" + std::to_string(in.slot);
                if (in.use) out += " use";
                std::snprintf(buf, sizeof buf, " yaw=%.9g pitch=%.9g", static_cast<double>(in.yaw), static_cast<double>(in.pitch));
                out += buf;
            }
            out += "\n";
        }
        return out;
    }
};

}  // namespace mcp
