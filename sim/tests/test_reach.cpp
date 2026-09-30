// Reach geometry, including the low-ground advantage.
#include <cstdio>

#include "mcp/player.hpp"
#include "mcp/reach.hpp"

using namespace mcp;

static int failures = 0;
static void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

static AABB box(double x, double y, double z) {
    Player p;
    p.x = x;
    p.y = y;
    p.z = z;
    return p.boundingBox();
}

int main() {
    const double e = static_cast<double>(1.62F);
    // Level ground, 3.2 apart (centre to centre): both reach 2.9, both in range.
    {
        Vec3 aEye{0.0, 0.0 + e, 0.0}, bEye{0.0, 0.0 + e, 3.2};
        reach::ReachState r{reach::eyeToBox(aEye, box(0.0, 0.0, 3.2)), reach::eyeToBox(bEye, box(0.0, 0.0, 0.0))};
        check(r.canHit() && r.canBeHit() && r.advantage() == 0, "level ground at 3.2: mutual");
    }
    // A one block lower than B, same 3.2 apart: A's eye is level with B's legs
    // (horizontal reach 2.9); B reaches down 0.82 to A's head: sqrt(2.9^2 + 0.82^2) > 3.
    {
        Vec3 aEye{0.0, 0.0 + e, 0.0}, bEye{0.0, 1.0 + e, 3.2};
        reach::ReachState r{reach::eyeToBox(aEye, box(0.0, 1.0, 3.2)), reach::eyeToBox(bEye, box(0.0, 0.0, 0.0))};
        std::printf("low ground: A reaches %.4f, B reaches %.4f\n", r.mine, r.theirs);
        check(r.canHit() && !r.canBeHit() && r.advantage() == 1, "low ground: A hits, B cannot");
        check(r.margin() > 0.1, "low ground margin");
    }
    // Two blocks lower: A must now reach up to B's feet, but B reaches down much further.
    {
        Vec3 aEye{0.0, 0.0 + e, 0.0}, bEye{0.0, 2.0 + e, 3.2};
        reach::ReachState r{reach::eyeToBox(aEye, box(0.0, 2.0, 3.2)), reach::eyeToBox(bEye, box(0.0, 0.0, 0.0))};
        std::printf("two blocks lower: A reaches %.4f, B reaches %.4f\n", r.mine, r.theirs);
        check(r.mine > 2.9 && r.margin() > 0.4, "two blocks lower: A reaches up, B reaches down further");
    }
    std::printf("%s\n", failures == 0 ? "reach: all checks passed" : "reach: FAILED");
    return failures == 0 ? 0 : 1;
}
