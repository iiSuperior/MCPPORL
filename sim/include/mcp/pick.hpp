// The client's crosshair pick, ported from vanilla 26.3:
//   LocalPlayer.pick / filterHitResult (bare hand: no ATTACK_RANGE component),
//   Entity.pick / getEyePosition / calculateViewVector,
//   BlockGetter.clip / traverseBlocks, VoxelShape.clip (full cubes),
//   AABB.clip / getDirection / clipPoint, ProjectileUtil.getEntityHitResult.
//
// Minecraft.tick runs pick(1.0F) right before handleKeybinds, so this is what
// decides whether a click attacks, mines or whiffs.
#pragma once

#include <cfloat>
#include <cstdint>

#include "mcp/jmath.hpp"
#include "mcp/vec.hpp"
#include "mcp/world.hpp"

namespace mcp {

struct PickConstants {
    static constexpr double kBlockInteractionRange = 4.5;   // Attributes.BLOCK_INTERACTION_RANGE
    static constexpr double kEntityInteractionRange = 3.0;  // Attributes.ENTITY_INTERACTION_RANGE
};

enum class HitType : int32_t { Miss = 0, Block = 1, Entity = 2 };

struct HitResult {
    HitType type = HitType::Miss;
    Vec3 location{};
};

namespace pick {

// Mth.lerp(double, double, double)
MCP_HD inline double lerp(double a, double p0, double p1) { return p0 + a * (p1 - p0); }

// Mth.frac(double): num - lfloor(num)
MCP_HD inline double frac(double v) { return v - static_cast<double>(j::d2l(::floor(v))); }

// Mth.sign(double)
MCP_HD inline int32_t sign(double v) { return v == 0.0 ? 0 : (v > 0.0 ? 1 : -1); }

MCP_HD inline double distanceToSqr(const Vec3& a, const Vec3& b) {
    double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

// Entity.calculateViewVector
MCP_HD inline Vec3 viewVector(float xRot, float yRot, const float* sinTab) {
    constexpr float kDeg = static_cast<float>(3.141592653589793 / 180.0);
    float realXRot = xRot * kDeg;
    float realYRot = -yRot * kDeg;
    float yCos = mth::cos(sinTab, static_cast<double>(realYRot));
    float ySin = mth::sin(sinTab, static_cast<double>(realYRot));
    float xCos = mth::cos(sinTab, static_cast<double>(realXRot));
    float xSin = mth::sin(sinTab, static_cast<double>(realXRot));
    return Vec3{static_cast<double>(ySin * xCos), static_cast<double>(-xSin), static_cast<double>(yCos * xCos)};
}

// AABB.contains(Vec3)
MCP_HD inline bool contains(const AABB& b, const Vec3& v) {
    return v.x >= b.minX && v.x < b.maxX && v.y >= b.minY && v.y < b.maxY && v.z >= b.minZ && v.z < b.maxZ;
}

// AABB.intersects(AABB)
MCP_HD inline bool intersects(const AABB& a, const AABB& b) {
    return a.minX < b.maxX && a.maxX > b.minX && a.minY < b.maxY && a.maxY > b.minY && a.minZ < b.maxZ && a.maxZ > b.minZ;
}

// AABB.clipPoint: returns true (and narrows scale) when this face is hit first.
MCP_HD inline bool clipPoint(double& scale, double da, double db, double dc, double point, double minB, double maxB,
                             double minC, double maxC, double fromA, double fromB, double fromC) {
    double s = (point - fromA) / da;
    double pb = fromB + s * db;
    double pc = fromC + s * dc;
    if (0.0 < s && s < scale && minB - 1.0E-7 < pb && pb < maxB + 1.0E-7 && minC - 1.0E-7 < pc && pc < maxC + 1.0E-7) {
        scale = s;
        return true;
    }
    return false;
}

// AABB.getDirection; `hit` stays true once any face was hit (direction != null).
MCP_HD inline void getDirection(const AABB& b, const Vec3& from, double& scale, bool& hit, double dx, double dy, double dz) {
    if (dx > 1.0E-7) hit |= clipPoint(scale, dx, dy, dz, b.minX, b.minY, b.maxY, b.minZ, b.maxZ, from.x, from.y, from.z);
    else if (dx < -1.0E-7) hit |= clipPoint(scale, dx, dy, dz, b.maxX, b.minY, b.maxY, b.minZ, b.maxZ, from.x, from.y, from.z);
    if (dy > 1.0E-7) hit |= clipPoint(scale, dy, dz, dx, b.minY, b.minZ, b.maxZ, b.minX, b.maxX, from.y, from.z, from.x);
    else if (dy < -1.0E-7) hit |= clipPoint(scale, dy, dz, dx, b.maxY, b.minZ, b.maxZ, b.minX, b.maxX, from.y, from.z, from.x);
    if (dz > 1.0E-7) hit |= clipPoint(scale, dz, dx, dy, b.minZ, b.minX, b.maxX, b.minY, b.maxY, from.z, from.x, from.y);
    else if (dz < -1.0E-7) hit |= clipPoint(scale, dz, dx, dy, b.maxZ, b.minX, b.maxX, b.minY, b.maxY, from.z, from.x, from.y);
}

// AABB.clip(from, to): the entry point on the box, if the segment enters it.
MCP_HD inline bool clip(const AABB& b, const Vec3& from, const Vec3& to, Vec3& out) {
    double scale = 1.0;
    bool hit = false;
    double dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    getDirection(b, from, scale, hit, dx, dy, dz);
    if (!hit) return false;
    out = from.add(scale * dx, scale * dy, scale * dz);
    return true;
}

// VoxelShape.clip(from, to, pos) for a full cube.
MCP_HD inline bool clipFullCube(const Vec3& from, const Vec3& to, int32_t x, int32_t y, int32_t z, Vec3& out) {
    Vec3 diff{to.x - from.x, to.y - from.y, to.z - from.z};
    if (diff.lengthSqr() < 1.0E-7) return false;
    Vec3 test = from.add(diff.scale(0.001));
    double tx = test.x - static_cast<double>(x), ty = test.y - static_cast<double>(y), tz = test.z - static_cast<double>(z);
    if (tx >= 0.0 && tx < 1.0 && ty >= 0.0 && ty < 1.0 && tz >= 0.0 && tz < 1.0) {  // findIndex == 0 on every axis
        out = test;
        return true;
    }
    AABB cube{static_cast<double>(x), static_cast<double>(y), static_cast<double>(z), static_cast<double>(x) + 1.0,
              static_cast<double>(y) + 1.0, static_cast<double>(z) + 1.0};
    return clip(cube, from, to, out);  // AABB.clip(toAabbs(), from, to, pos)
}

// BlockGetter.clip with ClipContext(OUTLINE, Fluid.NONE) over full-cube blocks.
template <typename World>
MCP_HD HitResult clipBlocks(const World& w, const Vec3& from, const Vec3& to) {
    HitResult miss{HitType::Miss, to};
    if (from.x == to.x && from.y == to.y && from.z == to.z) return miss;  // Vec3.equals (no NaNs here)
    double toX = lerp(-1.0E-7, to.x, from.x), toY = lerp(-1.0E-7, to.y, from.y), toZ = lerp(-1.0E-7, to.z, from.z);
    double fromX = lerp(-1.0E-7, from.x, to.x), fromY = lerp(-1.0E-7, from.y, to.y), fromZ = lerp(-1.0E-7, from.z, to.z);
    int32_t bx = mth::floor(fromX), by = mth::floor(fromY), bz = mth::floor(fromZ);
    Vec3 loc;
    auto visit = [&](int32_t x, int32_t y, int32_t z) { return w.solid(x, y, z) && clipFullCube(from, to, x, y, z, loc); };
    if (visit(bx, by, bz)) return HitResult{HitType::Block, loc};
    double dx = toX - fromX, dy = toY - fromY, dz = toZ - fromZ;
    int32_t sx = sign(dx), sy = sign(dy), sz = sign(dz);
    double tDeltaX = sx == 0 ? DBL_MAX : static_cast<double>(sx) / dx;
    double tDeltaY = sy == 0 ? DBL_MAX : static_cast<double>(sy) / dy;
    double tDeltaZ = sz == 0 ? DBL_MAX : static_cast<double>(sz) / dz;
    double tX = tDeltaX * (sx > 0 ? 1.0 - frac(fromX) : frac(fromX));
    double tY = tDeltaY * (sy > 0 ? 1.0 - frac(fromY) : frac(fromY));
    double tZ = tDeltaZ * (sz > 0 ? 1.0 - frac(fromZ) : frac(fromZ));
    while (tX <= 1.0 || tY <= 1.0 || tZ <= 1.0) {
        if (tX < tY) {
            if (tX < tZ) {
                bx += sx;
                tX += tDeltaX;
            } else {
                bz += sz;
                tZ += tDeltaZ;
            }
        } else if (tY < tZ) {
            by += sy;
            tY += tDeltaY;
        } else {
            bz += sz;
            tZ += tDeltaZ;
        }
        if (visit(bx, by, bz)) return HitResult{HitType::Block, loc};
    }
    return miss;
}

}  // namespace pick

// What the picking player needs: eye position inputs and view rotation.
struct PickView {
    double xo, yo, zo;  // position at the start of the last tick (Entity.xo/yo/zo)
    double x, y, z;     // current position
    float eyeHeight;    // 1.62F standing
    float xRot, yRot;   // view rotation at the pick
    AABB box;           // the picking player's own bounding box
};

// LocalPlayer.pick(cameraEntity, blockRange, entityRange, 1.0F) against one
// candidate entity box (a pickable entity in the client's level).
template <typename World>
MCP_HD HitResult clientPick(const PickView& v, const AABB& target, const World& w, const float* sinTab) {
    using C = PickConstants;
    double maxDistance = C::kBlockInteractionRange > C::kEntityInteractionRange ? C::kBlockInteractionRange
                                                                                : C::kEntityInteractionRange;
    double maxDistanceSq = maxDistance * maxDistance;
    // getEyePosition(1.0F): Mth.lerp with a = 1.0
    Vec3 from{pick::lerp(1.0, v.xo, v.x), pick::lerp(1.0, v.yo, v.y) + static_cast<double>(v.eyeHeight),
              pick::lerp(1.0, v.zo, v.z)};
    Vec3 dir = pick::viewVector(v.xRot, v.yRot, sinTab);
    // Entity.pick(maxDistance, 1.0F, false)
    Vec3 blockTo = from.add(dir.x * maxDistance, dir.y * maxDistance, dir.z * maxDistance);
    HitResult blockHit = pick::clipBlocks(w, from, blockTo);
    double blockDistanceSq = pick::distanceToSqr(blockHit.location, from);
    if (blockHit.type != HitType::Miss) {
        maxDistanceSq = blockDistanceSq;
        maxDistance = ::sqrt(maxDistanceSq);
    }
    Vec3 to = from.add(dir.x * maxDistance, dir.y * maxDistance, dir.z * maxDistance);
    AABB box = v.box.expandTowards(dir.scale(maxDistance));
    box = AABB{box.minX - 1.0, box.minY - 1.0, box.minZ - 1.0, box.maxX + 1.0, box.maxY + 1.0, box.maxZ + 1.0};
    // ProjectileUtil.getEntityHitResult(camera, from, to, box, CAN_BE_PICKED, maxDistanceSq)
    bool entityFound = false;
    Vec3 entityLoc;
    if (pick::intersects(target, box)) {
        AABB bb = AABB{target.minX - 0.0, target.minY - 0.0, target.minZ - 0.0, target.maxX + 0.0, target.maxY + 0.0,
                       target.maxZ + 0.0};  // inflate(getPickRadius() = 0.0F)
        Vec3 clipLoc;
        bool clipped = pick::clip(bb, from, to, clipLoc);
        if (pick::contains(bb, from)) {
            if (maxDistanceSq >= 0.0) {  // canBePickedFromInside
                entityFound = true;
                entityLoc = clipped ? clipLoc : from;
            }
        } else if (clipped) {
            double dd = pick::distanceToSqr(from, clipLoc);
            if (dd < maxDistanceSq || maxDistanceSq == 0.0) {
                entityFound = true;
                entityLoc = clipLoc;
            }
        }
    }
    // filterHitResult: Vec3.closerThan is a strict distanceToSqr < range^2.
    if (entityFound && pick::distanceToSqr(entityLoc, from) < blockDistanceSq) {
        if (pick::distanceToSqr(entityLoc, from) < C::kEntityInteractionRange * C::kEntityInteractionRange)
            return HitResult{HitType::Entity, entityLoc};
        return HitResult{HitType::Miss, entityLoc};
    }
    if (pick::distanceToSqr(blockHit.location, from) < C::kBlockInteractionRange * C::kBlockInteractionRange)
        return blockHit;
    return HitResult{HitType::Miss, blockHit.location};
}

}  // namespace mcp
