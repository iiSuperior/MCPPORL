#include <cstdio>

#include "mcp/latency.hpp"

using namespace mcp;

static int failures = 0;
#define CHECK(cond, msg)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            ++failures;                                      \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, msg); \
        }                                                    \
    } while (0)

// Fixed latency, no jitter: every packet arrives exactly base_ms later.
static void fixed_delay() {
    DelayLine<int, 16> line;
    SplitMix64 rng(1);
    LatencyProfile lat{120, 0};
    for (int t = 0; t < 10; ++t) CHECK(line.push(t, t * kTickMs, lat, rng), "push failed");
    int got;
    // 120 ms one way: sent at tick 0 (0 ms), arrives 120 ms, processed at tick 3 (150 ms).
    CHECK(!line.pop(2 * kTickMs, got), "arrived too early");
    CHECK(line.pop(3 * kTickMs, got) && got == 0, "tick 0 packet should be processed at tick 3");
    CHECK(!line.pop(3 * kTickMs, got), "tick 1 packet arrives at 170 ms");
    CHECK(line.pop(4 * kTickMs, got) && got == 1, "tick 1 packet at tick 4");
}

// With jitter: order is preserved, nothing is lost, delays stay in bounds.
static void jitter_preserves_order() {
    DelayLine<int, 64> line;
    SplitMix64 rng(42);
    LatencyProfile lat{80, 200};
    int next_expected = 0, sent = 0;
    int max_burst = 0;
    for (int tick = 0; tick < 2000; ++tick) {
        const int now = tick * kTickMs;
        CHECK(line.push(sent, now, lat, rng), "line overflowed");
        ++sent;
        int burst = 0, v;
        while (line.pop(now, v)) {
            CHECK(v == next_expected, "packet reordered or lost");
            const int delay = now - v * kTickMs;
            CHECK(delay >= lat.base_ms, "delivered faster than base latency");
            ++next_expected;
            ++burst;
        }
        if (burst > max_burst) max_burst = burst;
    }
    CHECK(max_burst >= 2, "jitter should produce multi-packet ticks");
}

static void profile_bounds() {
    SplitMix64 rng(7);
    for (int i = 0; i < 1000; ++i) {
        auto p = LatencyProfile::sample(rng, 20, 250, 60);
        CHECK(p.base_ms >= 20 && p.base_ms <= 250, "base out of range");
        CHECK(p.jitter_ms >= 0 && p.jitter_ms <= 60, "jitter out of range");
    }
}

int main() {
    fixed_delay();
    jitter_preserves_order();
    profile_bounds();
    std::printf("latency: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
