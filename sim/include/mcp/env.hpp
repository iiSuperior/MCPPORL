// A batch of duels for reinforcement learning (trainer/mcporl/env.py).
//
// Each duel has two agent slots (0 and 1). Every step takes one action per
// slot, gates it through the fairness policy (fairness.hpp), steps the duel,
// and returns observations, rewards (reward.hpp) and episode ends. Finished
// duels restart at once with a fresh random start (auto-reset); the
// observation returned for them is the new episode's first.
//
// Observations only use what the player's client knows: its own state, and
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
    // Leaving the arena (|x| or |z| beyond this) loses the episode, as a wall
    // would stop a real player; within the oracle's loaded area (+-128).
    double arenaRadius = 24.0;
    double minStartDist = 3.0, maxStartDist = 10.0;
    uint32_t weaponMask = 1u << static_cast<uint32_t>(Weapon::Hand);  // weapons drawn at each start
    bool sameWeapon = true;          // both players get the same weapon
    RewardWeights reward{};
    FairnessCaps caps{};
};

// Per-slot action, as floats: forward/back (-1, 0, 1), left/right (-1, 0, 1),
// jump, sprint, click (0 or 1, a tap), then the turn this tick in degrees
// (yaw, pitch; the fairness cap still applies).
constexpr int32_t kActionSize = 7;
constexpr int32_t kObsSize = 31;
// Per finished episode: winner (-1 draw), ticks, truncated, then per slot the counters
// of EpisodeStats::Slot, in order.
constexpr int32_t kSlotStats = 9;
constexpr int32_t kEpisodeStatsSize = 3 + 2 * kSlotStats;

struct EpisodeStats {
    struct Slot {
        float clicks = 0, attacks = 0, hits = 0, damageDealt = 0, damageTaken = 0;
        float aimErrorSum = 0, aimTicks = 0, advantageTicks = 0, disadvantageTicks = 0;
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

}  // namespace env

struct BatchEnv {
    EnvConfig cfg;
    FlatWorld world;
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
    std::vector<std::vector<DuelInput>> recorded;  // [2 * n]
    std::vector<DuelStart> recordedStarts;         // [2 * n], the recorded episode's starts
    std::vector<uint8_t> recordedDone;             // the recorded episode finished (not truncated)
    std::vector<int32_t> recordedNonParity;        // random draws vanilla makes differently (duel.hpp)

    BatchEnv(int32_t n, uint64_t seed, std::vector<float> table, const EnvConfig& c)
        : cfg(c), sinTab(std::move(table)), duels(n), rewards(n), ticks(n, 0), stats(n), starts(2 * n), rng{seed},
          recording(n, 0), recorded(2 * n), recordedStarts(2 * n), recordedDone(n, 0),
          recordedNonParity(n, 0) {
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
        a.weapon = drawWeapon();
        b.weapon = cfg.sameWeapon ? a.weapon : drawWeapon();
        duels[i].reset(a, b, world, rng.next());
        rewards[i].reset(duels[i]);
        ticks[i] = 0;
        stats[i] = EpisodeStats{};
        starts[2 * i] = a;
        starts[2 * i + 1] = b;
        if (recording[i] == 1) {  // a new episode to record
            recorded[2 * i].clear();
            recorded[2 * i + 1].clear();
            recordedStarts[2 * i] = a;
            recordedStarts[2 * i + 1] = b;
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
        float delay = attackStrengthDelay(me.server.weapon);
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
        o[n++] = static_cast<float>(attackDamageAttribute(me.server.weapon)) / 10.0F;
        o[n++] = delay / 25.0F;
        o[n++] = static_cast<float>(attackDamageAttribute(op.server.weapon)) / 10.0F;
        o[n++] = attackStrengthDelay(op.server.weapon) / 25.0F;
        o[n++] = static_cast<float>(ticks[i]) / static_cast<float>(cfg.maxTicks);
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
                in[k] = applyFairness(cfg.caps, d.p[k].client.yRot, d.p[k].client.xRot, d.p[k].lastAttackHeld, act, fairness);
                if (recording[i] == 1) recorded[2 * i + k].push_back(in[k]);
            }
            float before[2] = {d.p[0].server.health, d.p[1].server.health};
            d.step(in[0], in[1], world, sinTab.data());
            bool dead = d.done();
            int32_t winner = d.winner();
            bool out[2] = {false, false};
            for (int32_t k = 0; k < 2; ++k) {
                const Player& c = d.p[k].client;
                out[k] = std::fabs(c.x) > cfg.arenaRadius || std::fabs(c.z) > cfg.arenaRadius;
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
            for (int32_t k = 0; k < 2; ++k) {
                EpisodeStats::Slot& s = st.s[k];
                s.clicks += in[k].attack ? 1.0F : 0.0F;
                s.attacks += d.p[k].sentAttack ? 1.0F : 0.0F;
                float dealt = before[1 - k] - d.p[1 - k].server.health;
                if (d.p[k].sentAttack && dealt > 0.0F) s.hits += 1.0F;
                s.damageDealt += dealt;
                s.damageTaken += before[k] - d.p[k].server.health;
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
                }
                if (recording[i] == 1) {
                    recordedDone[i] = dead ? 1 : 0;  // replays end at a death; an arena exit just stops
                    recordedNonParity[i] = d.nonParityEvents() + (d.unsupported() ? 1000 : 0);
                    recording[i] = 2;  // keep this episode; stop recording
                }
                resetDuel(i);
            }
        }
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
        for (int32_t j = 0; j < kActionSize; ++j) a[j] = 0.0F;
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
        float delay = attackStrengthDelay(me.server.weapon);
        float strength = mth::clamp((static_cast<float>(me.clientAttackStrengthTicker) + 0.5F) / delay, 0.0F, 1.0F);
        a[4] = (r.canHit() && strength >= 1.0F && m < 6.0F) ? 1.0F : 0.0F;
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
            std::snprintf(buf, sizeof buf, "start %s x=%.17g z=%.17g yaw=%.9g health=%.9g%s%s\n", k == 0 ? "A" : "B", s.x, s.z,
                          static_cast<double>(s.yaw), static_cast<double>(s.health), s.weapon == Weapon::Hand ? "" : " item=",
                          mcp::stats(s.weapon).name);
            out += buf;
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
                std::snprintf(buf, sizeof buf, " yaw=%.9g pitch=%.9g", static_cast<double>(in.yaw), static_cast<double>(in.pitch));
                out += buf;
            }
            out += "\n";
        }
        return out;
    }
};

}  // namespace mcp
