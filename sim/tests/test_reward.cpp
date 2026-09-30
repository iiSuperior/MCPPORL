// Duel rewards (reward.hpp): damage and win terms follow the server's health,
// and the reach term is potential-based shaping, so over a whole episode it
// sums to -phi(start) whatever the players do, and idling in range earns
// nothing.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

#include "mcp/fairness.hpp"
#include "mcp/reward.hpp"

using namespace mcp;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
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

    // 1. A standing hit: +1 for A, -1 for B; both stay in each other's range
    //    (mutual reach: potential 0), so shaping adds nothing.
    {
        Duel d;
        d.reset(DuelStart{0.5, 0.5, 0.0F}, DuelStart{0.5, 3.0, 180.0F}, world);
        DuelRewards rw;
        rw.reset(d);
        RewardWeights w;
        reach::ReachState r0 = reachOf(d, 0);
        check(r0.canHit() && r0.canBeHit(), "players 2.5 apart reach each other");
        DuelInput a{}, b{};
        a.yaw = 0.0F;
        a.pitch = 20.0F;
        b.yaw = 180.0F;
        float sum[2] = {0.0F, 0.0F}, out[2];
        for (int t = 0; t < 21; ++t) {
            a.attack = a.attackHeld = t == 20;  // after the cooldown refills
            d.step(a, b, world, sinTab.data());
            d.deliver();
            rw.step(d, w, out);
            sum[0] += out[0];
            sum[1] += out[1];
        }
        check(d.p[1].server.health == 19.0F, "the hit landed");
        check(sum[0] == 1.0F && sum[1] == -1.0F, "damage reward is the health exchanged");
    }

    // 2. Telescoping: with gamma = 1 the shaping over an episode is -phi(start),
    //    for any behaviour. Random aimed bots fight to the end.
    {
        RewardWeights w;
        w.damageDealt = w.damageTaken = w.win = w.loss = 0.0F;
        w.reachShaping = 1.0F;
        w.gamma = 1.0F;
        FairnessCaps caps;
        FairnessStats stats;
        uint64_t s = 11;
        int episodes = 0;
        for (int ep = 0; ep < 10; ++ep) {
            Duel d;
            d.reset(DuelStart{0.5, 0.5, 0.0F, 4.0F}, DuelStart{0.5, 6.5, 180.0F, 4.0F}, world, static_cast<uint64_t>(ep));
            DuelRewards rw;
            rw.reset(d);
            float phi0[2] = {rw.phi[0], rw.phi[1]};
            double sum[2] = {0.0, 0.0};
            float out[2];
            for (int t = 0; t < 4000 && !d.done(); ++t) {
                AgentAction act[2];
                for (int k = 0; k < 2; ++k) {
                    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
                    const Player& me = d.p[k].client;
                    const RemoteView& op = d.p[k].view;
                    double dx = op.pos.x - me.x, dz = op.pos.z - me.z, dy = (op.pos.y + 1.0) - (me.y + 1.62);
                    act[k].yaw = static_cast<float>(std::atan2(-dx, dz) * 180.0 / 3.141592653589793) +
                                 static_cast<float>(static_cast<int>(s >> 40 & 31) - 16);
                    act[k].pitch = static_cast<float>(-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.0 / 3.141592653589793);
                    act[k].keys.forward = (s >> 63) != 0;
                    act[k].keys.backward = !act[k].keys.forward && (s >> 61 & 3) == 0;
                    act[k].keys.left = (s >> 58 & 3) == 0;
                    act[k].keys.sprint = (s >> 57 & 1) != 0;
                    act[k].keys.jump = (s >> 52 & 15) == 0;
                    act[k].click = (s >> 48 & 3) == 0;
                    act[k].holdAttack = act[k].click;
                }
                stepFair(d, caps, act[0], act[1], world, sinTab.data(), stats);
                if (!d.done()) d.deliver();
                rw.step(d, w, out);
                sum[0] += out[0];
                sum[1] += out[1];
            }
            check(!d.unsupported(), "the random fight stayed in the known domain");
            if (d.done()) episodes++;
            else continue;
            check(sum[0] == -static_cast<double>(phi0[0]) && sum[1] == -static_cast<double>(phi0[1]),
                  "shaping telescopes to -phi(start)");
        }
        check(episodes >= 5, "most random fights end in a death");
        std::printf("telescoping checked on %d finished episodes\n", episodes);
    }

    // 3. No farming: idling in range with gamma < 1 earns nothing when the
    //    reach is mutual, and (gamma - 1) * phi <= 0 in general.
    {
        Duel d;
        d.reset(DuelStart{0.5, 0.5, 0.0F}, DuelStart{0.5, 3.0, 180.0F}, world);
        DuelRewards rw;
        rw.reset(d);
        RewardWeights w;
        DuelInput a{}, b{};
        b.yaw = 180.0F;
        float out[2];
        bool zero = true;
        for (int t = 0; t < 100; ++t) {
            d.step(a, b, world, sinTab.data());
            d.deliver();
            rw.step(d, w, out);
            zero = zero && out[0] == 0.0F && out[1] == 0.0F;
        }
        check(zero, "idling in mutual range is worth nothing");
        check(w.gamma * 1.0F - 1.0F < 0.0F, "holding a reach advantage costs a little per tick, it is not paid");
    }

    if (failures == 0) std::printf("reward: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
