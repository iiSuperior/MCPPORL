// Single-threaded player tick throughput on the CPU reference backend.
// Usage: bench_tick <sin_table.bin> [players] [ticks]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

#include "mcp/player.hpp"

using namespace mcp;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <sin_table.bin> [players] [ticks]\n", argv[0]);
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
    const int players = argc > 2 ? std::atoi(argv[2]) : 4096;
    const int ticks = argc > 3 ? std::atoi(argv[3]) : 2000;

    std::vector<Player> ps(players);
    for (int i = 0; i < players; ++i) {
        ps[i].x = i * 3.0 + 0.5;
        ps[i].y = -60.0;
        ps[i].z = 0.5;
    }
    FlatWorld world;
    uint64_t s = 12345;
    auto t0 = std::chrono::steady_clock::now();
    for (int t = 0; t < ticks; ++t) {
        for (auto& p : ps) {
            s = s * 6364136223846793005ULL + 1442695040888963407ULL;  // random but reproducible inputs
            Keys k;
            k.forward = (s >> 63) != 0;
            k.left = ((s >> 62) & 1) != 0;
            k.sprint = ((s >> 61) & 1) != 0;
            k.jump = ((s >> 57) & 15) == 0;
            tick(p, k, static_cast<float>((s >> 40) % 360), 0.0F, world, sinTab.data());
        }
    }
    double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    double checksum = 0.0;
    for (const auto& p : ps) checksum += p.x + p.z;
    std::printf("%.2f M player-ticks/s on one thread (%d players x %d ticks, %.2f s, checksum %.6g)\n",
                players * double(ticks) / dt / 1e6, players, ticks, dt, checksum);
    return 0;
}
