// BatchEnv (env.hpp): the same seed and actions give the same run, episodes
// end and restart, and a recorded episode exports as a scenario with one line
// per tick.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mcp/env.hpp"

using namespace mcp;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

struct Run {
    std::vector<float> obs, rew;
    int episodes = 0, deaths = 0;
};

Run play(const std::vector<float>& table, uint64_t seed, int steps, BatchEnv** keep = nullptr) {
    EnvConfig c;
    c.weaponMask = (1u << static_cast<uint32_t>(Weapon::DiamondSword)) | (1u << static_cast<uint32_t>(Weapon::IronAxe));
    BatchEnv* e = new BatchEnv(8, seed, table, c);
    e->record(3);
    const int n = e->size();
    std::vector<float> act(2 * n * kActionSize), obs(2 * n * kObsSize), rew(2 * n), st(n * kEpisodeStatsSize);
    std::vector<uint8_t> done(n);
    Run r;
    for (int t = 0; t < steps; ++t) {
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < 2; ++k) e->scripted(i, k, 1, 3.0F, 30.0F, act.data() + (2 * i + k) * kActionSize);
        e->step(act.data(), obs.data(), rew.data(), done.data(), st.data());
        for (int i = 0; i < n; ++i) {
            if (done[i]) r.episodes++;
            if (done[i] == 1 && st[i * kEpisodeStatsSize] >= 0.0F) r.deaths++;
        }
        r.obs.insert(r.obs.end(), obs.begin(), obs.end());
        r.rew.insert(r.rew.end(), rew.begin(), rew.end());
    }
    if (keep) *keep = e;
    else delete e;
    return r;
}
// Reward for wasted attacks and draws: a netherite sword against a shield
// that is never lowered costs wastedAttack per attack; the axe hit that
// disables the shield does not; a time-out costs both players `draw`.
void wastedAttacks(const std::vector<float>& table) {
    EnvConfig c;
    c.maxTicks = 300;
    c.minStartDist = c.maxStartDist = 2.5;
    c.reward = RewardWeights{0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.99F, 0.0F, 0.0F, 0.0F, 1.0F, 3.0F};
    BatchEnv e(1, 5, table, c);
    e.hasLoadout[0] = e.hasLoadout[1] = 1;
    e.loadouts[0].hotbar[0] = Weapon::NetheriteSword;
    e.loadouts[0].hotbar[1] = Weapon::WoodenAxe;
    e.loadouts[1].hotbar[0] = Weapon::DiamondSword;
    e.loadouts[1].offhand = Weapon::Shield;
    e.resetDuel(0);
    std::vector<float> act(2 * kActionSize), obs(2 * kObsSize), rew(2), st(kEpisodeStatsSize);
    uint8_t done = 0;
    int wasted = 0, penalised = 0, disables = 0, disableCharged = 0, useAxeFrom = 150;
    float drawCharge = 0.0F;
    for (int t = 0; t < 300 && !done; ++t) {
        e.scripted(0, 0, 1, 0.0F, 45.0F, act.data());
        if (t >= useAxeFrom) act[7] = 1.0F;  // hotbar key: the axe
        e.shielder(0, 1, 0.0F, 45.0F, 0.0F, 0.0F, 0.0F, act.data() + kActionSize);
        const Duel& d = e.duels[0];
        bool blocking = d.p[1].server.blocking();
        int32_t cd = d.p[1].server.shieldCooldown;
        float hp = d.p[1].server.health;
        e.step(act.data(), obs.data(), rew.data(), &done, st.data());
        bool sent = e.duels[0].p[0].sentAttack;
        bool disabled = e.duels[0].p[1].server.shieldCooldown > cd;
        if (disabled) {
            disables++;
            if (rew[0] != 0.0F) disableCharged++;
        } else if (sent && blocking && e.duels[0].p[1].server.health == hp) {
            wasted++;
            if (rew[0] == -1.0F) penalised++;
        }
    }
    check(wasted > 0 && penalised == wasted, "every sword hit into a raised shield costs wastedAttack");
    check(disables > 0 && disableCharged == 0, "the axe hit that disables the shield costs nothing");
    // Two standing players until the time limit.
    c.maxTicks = 40;
    BatchEnv idle(1, 6, table, c);
    for (int t = 0; t < 40; ++t) {
        idle.scripted(0, 0, 0, 0.0F, 45.0F, act.data());
        idle.scripted(0, 1, 0, 0.0F, 45.0F, act.data() + kActionSize);
        idle.step(act.data(), obs.data(), rew.data(), &done, st.data());
        if (done) {
            drawCharge = rew[0] + rew[1];
            break;
        }
    }
    check(done == 2 && drawCharge == -6.0F, "a time-out costs both players `draw`");
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::vector<float> table(mth::kSinTableSize);
    std::ifstream in(argv[1], std::ios::binary);
    for (auto& v : table) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), 4)) return 2;
        uint32_t u = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        std::memcpy(&v, &u, 4);
    }
    BatchEnv* e = nullptr;
    Run a = play(table, 42, 1500, &e);
    Run b = play(table, 42, 1500);
    check(a.obs == b.obs && a.rew == b.rew, "same seed, same run");
    check(a.episodes > 8 && a.deaths > 8, "episodes end in deaths and restart");
    for (float v : a.obs) check(v == v && v > -1e6F && v < 1e6F, "observations are finite");
    check(e->recording[3] == 2, "the recorded episode finished");
    std::string sc = e->scenario(3, "test");
    std::istringstream lines(sc);
    int ticks = 0, starts = 0;
    for (std::string l; std::getline(lines, l);) {
        if (l.rfind("start ", 0) == 0) starts++;
        else if (l.rfind("1 ", 0) == 0 && l.find(" | ") != std::string::npos) ticks++;
    }
    check(starts == 2 && ticks == static_cast<int>(e->recorded[6].size()) && ticks > 0, "one scenario line per tick");
    delete e;
    wastedAttacks(table);
    std::printf("env: %d episodes, %d by a death; %s\n", a.episodes, a.deaths, failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
