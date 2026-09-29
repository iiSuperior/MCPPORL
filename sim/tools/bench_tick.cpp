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
    // FNV-1a over the exact bits of every player's state: identical hashes on
    // two machines mean the simulations agreed bit for bit.
    uint64_t hash = 1469598103934665603ULL;
    auto mix = [&hash](uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            hash ^= (v >> (8 * i)) & 0xFF;
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& p : ps) {
        mix(j::dbits(p.x)); mix(j::dbits(p.y)); mix(j::dbits(p.z));
        mix(j::dbits(p.vel.x)); mix(j::dbits(p.vel.y)); mix(j::dbits(p.vel.z));
        mix(j::fbits(p.yRot)); mix(p.onGround); mix(p.sprinting);
    }
    std::printf("%.2f M player-ticks/s on one thread (%d players x %d ticks, %.2f s)\n",
                players * double(ticks) / dt / 1e6, players, ticks, dt);
    std::printf("state hash: %016llx\n", static_cast<unsigned long long>(hash));
    return 0;
}
