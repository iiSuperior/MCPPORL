// Checks the crosshair pick port (mcp/pick.hpp) against picks resolved by
// vanilla 26.3 in the oracle (CombatOracle.pickProbe): hit type and hit
// location must match bit for bit.
// Usage: test_pick <sin_table.bin> <pick_golden.txt>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "mcp/pick.hpp"
#include "mcp/player.hpp"

using namespace mcp;

namespace {

// The flat arena plus the stone blocks the probe placed (listed in the header).
struct ProbeWorld {
    FlatWorld flat;
    std::vector<BlockPos> extra;
    bool solid(int32_t x, int32_t y, int32_t z) const {
        for (const BlockPos& b : extra)
            if (b.x == x && b.y == y && b.z == z) return true;
        return flat.solid(x, y, z);
    }
};

double d(const std::string& h) {
    uint64_t u = std::strtoull(h.c_str(), nullptr, 16);
    double v;
    std::memcpy(&v, &u, 8);
    return v;
}

float f(const std::string& h) {
    uint32_t u = static_cast<uint32_t>(std::strtoul(h.c_str(), nullptr, 16));
    float v;
    std::memcpy(&v, &u, 4);
    return v;
}

AABB playerBox(double x, double y, double z) {
    Player p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p.boundingBox();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <sin_table.bin> <pick_golden.txt>\n", argv[0]);
        return 2;
    }
    std::vector<float> sinTab(mth::kSinTableSize);
    {
        std::ifstream in(argv[1], std::ios::binary);
        for (auto& v : sinTab) {
            unsigned char b[4];
            if (!in.read(reinterpret_cast<char*>(b), 4)) return 2;
            uint32_t u = (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
            std::memcpy(&v, &u, 4);
        }
    }
    std::ifstream in(argv[2]);
    if (!in) return 2;
    ProbeWorld world;
    std::string line;
    int cases = 0, bad = 0, counts[3] = {0, 0, 0};
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '#') {
            // "... plus stone at x,y,z x,y,z ..."
            auto at = line.find("stone at");
            if (at != std::string::npos) {
                std::istringstream ss(line.substr(at + 8));
                for (std::string t; ss >> t;) {
                    BlockPos b;
                    if (std::sscanf(t.c_str(), "%d,%d,%d", &b.x, &b.y, &b.z) == 3) world.extra.push_back(b);
                }
            }
            continue;
        }
        std::istringstream ss(line);
        std::vector<std::string> t;
        for (std::string w; ss >> w;) t.push_back(w);
        if (t.size() != 15) {
            std::fprintf(stderr, "bad line: %s\n", line.c_str());
            return 2;
        }
        PickView v{d(t[0]), d(t[1]), d(t[2]), d(t[3]), d(t[4]), d(t[5]), 1.62F, f(t[7]), f(t[6]),
                   playerBox(d(t[3]), d(t[4]), d(t[5]))};
        AABB target = playerBox(d(t[8]), d(t[9]), d(t[10]));
        HitResult r = clientPick(v, target, world, sinTab.data());
        int type = std::atoi(t[11].c_str());
        double hx = d(t[12]), hy = d(t[13]), hz = d(t[14]);
        cases++;
        if (type >= 0 && type < 3) counts[type]++;
        if (static_cast<int>(r.type) != type || j::dbits(r.location.x) != j::dbits(hx) ||
            j::dbits(r.location.y) != j::dbits(hy) || j::dbits(r.location.z) != j::dbits(hz)) {
            if (bad < 10)
                std::fprintf(stderr, "case %d: vanilla %d (%.17g, %.17g, %.17g), sim %d (%.17g, %.17g, %.17g)\n", cases,
                             type, hx, hy, hz, static_cast<int>(r.type), r.location.x, r.location.y, r.location.z);
            bad++;
        }
    }
    std::printf("%d picks (miss/block/entity %d/%d/%d), %d mismatches\n", cases, counts[0], counts[1], counts[2], bad);
    return bad == 0 && cases > 0 ? 0 : 1;
}
