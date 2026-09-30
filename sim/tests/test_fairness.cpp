// Unit tests for the fairness gate (mcp/fairness.hpp).
#include <cmath>
#include <cstdio>

#include "mcp/fairness.hpp"

using namespace mcp;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    FairnessCaps caps;
    FairnessStats st;
    AgentAction a;

    // The 180 hit fits in one tick.
    a.yaw = 180.0F;
    DuelInput in = applyFairness(caps, 0.0F, 0.0F, false, a, st);
    check(in.yaw == 180.0F || in.yaw == -180.0F, "180 degree flick allowed");
    check(st.turnsClamped == 0, "180 flick not clamped");

    // Turning the short way round across the +-180 seam.
    a.yaw = -170.0F;
    in = applyFairness(caps, 170.0F, 0.0F, false, a, st);
    check(std::fabs(in.yaw - 190.0F) < 1e-4F, "wraps across the seam (170 -> 190)");

    // A combined turn beyond the cap is scaled down to it.
    a.yaw = 180.0F;  // 180 yaw + 90 pitch = 201 degrees: scaled down to 200
    a.pitch = 100.0F;  // also beyond the pitch limit: clamped to 90 first
    in = applyFairness(caps, 0.0F, 0.0F, false, a, st);
    float dy = duel::wrapDegrees(in.yaw), dp = in.pitch;
    float total = std::sqrt(dy * dy + dp * dp);
    check(std::fabs(total - 200.0F) < 1e-3F, "turn scaled to the cap");
    check(st.turnsClamped == 1, "clamp counted");
    check(in.pitch <= 90.0F, "pitch within limits");

    // Holding the attack key needs a press now or a hold last tick.
    a = AgentAction{};
    a.holdAttack = true;
    in = applyFairness(caps, 0.0F, 0.0F, false, a, st);
    check(!in.attackHeld && st.holdsRejected == 1, "hold without a press is rejected");
    in = applyFairness(caps, 0.0F, 0.0F, true, a, st);
    check(in.attackHeld, "continued hold is allowed");
    a.click = true;
    in = applyFairness(caps, 0.0F, 0.0F, false, a, st);
    check(in.attack && in.attackHeld, "press and hold");

    std::printf("%s\n", failures == 0 ? "fairness gate: all checks passed" : "fairness gate: FAILED");
    return failures == 0 ? 0 : 1;
}
