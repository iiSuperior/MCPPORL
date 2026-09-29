// Single-threaded duel throughput: client ticks, packets, server copies,
// attacks and knockback for many independent 1v1 duels on flat ground.
// Usage: bench_duel <sin_table.bin> [duels] [ticks]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "mcp/duel.hpp"

using namespace mcp;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <sin_table.bin> [duels] [ticks]\n", argv[0]);
        return 2;
    }
    std::vector<float> sinTab(mth::kSinTableSize);
    std::ifstream in(argv[1], std::ios::binary);
    for (auto& v : sinTab) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), 4)) return 2;
        uint32_t u = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
        std::memcpy(&v, &u, 4);
    }
    const int duels = argc > 2 ? std::atoi(argv[2]) : 2048;
    const int ticks = argc > 3 ? std::atoi(argv[3]) : 2000;

    FlatWorld world;
    std::vector<Duel> ds(duels);
    for (int i = 0; i < duels; ++i) {
        double x = i * 16.0 + 0.5;
        ds[i].spawn(0, x, -60.0, 0.5, 0.0F, world);
        ds[i].spawn(1, x, -60.0, 3.5, 180.0F, world);
    }
    uint64_t s = 12345;
    auto rnd = [&s]() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return s;
    };
    int64_t hits = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < ticks; ++t) {
        for (Duel& d : ds) {
            DuelInput in[2];
            for (int k = 0; k < 2; ++k) {
                uint64_t r = rnd();
                // Face the opponent (yaw 0 is +z), then random keys and clicks.
                const Player& me = d.p[k].client;
                const Player& op = d.p[1 - k].client;
                in[k].yaw = op.z > me.z ? 0.0F : 180.0F;
                in[k].keys.forward = (r >> 63) != 0 || (r >> 60 & 3) == 0;
                in[k].keys.backward = !in[k].keys.forward && (r >> 59 & 7) == 0;
                in[k].keys.left = (r >> 56 & 7) == 0;
                in[k].keys.right = !in[k].keys.left && (r >> 53 & 7) == 0;
                in[k].keys.sprint = (r >> 52 & 1) != 0;
                in[k].keys.jump = (r >> 47 & 31) == 0;
                in[k].attack = (r >> 44 & 7) == 0;
            }
            d.step(in[0], in[1], world, sinTab.data());
            hits += d.p[0].replies.motion + d.p[1].replies.motion;
            d.deliver();
        }
    }
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    int unsupported = 0;
    for (const Duel& d : ds) unsupported += d.unsupported();
    std::printf("%.2f M duel-ticks/s (%.2f M player-ticks/s) on one thread (%d duels x %d ticks, %.2f s)\n",
                duels * double(ticks) / dt / 1e6, 2.0 * duels * double(ticks) / dt / 1e6, duels, ticks, dt);
    std::printf("%lld knockback hits, %d duel(s) left the supported domain\n", static_cast<long long>(hits), unsupported);
    return 0;
}
