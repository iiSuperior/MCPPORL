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
    std::printf("env: %d episodes, %d by a death; %s\n", a.episodes, a.deaths, failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
