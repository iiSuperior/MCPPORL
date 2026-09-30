// Block world and collision, ported from Entity.collide/collideWithShapes,
// Shapes.collide and VoxelShape.collideX, restricted to full-cube blocks.
#pragma once

#include <cstdint>

#include "mcp/jmath.hpp"
#include "mcp/vec.hpp"

namespace mcp {

enum class Block : uint8_t { Air, Grass, Dirt, Bedrock, Barrier };

// Block properties used by movement (Block.getFriction/getSpeedFactor/getJumpFactor).
MCP_HD inline float blockFrictionOf(Block) { return 0.6F; }
MCP_HD inline float blockSpeedFactorOf(Block) { return 1.0F; }
MCP_HD inline float blockJumpFactorOf(Block) { return 1.0F; }

// Superflat default: bedrock, dirt, dirt, grass; the walkable surface is at surfaceY.
struct FlatWorld {
    int32_t surfaceY = -60;

    MCP_HD Block get(int32_t, int32_t y, int32_t) const {
        if (y >= surfaceY) return Block::Air;
        if (y == surfaceY - 1) return Block::Grass;
        if (y >= surfaceY - 3) return Block::Dirt;
        if (y == surfaceY - 4) return Block::Bedrock;
        return Block::Air;  // below bedrock: the void
    }
    MCP_HD bool solid(int32_t x, int32_t y, int32_t z) const { return get(x, y, z) != Block::Air; }
};

// The superflat floor walled in by barrier blocks: a ring `wallHeight` high
// (more than a jump) on the columns x or z = -radius-1 and radius, so a
// player's feet stay within [-radius, radius). Barriers are invisible to
// human players and full cubes to collision; the oracle builds the same ring
// (CombatOracle.forceArena).
//
// When block placing arrives (Phase 3: buckets, cobwebs, blocks), the ring
// must stay the arena's edge: no placement on or above a wall column, no
// breaking it, and no building up past its top (docs/ARCHITECTURE.md,
// "Walled arena"). Raise wallHeight to the build limit or add a height
// check then, or bots will learn to climb out over their own blocks.
struct ArenaWorld {
    int32_t surfaceY = -60;
    int32_t radius = 24;
    int32_t wallHeight = 4;

    MCP_HD bool wall(int32_t x, int32_t y, int32_t z) const {
        if (y < surfaceY || y >= surfaceY + wallHeight) return false;
        bool inX = x >= -radius - 1 && x <= radius, inZ = z >= -radius - 1 && z <= radius;
        return (inZ && (x == -radius - 1 || x == radius)) || (inX && (z == -radius - 1 || z == radius));
    }
    MCP_HD Block get(int32_t x, int32_t y, int32_t z) const {
        if (wall(x, y, z)) return Block::Barrier;
        return FlatWorld{surfaceY}.get(x, y, z);
    }
    MCP_HD bool solid(int32_t x, int32_t y, int32_t z) const { return get(x, y, z) != Block::Air; }
};

struct BlockPos {
    int32_t x, y, z;
};

// Full-cube colliders overlapping a box (Level.getBlockCollisions for full blocks).
template <int Cap>
struct Colliders {
    BlockPos pos[Cap];
    int32_t count = 0;
    bool overflow = false;

    MCP_HD bool empty() const { return count == 0; }
};

template <typename World, int Cap>
MCP_HD void collectBlockCollisions(const World& w, const AABB& box, Colliders<Cap>& out) {
    out.count = 0;
    out.overflow = false;
    int32_t x0 = mth::floor(box.minX - 1.0E-7) - 1, x1 = mth::floor(box.maxX + 1.0E-7) + 1;
    int32_t y0 = mth::floor(box.minY - 1.0E-7) - 1, y1 = mth::floor(box.maxY + 1.0E-7) + 1;
    int32_t z0 = mth::floor(box.minZ - 1.0E-7) - 1, z1 = mth::floor(box.maxZ + 1.0E-7) + 1;
    for (int32_t x = x0; x <= x1; ++x)
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t z = z0; z <= z1; ++z) {
                if (!w.solid(x, y, z)) continue;
                if (!box.intersects(x, y, z, x + 1.0, y + 1.0, z + 1.0)) continue;
                if (out.count == Cap) {
                    out.overflow = true;
                    return;
                }
                out.pos[out.count++] = BlockPos{x, y, z};
            }
}

