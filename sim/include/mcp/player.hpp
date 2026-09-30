// One player tick, ported from vanilla 26.3:
//   LocalPlayer.aiStep/applyInput/modifyInput (client input handling)
//   Player.aiStep/travel, LivingEntity.aiStep/travel/travelInAir/jumpFromGround,
//   Entity.move/collide/moveRelative/getInputVector.
// Scope (Phase 1): on land, survival, no effects, no items in use, no fluids,
// no climbables, no entities to collide with. Out-of-scope states set
// Player::unsupported rather than silently diverging.
#pragma once

#include <cstdint>

#include "mcp/jmath.hpp"
#include "mcp/vec.hpp"
#include "mcp/world.hpp"

namespace mcp {

// Attribute defaults and modifiers, as declared in 26.3 (Attributes,
// Player.createAttributes, LivingEntity.SPEED_MODIFIER_SPRINTING).
struct PlayerConstants {
    static constexpr double kMovementSpeedBase = static_cast<double>(0.1F);  // Player.createAttributes
    static constexpr double kSprintModifier = static_cast<double>(0.3F);    // ADD_MULTIPLIED_TOTAL
    static constexpr double kJumpStrength = static_cast<double>(0.42F);
    static constexpr double kGravity = 0.08;
    static constexpr double kStepHeight = 0.6;
    static constexpr double kSneakingSpeed = 0.3;
    static constexpr double kFrictionModifier = 1.0;
    static constexpr double kAirDragModifier = 1.0;
    static constexpr float kHalfWidth = 0.6F / 2.0F;
    static constexpr float kHeightStanding = 1.8F;
    static constexpr float kHeightCrouching = 1.5F;
    static constexpr float kDegToRad = static_cast<float>(3.141592653589793 / 180.0);
};

struct Keys {
    bool forward = false, backward = false, left = false, right = false;
    bool jump = false, shift = false, sprint = false;
};

struct Player {
    double x = 0.0, y = 0.0, z = 0.0;
    double xo = 0.0, yo = 0.0, zo = 0.0;  // position at the start of the last tick (setOldPosAndRot)
    Vec3 vel{};
    float yRot = 0.0F, xRot = 0.0F;
    float xxa = 0.0F, zza = 0.0F;
    bool jumping = false;
    bool onGround = false;
    bool horizontalCollision = false, minorHorizontalCollision = false;
    bool verticalCollision = false, verticalCollisionBelow = false;
    bool sprinting = false;       // shared flag 3 (Entity.isSprinting)
    bool sprintModifier = false;  // SPEED_MODIFIER_SPRINTING present on MOVEMENT_SPEED
    bool crouching = false;
    bool hasSupportingBlock = false;
    int32_t noJumpDelay = 0;
    Keys keys{};
    Vec2 moveVector{};
    bool unsupported = false;  // entered a state this port does not cover

    MCP_HD AABB boundingBox() const {
        double w = static_cast<double>(PlayerConstants::kHalfWidth);
        return AABB{x - w, y, z - w, x + w, y + static_cast<double>(PlayerConstants::kHeightStanding), z + w};
    }

    // The flag and the modifier are separate state: the server echoes them to
    // the client in different packets (entity data, attributes).
    MCP_HD double movementSpeedAttribute() const {
        double result = PlayerConstants::kMovementSpeedBase;
        if (sprintModifier) result *= 1.0 + PlayerConstants::kSprintModifier;
        return mth::clamp(result, 0.0, 1024.0);
    }

