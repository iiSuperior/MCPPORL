// Reach geometry for observations and reward shaping.
//
// A click can hit only if the crosshair ray from the attacker's eye meets the
// target's hitbox within 3.0 blocks (LocalPlayer.pick; the entity must also be
// nearer than any block). The shortest such ray goes to the nearest point of
// the hitbox, so "the nearest point of the opponent's hitbox is within 3.0 of
// my eye" is exactly "a perfectly aimed click would land".
//
// Because reach is measured from the *eye* (1.62 above the feet) to the
// *nearest point* of a 1.8-tall box, height matters: a player standing one
// block lower has their eye level with the higher player's legs, so their
// reach line is horizontal, while the higher player must reach down to a head
// 0.82 below their eye along a longer diagonal (Technoblade's "low ground"
// argument). No special casing: it falls out of the geometry.
#pragma once

#include <cmath>

#include "mcp/pick.hpp"

namespace mcp::reach {

// Distance from an eye position to the nearest point of a box
// (sqrt of AABB.distanceToSqr, as AttackRange.isInRange measures it).
MCP_HD inline double eyeToBox(const Vec3& eye, const AABB& b) {
    double dx = ::fmax(::fmax(b.minX - eye.x, eye.x - b.maxX), 0.0);
    double dy = ::fmax(::fmax(b.minY - eye.y, eye.y - b.maxY), 0.0);
    double dz = ::fmax(::fmax(b.minZ - eye.z, eye.z - b.maxZ), 0.0);
    return ::sqrt(dx * dx + dy * dy + dz * dz);
}

// Within client pick range (Vec3.closerThan is strict).
MCP_HD inline bool inRange(double distance) { return distance < PickConstants::kEntityInteractionRange; }

struct ReachState {
    double mine = 0.0;    // my eye to the opponent's hitbox, as I see it
    double theirs = 0.0;  // their eye to my hitbox, as they see me
    MCP_HD bool canHit() const { return inRange(mine); }
    MCP_HD bool canBeHit() const { return inRange(theirs); }
    // +1: I can hit and they cannot; -1: the reverse; 0 otherwise.
    MCP_HD int advantage() const { return (canHit() ? 1 : 0) - (canBeHit() ? 1 : 0); }
    // How much further I reach than they do (positive is good), in blocks.
    MCP_HD double margin() const { return theirs - mine; }
};

}  // namespace mcp::reach
