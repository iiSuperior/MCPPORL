// Compares jmath.hpp against vectors produced by the JVM (oracle/jref/JRef.java).
// Usage: test_jmath <jref-out-dir>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "mcp/jmath.hpp"

using namespace mcp;

static int failures = 0;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            if (++failures <= 20) {              \
                std::fprintf(stderr, __VA_ARGS__); \
                std::fputc('\n', stderr);        \
            }                                    \
        }                                        \
    } while (0)

static double from_bits(uint64_t u) {
    double d;
    std::memcpy(&d, &u, sizeof d);
    return d;
}
static float from_bits32(uint32_t u) {
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

static std::vector<float> load_sin_table(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<float> table(mth::kSinTableSize);
    for (auto& v : table) {
        unsigned char b[4];
        if (!in.read(reinterpret_cast<char*>(b), 4)) {
            std::fprintf(stderr, "short read on %s\n", path.c_str());
            std::exit(2);
        }
        v = from_bits32((uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]);
    }
    return table;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <jref-out-dir>\n", argv[0]);
        return 2;
    }
    const std::string dir = argv[1];
    const auto table = load_sin_table(dir + "/sin_table.bin");

    // Informational: does this libm reproduce the JVM's table? The simulator
    // never relies on it (the table is loaded as data), but it's good to know.
    int libm_diffs = 0;
    for (int i = 0; i < mth::kSinTableSize; ++i) {
        float mine = static_cast<float>(std::sin(static_cast<double>(i) * 3.141592653589793 * 2.0 / 65536.0));
        if (j::fbits(mine) != j::fbits(table[i])) ++libm_diffs;
    }
    std::printf("libm vs JVM sin table: %d / %d entries differ (informational)\n", libm_diffs,
                mth::kSinTableSize);

    long n = 0;
    {
        std::FILE* f = std::fopen((dir + "/double_vectors.txt").c_str(), "r");
        if (!f) return 2;
        uint64_t bits;
        int32_t ei, efl, ece;
        int64_t el;
        uint32_t esin, ecos;
        while (std::fscanf(f, "%" SCNx64 " %" SCNd32 " %" SCNd64 " %" SCNd32 " %" SCNd32 " %" SCNx32 " %" SCNx32,
                           &bits, &ei, &el, &efl, &ece, &esin, &ecos) == 7) {
            const double d = from_bits(bits);
            ++n;
            CHECK(j::d2i(d) == ei, "d2i(%016" PRIx64 ") = %d, java %d", bits, j::d2i(d), ei);
            CHECK(j::d2l(d) == el, "d2l(%016" PRIx64 ") = %" PRId64 ", java %" PRId64, bits, j::d2l(d), el);
            CHECK(mth::floor(d) == efl, "floor(%016" PRIx64 ") = %d, java %d", bits, mth::floor(d), efl);
            CHECK(mth::ceil(d) == ece, "ceil(%016" PRIx64 ") = %d, java %d", bits, mth::ceil(d), ece);
            CHECK(j::fbits(mth::sin(table.data(), d)) == esin, "sin(%016" PRIx64 ") mismatch", bits);
            CHECK(j::fbits(mth::cos(table.data(), d)) == ecos, "cos(%016" PRIx64 ") mismatch", bits);
        }
        std::fclose(f);
    }
    {
        std::FILE* f = std::fopen((dir + "/float_vectors.txt").c_str(), "r");
        if (!f) return 2;
        uint32_t bits;
        int32_t ei;
        int64_t el;
        while (std::fscanf(f, "%" SCNx32 " %" SCNd32 " %" SCNd64, &bits, &ei, &el) == 3) {
            const float x = from_bits32(bits);
            ++n;
            CHECK(j::f2i(x) == ei, "f2i(%08" PRIx32 ") = %d, java %d", bits, j::f2i(x), ei);
            CHECK(j::f2l(x) == el, "f2l(%08" PRIx32 ") mismatch", bits);
        }
        std::fclose(f);
    }

    std::printf("%ld vectors checked, %d failures\n", n, failures);
    return failures == 0 && n > 0 ? 0 : 1;
}