    // LivingEntity.setSprinting: the flag plus remove-then-add of the modifier.
    MCP_HD void setSprinting(bool value) {
        sprinting = value;
        sprintModifier = value;
    }
};

namespace detail {

MCP_HD inline float impulse(bool positive, bool negative) {
    return positive == negative ? 0.0F : (positive ? 1.0F : -1.0F);
}

MCP_HD inline float computeModifiedFriction(float friction, float modifier) {
    return mth::clamp(1.0F - (1.0F - friction) * modifier, 0.0F, 1.0F);
}

MCP_HD inline float distanceToUnitSquare(const Vec2& d) {
    float dx = ::fabsf(d.x), dy = ::fabsf(d.y);
    float tan = dy > dx ? dx / dy : dy / dx;
    return static_cast<float>(::sqrt(static_cast<double>(1.0F + tan * tan)));
}

MCP_HD inline Vec2 modifyInputSpeedForSquareMovement(const Vec2& in) {
    float length = in.length();
    if (length <= 0.0F) return in;
    Vec2 direction = in.scale(1.0F / length);
    float d = distanceToUnitSquare(direction);
    float modified = length * d;
    modified = modified < 1.0F ? modified : 1.0F;  // Math.min(float, float)
    return direction.scale(modified);
}

// Entity.getInputVector
MCP_HD inline Vec3 getInputVector(const Vec3& input, float speed, float yRot, const float* sinTab) {
    double len = input.lengthSqr();
    if (len < 1.0E-7) return Vec3{};
    Vec3 m = (len > 1.0 ? input.normalize() : input).scale(static_cast<double>(speed));
    float s = mth::sin(sinTab, static_cast<double>(yRot * PlayerConstants::kDegToRad));
    float c = mth::cos(sinTab, static_cast<double>(yRot * PlayerConstants::kDegToRad));
    return Vec3{m.x * c - m.z * s, m.y, m.z * c + m.x * s};
}

}  // namespace detail

// Entity.collide, including the step-up search.
template <typename World>
MCP_HD Vec3 collide(const Player& p, const World& w, const Vec3& movement) {
    AABB aabb = p.boundingBox();
    Colliders<64> shapes;
    Vec3 step = movement;
    if (movement.lengthSqr() != 0.0) {
        collectBlockCollisions(w, aabb.expandTowards(movement), shapes);
        step = collideWithShapes(movement, aabb, shapes);
    }
    bool xC = movement.x != step.x, yC = movement.y != step.y, zC = movement.z != step.z;
    bool onGroundAfter = yC && movement.y < 0.0;
    float maxUp = static_cast<float>(PlayerConstants::kStepHeight);
    if (maxUp > 0.0F && (onGroundAfter || p.onGround) && (xC || zC)) {
        AABB grounded = onGroundAfter ? aabb.move(0.0, step.y, 0.0) : aabb;
        AABB stepBox = grounded.expandTowards(movement.x, static_cast<double>(maxUp), movement.z);
        if (!onGroundAfter) stepBox = stepBox.expandTowards(0.0, static_cast<double>(-1.0E-5F), 0.0);
        Colliders<64> cols;
        collectBlockCollisions(w, stepBox, cols);
        float skip = static_cast<float>(step.y);
        float cand[8];
        int32_t n = 0;
        for (int32_t i = 0; i < cols.count; ++i) {
            for (int32_t k = 0; k < 2; ++k) {
                float rel = static_cast<float>((cols.pos[i].y + static_cast<double>(k)) - grounded.minY);
                if (rel < 0.0F || rel == skip) continue;
                if (rel > maxUp) break;
                bool dup = false;
                for (int32_t q = 0; q < n; ++q) dup = dup || cand[q] == rel;
                if (!dup && n < 8) cand[n++] = rel;
            }
        }
        for (int32_t a = 1; a < n; ++a)  // sort ascending
            for (int32_t b = a; b > 0 && cand[b - 1] > cand[b]; --b) {
                float t = cand[b];
                cand[b] = cand[b - 1];
                cand[b - 1] = t;
            }
        for (int32_t i = 0; i < n; ++i) {
            Vec3 fromGround = collideWithShapes(Vec3{movement.x, static_cast<double>(cand[i]), movement.z}, grounded, cols);
            if (fromGround.horizontalDistanceSqr() > step.horizontalDistanceSqr()) {
                double toGround = aabb.minY - grounded.minY;
                return fromGround.subtract(0.0, toGround, 0.0);
            }
        }
    }
    return step;
}

// Entity.move(MoverType.SELF or PLAYER, delta). `authoritative` is
// isLocalInstanceAuthoritative(): true for a client's own player, false for
// the server's copy of a client-authoritative player, which only updates its
// ground state when it moves vertically. Both simulate movement, so both
// apply collision restitution.
template <typename World>
MCP_HD void move(Player& p, const World& w, Vec3 delta, bool authoritative = true) {
    // Player.maybeBackOffFromEdge only changes delta when the player could fall
    // off an edge while sneaking; impossible on flat ground.
    Vec3 movement = collide(p, w, delta);
    double movementLength = movement.lengthSqr();
    if (movementLength > 1.0E-7 || delta.lengthSqr() - movementLength < 1.0E-7) {
        p.x = p.x + movement.x;
        p.y = p.y + movement.y;
        p.z = p.z + movement.z;
    }
    bool xCollision = !(::fabs(movement.x - delta.x) < static_cast<double>(1.0E-5F));
    bool zCollision = !(::fabs(movement.z - delta.z) < static_cast<double>(1.0E-5F));
    p.horizontalCollision = xCollision || zCollision;
    bool movedVertically = ::fabs(delta.y) > 0.0;
    if (movedVertically || authoritative) {
        p.verticalCollision = delta.y != movement.y;
        p.verticalCollisionBelow = p.verticalCollision && delta.y < 0.0;
        p.onGround = p.verticalCollisionBelow;
        p.hasSupportingBlock = p.onGround;  // flat ground always supports
    }
    if (p.horizontalCollision) {
        // LocalPlayer.isHorizontalCollisionMinor is not ported yet; the server
        // copy uses Entity's (always false), but walls are out of scope too.
        p.unsupported = true;
        p.minorHorizontalCollision = false;
    } else {
        p.minorHorizontalCollision = false;
    }
    if ((movedVertically && p.verticalCollision) || p.horizontalCollision) {
        // restituteMovementAfterCollisions with zero bounciness.
        double restitution = 0.0;
        Vec3 cur = p.vel, after = cur;
        if (xCollision) after = after.with(Axis::X, -cur.x * restitution);
        if (zCollision) after = after.with(Axis::Z, -cur.z * restitution);
        if (p.verticalCollision) {
            double gravityCompensation = 0.0, effectiveDrag = 1.0;
            after = after.with(Axis::Y, (gravityCompensation - cur.y) * effectiveDrag * restitution);
        }
        p.vel = after;
    }
    float f = blockSpeedFactorOf(Block::Air);
    p.vel = p.vel.multiply(static_cast<double>(f), 1.0, static_cast<double>(f));
}

// One full tick with the given held keys and rotation.
template <typename World>
MCP_HD void tick(Player& p, const Keys& keys, float yaw, float pitch, const World& w, const float* sinTab) {
    using C = PlayerConstants;
    p.yRot = yaw;
    p.xRot = pitch;
    p.xo = p.x;  // Level.tickEntities: setOldPosAndRot before the entity tick
    p.yo = p.y;
    p.zo = p.z;

    // ---- LocalPlayer.aiStep (input part) ----
    p.crouching = p.keys.shift;  // isShiftKeyDown() before the input is re-sampled
    p.keys = keys;
    p.moveVector = Vec2{detail::impulse(keys.left, keys.right), detail::impulse(keys.forward, keys.backward)}.normalized();
    bool forwardImpulse = p.moveVector.y > 1.0E-5F;
    bool movingSlowly = p.crouching;
    bool sprintPossible = true;  // food, mobility and shallow water are fine on flat land
    if (!p.sprinting && forwardImpulse && sprintPossible && !movingSlowly && keys.sprint) p.setSprinting(true);
    if (p.sprinting && (!sprintPossible || !forwardImpulse || (p.horizontalCollision && !p.minorHorizontalCollision)))
        p.setSprinting(false);

    // ---- LivingEntity.aiStep ----
    if (p.noJumpDelay > 0) p.noJumpDelay--;
    {
        double dx = p.vel.x, dy = p.vel.y, dz = p.vel.z;
        if (p.vel.horizontalDistanceSqr() < 9.0E-6) {
            dx = 0.0;
            dz = 0.0;
        }
        if (::fabs(p.vel.y) < 0.003) dy = 0.0;
        p.vel = Vec3{dx, dy, dz};
    }
    // LocalPlayer.applyInput / modifyInput
    {
        Vec2 in = p.moveVector;
        if (in.lengthSquared() != 0.0F) {
            in = in.scale(0.98F);
            if (movingSlowly) in = in.scale(static_cast<float>(C::kSneakingSpeed));
            in = detail::modifyInputSpeedForSquareMovement(in);
        }
        p.xxa = in.x;
        p.zza = in.y;
        p.jumping = keys.jump;
    }
    if (p.jumping) {
        if (p.onGround && p.noJumpDelay == 0) {
            // jumpFromGround
            float jumpPower = static_cast<float>(C::kJumpStrength) * 1.0F * blockJumpFactorOf(Block::Grass) + 0.0F;
            if (!(jumpPower <= 1.0E-5F)) {
                double vy = static_cast<double>(jumpPower) > p.vel.y ? static_cast<double>(jumpPower) : p.vel.y;
                p.vel = Vec3{p.vel.x, vy, p.vel.z};
                if (p.sprinting) {
                    float angle = p.yRot * C::kDegToRad;
                    Vec3 boost{static_cast<double>(-mth::sin(sinTab, static_cast<double>(angle))) * 0.2, 0.0,
                               static_cast<double>(mth::cos(sinTab, static_cast<double>(angle))) * 0.2};
                    p.vel = p.vel.add(boost);
                }
            }
            p.noJumpDelay = 10;
        }
    } else {
        p.noJumpDelay = 0;
    }

    // ---- travel -> travelInAir ----
    Vec3 input{static_cast<double>(p.xxa), 0.0, static_cast<double>(p.zza)};
    float blockFriction = p.onGround
        ? detail::computeModifiedFriction(blockFrictionOf(Block::Grass), static_cast<float>(C::kFrictionModifier))
        : 1.0F;
    // handleRelativeFrictionAndCalculateMovement
    float moveSpeed;
    if (p.onGround) {
        // Player.getSpeed() reads the attribute directly (sprint speed applies on
        // the tick sprinting starts; confirmed by the 03_sprint oracle trace).
        float speed = static_cast<float>(p.movementSpeedAttribute());
        moveSpeed = static_cast<double>(blockFriction) > 0.6
            ? speed * (0.21600002F / (blockFriction * blockFriction * blockFriction))
            : speed;
    } else {
        moveSpeed = p.sprinting ? 0.025999999F : 0.02F;  // Player.getFlyingSpeed
    }
    p.vel = p.vel.add(detail::getInputVector(input, moveSpeed, p.yRot, sinTab));
    move(p, w, p.vel);
    Vec3 movement = p.vel;
    double movementY = movement.y - C::kGravity;
    float airDrag = detail::computeModifiedFriction(0.91F, static_cast<float>(C::kAirDragModifier));
    float friction = blockFriction * airDrag;
    float verticalFriction = detail::computeModifiedFriction(0.98F, static_cast<float>(C::kAirDragModifier));
    p.vel = Vec3{movement.x * static_cast<double>(friction), movementY * static_cast<double>(verticalFriction),
                 movement.z * static_cast<double>(friction)};
}

}  // namespace mcp
