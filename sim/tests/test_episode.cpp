// Episode lifecycle: a fight runs to a death, the duel reports it, and reset()
// restores exactly the state of a freshly created duel.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "mcp/fairness.hpp"

using namespace mcp;

namespace {

uint64_t hashDuel(const Duel& d) {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    for (const DuelPlayer& p : d.p) {
        const Player* bodies[2] = {&p.client, &p.server.body};
        for (const Player* b : bodies) {
            mix(j::dbits(b->x)); mix(j::dbits(b->y)); mix(j::dbits(b->z));
            mix(j::dbits(b->xo)); mix(j::dbits(b->yo)); mix(j::dbits(b->zo));
            mix(j::dbits(b->vel.x)); mix(j::dbits(b->vel.y)); mix(j::dbits(b->vel.z));
            mix(j::fbits(b->yRot)); mix(j::fbits(b->xRot));
            mix(b->onGround); mix(b->sprinting); mix(b->sprintModifier); mix(static_cast<uint64_t>(b->noJumpDelay));
        }
        const ServerCopy& s = p.server;
        mix(j::dbits(s.fallDistance)); mix(j::fbits(s.health)); mix(j::fbits(s.lastHurt));
        mix(static_cast<uint64_t>(s.hurtTime)); mix(static_cast<uint64_t>(s.damageCooldownTime));
        mix(static_cast<uint64_t>(s.attackStrengthTicker)); mix(s.dead);
        mix(static_cast<uint64_t>(p.missTime)); mix(static_cast<uint64_t>(p.clientAttackStrengthTicker));
        mix(p.isDestroying); mix(p.lastAttackHeld); mix(static_cast<uint64_t>(p.send.positionReminder));
        mix(p.send.wasSprinting);
    }
    return h;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<float> sinTab(mth::kSinTableSize);
    std::ifstream in(argv[1], std::ios::binary);
    for (auto& v : sinTab) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), 4)) return 2;
        uint32_t u = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        std::memcpy(&v, &u, 4);
    }
    FlatWorld world;
    DuelStart sa{0.5, 0.5, 0.0F, 6.0F}, sb{0.5, 3.5, 180.0F, 6.0F};
    Duel fresh;
    fresh.reset(sa, sb, world);
    const uint64_t freshHash = hashDuel(fresh);

    Duel d;
    d.reset(sa, sb, world);
    FairnessCaps caps;
    FairnessStats stats;
    uint64_t s = 7;
    int failures = 0, randomKnockbacks = 0;
    for (int episode = 0; episode < 20; ++episode) {
        int t = 0;
        while (!d.done() && t < 20000) {
            AgentAction act[2];
            for (int k = 0; k < 2; ++k) {
                s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                const Player& me = d.p[k].client;
                const Player& op = d.p[1 - k].client;
                // Aim at the opponent's chest (yaw 0 is +z, positive pitch looks down).
                double dx = op.x - me.x, dz = op.z - me.z, dy = (op.y + 1.0) - (me.y + 1.62);
                act[k].yaw = static_cast<float>(std::atan2(-dx, dz) * 180.0 / 3.141592653589793);
                act[k].pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0 / 3.141592653589793);
                act[k].keys.forward = (s >> 63) != 0;
                act[k].keys.sprint = (s >> 62 & 1) != 0;
                act[k].keys.jump = (s >> 55 & 15) == 0;
                act[k].click = (s >> 50 & 3) == 0;
            }
            stepFair(d, caps, act[0], act[1], world, sinTab.data(), stats);
            if (d.unsupported()) {
                std::fprintf(stderr, "episode %d left the supported domain at tick %d\n", episode, t);
                return 1;
            }
            if (!d.done()) d.deliver();
            t++;
        }
        if (!d.done() || d.winner() < 0) {
            std::fprintf(stderr, "episode %d: no single winner after %d ticks\n", episode, t);
            failures++;
        }
        randomKnockbacks += d.nonParityEvents();
        d.reset(sa, sb, world);
        if (hashDuel(d) != freshHash) {
            std::fprintf(stderr, "episode %d: reset state differs from a fresh duel\n", episode);
            failures++;
        }
    }
    std::printf("%s (%d random-direction knockbacks)\n",
                failures == 0 ? "episodes: 20 fights to a death, clean resets" : "episodes: FAILED", randomKnockbacks);
    return failures == 0 ? 0 : 1;
}
