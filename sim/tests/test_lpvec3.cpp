// Checks lpvec3::roundTrip against golden vectors produced by the game's own
// LpVec3 (oracle/probe/LpVec3Probe.java). Usage: test_lpvec3 <golden.txt>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "mcp/lpvec3.hpp"

using namespace mcp;

static double d(uint64_t u) {
    double v;
    std::memcpy(&v, &u, sizeof v);
    return v;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::FILE* f = std::fopen(argv[1], "r");
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    uint64_t in[3], out[3];
    long n = 0, bad = 0;
    while (std::fscanf(f, "%" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64 " %" SCNx64, &in[0], &in[1],
                       &in[2], &out[0], &out[1], &out[2]) == 6) {
        ++n;
        Vec3 got = lpvec3::roundTrip(Vec3{d(in[0]), d(in[1]), d(in[2])});
        uint64_t g[3] = {j::dbits(got.x), j::dbits(got.y), j::dbits(got.z)};
        if (g[0] != out[0] || g[1] != out[1] || g[2] != out[2]) {
            if (++bad <= 10)
                std::fprintf(stderr, "in (%g, %g, %g): got (%g, %g, %g), game (%g, %g, %g)\n", d(in[0]), d(in[1]),
                             d(in[2]), got.x, got.y, got.z, d(out[0]), d(out[1]), d(out[2]));
        }
    }
    std::fclose(f);
    std::printf("lpvec3: %ld vectors, %ld mismatches\n", n, bad);
    return n > 0 && bad == 0 ? 0 : 1;
}