// VoxelShape.findIndex for a unit cube spanning [lo, lo + 1] on one axis.
MCP_HD inline int32_t cubeFindIndex(double lo, double value) {
    if (value < lo) return -1;
    if (value < lo + 1.0) return 0;
    return 1;
}

MCP_HD inline double cubeLo(const BlockPos& p, Axis a) {
    return static_cast<double>(a == Axis::X ? p.x : (a == Axis::Y ? p.y : p.z));
}

// VoxelShape.collideX specialised to one full cube.
MCP_HD inline double collideCube(Axis a, const BlockPos& cube, const AABB& moving, double distance) {
    if (::fabs(distance) < 1.0E-7) return 0.0;
    Axis b = a == Axis::X ? Axis::Y : (a == Axis::Y ? Axis::Z : Axis::X);
    Axis c = a == Axis::X ? Axis::Z : (a == Axis::Y ? Axis::X : Axis::Y);
    double loA = cubeLo(cube, a), loB = cubeLo(cube, b), loC = cubeLo(cube, c);
    double maxA = moving.max(a), minA = moving.min(a);
    int32_t aMin = cubeFindIndex(loA, minA + 1.0E-7);
    int32_t aMax = cubeFindIndex(loA, maxA - 1.0E-7);
    int32_t bMin = cubeFindIndex(loB, moving.min(b) + 1.0E-7); bMin = bMin < 0 ? 0 : bMin;
    int32_t bMax = cubeFindIndex(loB, moving.max(b) - 1.0E-7) + 1; bMax = bMax > 1 ? 1 : bMax;
    int32_t cMin = cubeFindIndex(loC, moving.min(c) + 1.0E-7); cMin = cMin < 0 ? 0 : cMin;
    int32_t cMax = cubeFindIndex(loC, moving.max(c) - 1.0E-7) + 1; cMax = cMax > 1 ? 1 : cMax;
    bool overlaps = bMin < bMax && cMin < cMax;
    if (distance > 0.0) {
        if (aMax + 1 < 1 && overlaps) {  // cell a = 0 lies ahead
            double newDistance = (loA + 0.0) - maxA;
            if (newDistance >= -1.0E-7) distance = distance < newDistance ? distance : newDistance;
            return distance;
        }
    } else if (distance < 0.0) {
        if (aMin - 1 >= 0 && overlaps) {  // cell a = 0 lies behind
            double newDistance = (loA + 1.0) - minA;
            if (newDistance <= 1.0E-7) distance = distance > newDistance ? distance : newDistance;
            return distance;
        }
    }
    return distance;
}

// Shapes.collide
template <int Cap>
MCP_HD double collideShapes(Axis a, const AABB& moving, const Colliders<Cap>& shapes, double distance) {
    for (int32_t i = 0; i < shapes.count; ++i) {
        if (::fabs(distance) < 1.0E-7) return 0.0;
        distance = collideCube(a, shapes.pos[i], moving, distance);
    }
    return distance;
}

// Direction.axisStepOrder
MCP_HD inline void axisStepOrder(const Vec3& m, Axis out[3]) {
    out[0] = Axis::Y;
    if (::fabs(m.x) < ::fabs(m.z)) {
        out[1] = Axis::Z;
        out[2] = Axis::X;
    } else {
        out[1] = Axis::X;
        out[2] = Axis::Z;
    }
}

// Entity.collideWithShapes
template <int Cap>
MCP_HD Vec3 collideWithShapes(const Vec3& movement, const AABB& box, const Colliders<Cap>& shapes) {
    if (shapes.empty()) return movement;
    Vec3 resolved{};
    Axis order[3];
    axisStepOrder(movement, order);
    for (Axis axis : order) {
        double m = movement.get(axis);
        if (m != 0.0) {
            double c = collideShapes(axis, box.move(resolved), shapes, m);
            resolved = resolved.with(axis, c);
        }
    }
    return resolved;
}

}  // namespace mcp
