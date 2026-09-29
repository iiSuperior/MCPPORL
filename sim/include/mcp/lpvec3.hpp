// net.minecraft.network.LpVec3: the low-precision velocity encoding used by
// ClientboundSetEntityMotionPacket (knockback delivered to a player's client).
// We only need the lossy round trip: the velocity the receiving client applies.
#pragma once

#include <cstdint>

#include "mcp/jmath.hpp"
#include "mcp/vec.hpp"

namespace mcp::lpvec3 {

constexpr double kAbsMax = 1.7179869183E10;
constexpr double kAbsMin = 3.051944088384301E-5;

MCP_HD inline double sanitize(double v) {
    if (v != v) return 0.0;
    return v < -kAbsMax ? -kAbsMax : (v > kAbsMax ? kAbsMax : v);
}

// Java Math.round(double): nearest long, ties toward positive infinity.
MCP_HD inline int64_t javaRound(double x) {
    double f = ::floor(x);
    return j::d2l(x - f >= 0.5 ? f + 1.0 : f);
}

MCP_HD inline int64_t pack(double v) { return javaRound((v * 0.5 + 0.5) * 32766.0); }

MCP_HD inline double unpack(int64_t bits) {
    double m = static_cast<double>(bits & 32767LL);
    m = m < 32766.0 ? m : 32766.0;
    return m * 2.0 / 32766.0 - 1.0;
}

// What LpVec3.read(LpVec3.write(v)) returns.
MCP_HD inline Vec3 roundTrip(const Vec3& in) {
    double x = sanitize(in.x), y = sanitize(in.y), z = sanitize(in.z);
    double ax = ::fabs(x), ay = ::fabs(y), az = ::fabs(z);
    double m = ay > az ? ay : az;
    m = ax > m ? ax : m;  // Mth.absMax(x, Mth.absMax(y, z))
    if (m < kAbsMin) return Vec3{};
    int64_t scale = j::d2l(::ceil(m));  // Mth.ceilLong
    double s = static_cast<double>(scale);
    // Each component occupies 15 bits of the packed buffer, so pack() & 32767
    // is exactly what the reader sees.
    return Vec3{unpack(pack(x / s)) * s, unpack(pack(y / s)) * s, unpack(pack(z / s)) * s};
}

}  // namespace mcp::lpvec3
