// Network latency model.
//
// Minecraft runs over TCP, so packets are delayed but never reordered or
// dropped. Jitter therefore shows up as ticks where the server processes no
// packet from a client, followed by ticks where it processes several.
//
// Each direction (client->server actions, server->client observations) gets
// its own DelayLine. Times are in milliseconds; the server consumes whatever
// has arrived by each tick boundary (tick * 50 ms).
#pragma once

#include <cstdint>

#include "mcp/jmath.hpp"
#include "mcp/rng.hpp"

namespace mcp {

constexpr int32_t kTickMs = 50;

// Per-episode latency draw. One-way delay = base_ms + uniform[0, jitter_ms].
struct LatencyProfile {
    int32_t base_ms = 0;
    int32_t jitter_ms = 0;

    MCP_HD static LatencyProfile sample(SplitMix64& rng, int32_t min_base_ms, int32_t max_base_ms,
                                        int32_t max_jitter_ms) {
        LatencyProfile p;
        p.base_ms = rng.range(min_base_ms, max_base_ms);
        p.jitter_ms = rng.range(0, max_jitter_ms);
        return p;
    }

    MCP_HD int32_t max_delay_ms() const { return base_ms + jitter_ms; }
};

// Fixed-capacity FIFO of in-flight packets. Capacity must cover the longest
// delay: Cap >= max_delay_ms / kTickMs + 2 when sending one packet per tick.
template <typename T, int Cap>
struct DelayLine {
    T items[Cap];
    int32_t arrive_ms[Cap];
    int32_t head = 0;
    int32_t size = 0;
    int32_t last_arrive_ms = INT32_MIN;

    MCP_HD void clear() {
        head = 0;
        size = 0;
        last_arrive_ms = INT32_MIN;
    }

    // Returns false if the line is full; callers treat that as a config error.
    MCP_HD bool push(const T& item, int32_t send_ms, const LatencyProfile& lat, SplitMix64& rng) {
        if (size == Cap) return false;
        int32_t jitter = lat.jitter_ms > 0 ? rng.range(0, lat.jitter_ms) : 0;
        int32_t t = send_ms + lat.base_ms + jitter;
        if (t < last_arrive_ms) t = last_arrive_ms;  // TCP: in-order delivery
        last_arrive_ms = t;
        int32_t slot = (head + size) % Cap;
        items[slot] = item;
        arrive_ms[slot] = t;
        ++size;
        return true;
    }

    // Pops the oldest packet if it has arrived by now_ms.
    MCP_HD bool pop(int32_t now_ms, T& out) {
        if (size == 0 || arrive_ms[head] > now_ms) return false;
        out = items[head];
        head = (head + 1) % Cap;
        --size;
        return true;
    }
};

}  // namespace mcp
