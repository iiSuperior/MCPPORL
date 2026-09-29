// Deterministic, GPU-friendly RNG for simulator-side randomness that is NOT
// vanilla game randomness (latency, arena generation, scenario sampling).
// Vanilla randomness gets its own exact port in the domain that needs it.
#pragma once

#include <cstdint>

#include "mcp/jmath.hpp"

namespace mcp {

struct SplitMix64 {
    uint64_t state;

    MCP_HD explicit SplitMix64(uint64_t seed) : state(seed) {}

    MCP_HD uint64_t next() {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform integer in [lo, hi]. Slight modulo bias is fine for this use.
    MCP_HD int32_t range(int32_t lo, int32_t hi) {
        uint64_t span = static_cast<uint64_t>(static_cast<int64_t>(hi) - lo + 1);
        return static_cast<int32_t>(lo + static_cast<int64_t>(next() % span));
    }
};

}  // namespace mcp
