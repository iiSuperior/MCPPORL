// Two-player melee, ported from vanilla 26.3. Mirrors the combat oracle
// (oracle/harness CombatOracle): each player is a client copy (owns its
// movement, see player.hpp) plus the server's copy of it, and the two talk
// only through the packets a real client and server exchange, in the same
// order. Zero latency: packets sent during a tick are handled on that tick's
// server step and the replies are applied before the next client tick.
//
// Server side ported from: ServerGamePacketListenerImpl.handleMovePlayer /
// handlePlayerPositionChange / handleAttack / handlePlayerCommand / tickPlayer,
// ServerPlayer.tick/doTick, Player.attack/causeExtraKnockback/canCriticalAttack,
// LivingEntity.hurtServer/actuallyHurt/knockback/dealDefaultKnockback/baseTick,
// Entity.checkFallDamage, AttackRange.isInRange, ServerEntity.sendChanges.
//
// Scope: bare hands, no armor or effects, no shields, no deaths, no fall
// damage, no sneaking, no player-player pushing, full-cube flat arenas, no
// health regeneration (the oracle disables it). Leaving that scope sets
// `unsupported` instead of silently diverging.
#pragma once

#include <cstdint>

#include "mcp/lpvec3.hpp"
#include "mcp/player.hpp"

namespace mcp {

struct CombatConstants {
    static constexpr float kMaxHealth = 20.0F;
    static constexpr float kAttackDamage = 1.0F;                 // Attributes.ATTACK_DAMAGE, player base
    static constexpr double kAttackSpeed = 4.0;                   // Attributes.ATTACK_SPEED, player base
    static constexpr float kEntityInteractionRange = 3.0F;        // Attributes.ENTITY_INTERACTION_RANGE
    static constexpr double kAttackRangeBuffer = 3.0;             // handleAttack's extra buffer
    static constexpr float kEyeHeightStanding = 1.62F;
    static constexpr int32_t kDamageCooldown = 20;
    static constexpr int32_t kHurtDuration = 10;
    static constexpr double kSafeFallDistance = 3.0;
};

// Inputs for one player on one tick (CombatScenario.Input).
struct DuelInput {
    Keys keys{};
    bool attack = false;
    float yaw = 0.0F, pitch = 0.0F;
};

// ServerboundMovePlayerPacket (Pos, PosRot, Rot, StatusOnly).
struct MovePacket {
    bool hasPos = false, hasRot = false;
    double x = 0.0, y = 0.0, z = 0.0;
    float yRot = 0.0F, xRot = 0.0F;
    bool onGround = false, horizontalCollision = false;
};

// Everything one client sends during one tick, in send order:
// attack (handleKeybinds), then sendChanges (input, sprint command, move),
// then ServerboundClientTickEndPacket.
struct ClientPackets {
    bool attack = false;
    bool input = false;
    Keys inputKeys{};
    int32_t sprintCommand = 0;  // +1 START_SPRINTING, -1 STOP_SPRINTING
    bool move = false;
    MovePacket movePacket{};
};

// The server's copy of one player (ServerPlayer) plus its connection's
// movement bookkeeping (ServerGamePacketListenerImpl).
struct ServerCopy {
    Player body;  // position, rotation, velocity, ground and collision state, sprint flag
    double fallDistance = 0.0;
    float health = CombatConstants::kMaxHealth;
    float lastHurt = 0.0F;
    int32_t hurtTime = 0;
    int32_t damageCooldownTime = 0;
    int32_t attackStrengthTicker = 0;
    bool syncVelocity = false;
    double lastGoodX = 0.0, lastGoodY = 0.0, lastGoodZ = 0.0;
    // Entity data and attribute changes not yet echoed to the player's client.
    bool sprintFlagDirty = false, speedAttributeDirty = false;

    // LivingEntity.setSprinting with SynchedEntityData / AttributeInstance dirtiness.
    MCP_HD void setSprinting(bool value) {
        if (body.sprinting != value) sprintFlagDirty = true;
        if (body.sprintModifier || value) speedAttributeDirty = true;  // removeModifier hit, or addTransientModifier
        body.setSprinting(value);
    }

    // Player.getAttackStrengthScale
    MCP_HD float attackStrengthScale(float a) const {
        float delay = static_cast<float>(1.0 / CombatConstants::kAttackSpeed * 20.0);
        return mth::clamp((static_cast<float>(attackStrengthTicker) + a) / delay, 0.0F, 1.0F);
    }
};

// What the server sends back to one player's client during one step, in order.
struct ServerReplies {
    bool motion = false;
    Vec3 motionVelocity{};  // ClientboundSetEntityMotionPacket payload before LpVec3 encoding
    bool sprintFlag = false, sprintFlagValue = false;          // ClientboundSetEntityDataPacket
    bool speedAttribute = false, speedModifierValue = false;   // ClientboundUpdateAttributesPacket
};

// LocalPlayer.sendChanges / sendPosition state.
struct ClientSendState {
    double xLast = 0.0, yLast = 0.0, zLast = 0.0;
    float yRotLast = 0.0F, xRotLast = 0.0F;
    bool lastOnGround = false, lastHorizontalCollision = false, wasSprinting = false;
    int32_t positionReminder = 0;
    Keys lastSentInput{};
};

struct DuelPlayer {
    Player client;
    int32_t clientAttackStrengthTicker = 0;
    ServerCopy server;
    ClientSendState send;
    ClientPackets packets;
    ServerReplies replies;
    // per-tick trace flags
    bool gotVelocity = false, sentAttack = false;
};

namespace duel {

MCP_HD inline bool sameKeys(const Keys& a, const Keys& b) {
    return a.forward == b.forward && a.backward == b.backward && a.left == b.left && a.right == b.right &&
           a.jump == b.jump && a.shift == b.shift && a.sprint == b.sprint;
}

// Mth.wrapDegrees(float)
MCP_HD inline float wrapDegrees(float v) {
    float f = ::fmodf(v, 360.0F);
    if (f >= 180.0F) f -= 360.0F;
    if (f < -180.0F) f += 360.0F;
    return f;
}

// AABB.distanceToSqr(Vec3)
MCP_HD inline double distanceToSqr(const AABB& b, double x, double y, double z) {
    double dx = ::fmax(::fmax(b.minX - x, x - b.maxX), 0.0);
    double dy = ::fmax(::fmax(b.minY - y, y - b.maxY), 0.0);
    double dz = ::fmax(::fmax(b.minZ - z, z - b.maxZ), 0.0);
    return dx * dx + dy * dy + dz * dz;
}

// ---------------------------------------------------------------------------
// Client side
// ---------------------------------------------------------------------------

// One client tick: keybinds (attack), movement, sendChanges.
template <typename World>
MCP_HD void clientTick(DuelPlayer& me, const DuelInput& in, const World& w, const float* sinTab) {
    ClientPackets& out = me.packets;
    out = ClientPackets{};
    me.sentAttack = false;
    // Minecraft.handleKeybinds runs before the entity tick, so the attack packet
    // precedes this tick's movement. Client-side Player.attack changes nothing
    // but the attack strength ticker (hurtClient is false for other players).
    if (in.attack) {
        out.attack = true;
        me.clientAttackStrengthTicker = 0;
        me.sentAttack = true;
    }
    tick(me.client, in.keys, in.yaw, in.pitch, w, sinTab);
    me.clientAttackStrengthTicker++;

    // LocalPlayer.sendChanges -> sendPosition / sendIsSprintingIfNeeded
    Player& c = me.client;
    ClientSendState& s = me.send;
    if (!sameKeys(s.lastSentInput, c.keys)) {
        out.input = true;
        out.inputKeys = c.keys;
        s.lastSentInput = c.keys;
    }
    if (c.sprinting != s.wasSprinting) {
        out.sprintCommand = c.sprinting ? 1 : -1;
        s.wasSprinting = c.sprinting;
    }
    double dx = c.x - s.xLast, dy = c.y - s.yLast, dz = c.z - s.zLast;
    double dyRot = static_cast<double>(c.yRot - s.yRotLast), dxRot = static_cast<double>(c.xRot - s.xRotLast);
    s.positionReminder++;
    bool move = dx * dx + dy * dy + dz * dz > 2.0E-4 * 2.0E-4 || s.positionReminder >= 20;
    bool rot = dyRot != 0.0 || dxRot != 0.0;
    MovePacket& m = out.movePacket;
    m = MovePacket{};
    m.onGround = c.onGround;
    m.horizontalCollision = c.horizontalCollision;
    if (move || rot) {
        out.move = true;
        m.hasPos = move;
        m.hasRot = rot;
        m.x = c.x;
        m.y = c.y;
        m.z = c.z;
        m.yRot = c.yRot;
        m.xRot = c.xRot;
    } else if (s.lastOnGround != c.onGround || s.lastHorizontalCollision != c.horizontalCollision) {
        out.move = true;  // StatusOnly
    }
    if (move) {
        s.xLast = c.x;
        s.yLast = c.y;
        s.zLast = c.z;
        s.positionReminder = 0;
    }
    if (rot) {
        s.yRotLast = c.yRot;
        s.xRotLast = c.xRot;
    }
    s.lastOnGround = c.onGround;
    s.lastHorizontalCollision = c.horizontalCollision;
}

// ClientPacketListener: apply what the server sent (before the next client tick).
MCP_HD inline void deliver(DuelPlayer& me) {
    ServerReplies& r = me.replies;
    me.gotVelocity = false;
    if (r.motion) {
        me.client.vel = lpvec3::roundTrip(r.motionVelocity);  // Entity.lerpMotion
        me.gotVelocity = true;
    }
    if (r.sprintFlag) me.client.sprinting = r.sprintFlagValue;  // assignValues: the flag only
    if (r.speedAttribute) me.client.sprintModifier = r.speedModifierValue;
    r = ServerReplies{};
}

// ---------------------------------------------------------------------------
// Server side
// ---------------------------------------------------------------------------

// LivingEntity.jumpFromGround on the server copy (triggered by handlePlayerPositionChange).
MCP_HD inline void serverJumpFromGround(ServerCopy& sp, const float* sinTab) {
    using C = PlayerConstants;
    float jumpPower = static_cast<float>(C::kJumpStrength) * 1.0F * blockJumpFactorOf(Block::Grass) + 0.0F;
    if (jumpPower <= 1.0E-5F) return;
    Player& b = sp.body;
    double vy = static_cast<double>(jumpPower) > b.vel.y ? static_cast<double>(jumpPower) : b.vel.y;  // Math.max
    b.vel = Vec3{b.vel.x, vy, b.vel.z};
    if (b.sprinting) {
        float angle = b.yRot * C::kDegToRad;
        b.vel = b.vel.add(static_cast<double>(-mth::sin(sinTab, static_cast<double>(angle))) * 0.2, 0.0,
                          static_cast<double>(mth::cos(sinTab, static_cast<double>(angle))) * 0.2);
    }
}

// Entity.checkFallDamage as reached through doCheckFallDamage (chunks loaded).
MCP_HD inline void checkFallDamage(ServerCopy& sp, double ya, bool onGround) {
    if (ya < 0.0) sp.fallDistance -= static_cast<double>(static_cast<float>(ya));
    if (onGround) {
        if (sp.fallDistance > CombatConstants::kSafeFallDistance) sp.body.unsupported = true;  // fall damage not ported
        sp.fallDistance = 0.0;
    }
}

// ServerGamePacketListenerImpl.handleMovePlayer -> handlePlayerPositionChange.
template <typename World>
MCP_HD void handleMove(ServerCopy& sp, const MovePacket& pk, const World& w, const float* sinTab) {
    Player& b = sp.body;
    double reqX = pk.hasPos ? pk.x : b.x, reqY = pk.hasPos ? pk.y : b.y, reqZ = pk.hasPos ? pk.z : b.z;
    float reqYRot = pk.hasRot ? pk.yRot : b.yRot, reqXRot = pk.hasRot ? pk.xRot : b.xRot;
    double targetX = mth::clamp(reqX, -3.0E7, 3.0E7);
    double targetY = mth::clamp(reqY, -2.0E7, 2.0E7);
    double targetZ = mth::clamp(reqZ, -3.0E7, 3.0E7);
    float targetYRot = wrapDegrees(reqYRot), targetXRot = wrapDegrees(reqXRot);
    double startY = b.y;
    // The "moved too quickly" check (100 m^2 per packet) cannot trigger in scope.
    double xDist = targetX - sp.lastGoodX, yDist = targetY - sp.lastGoodY, zDist = targetZ - sp.lastGoodZ;
    bool movedUpwards = yDist > 0.0;
    if (b.onGround && !pk.onGround && movedUpwards) serverJumpFromGround(sp, sinTab);
    move(b, w, Vec3{xDist, yDist, zDist}, /*authoritative=*/false);  // MoverType.PLAYER
    double ex = targetX - b.x, ez = targetZ - b.z;                  // the y error is always zeroed
    if (ex * ex + ez * ez > 0.0625) b.unsupported = true;           // "moved wrongly": teleport path not ported
    // absSnapTo(target, rot)
    b.x = targetX;
    b.y = targetY;
    b.z = targetZ;
    b.yRot = ::fmodf(targetYRot, 360.0F);
    b.xRot = ::fmodf(mth::clamp(targetXRot, -90.0F, 90.0F), 360.0F);
    double dy = b.y - startY;
    // setOnGroundWithMovement(packet onGround, packet horizontalCollision)
    b.onGround = pk.onGround;
    b.horizontalCollision = pk.horizontalCollision;
    checkFallDamage(sp, dy, pk.onGround);  // doCheckFallDamage
    if (movedUpwards) sp.fallDistance = 0.0;
    sp.lastGoodX = b.x;
    sp.lastGoodY = b.y;
    sp.lastGoodZ = b.z;
}

// LivingEntity.knockback
MCP_HD inline void knockback(ServerCopy& victim, double power, double xd, double zd) {
    power *= 1.0 - 0.0;  // KNOCKBACK_RESISTANCE
    if (power <= 0.0) return;
    if (xd * xd + zd * zd < static_cast<double>(1.0E-5F)) {
        victim.body.unsupported = true;  // random direction (players on the same spot) not ported
        return;
    }
    Vec3 dv = Vec3{xd, 0.0, zd}.normalize().scale(power);
    const Vec3& m = victim.body.vel;
    double vy = victim.body.onGround ? ::fmin(0.4, m.y / 2.0 + power) : m.y;  // Math.min: operands are never NaN here
    victim.body.vel = Vec3{m.x / 2.0 - dv.x, vy, m.z / 2.0 - dv.z};
}

// LivingEntity.hurtServer for a player hit by a player's melee attack.
MCP_HD inline bool hurt(ServerCopy& victim, const ServerCopy& attacker, float damage) {
    if (victim.health <= 0.0F) return false;  // isDeadOrDying
    if (damage == 0.0F) return false;         // Player.hurtServer
    bool tookFullDamage = true;
    float dealt;
    if (static_cast<float>(victim.damageCooldownTime) > 10.0F) {
        if (damage <= victim.lastHurt) return false;
        dealt = damage - victim.lastHurt;
        victim.lastHurt = damage;
        tookFullDamage = false;
    } else {
        victim.lastHurt = damage;
        victim.damageCooldownTime = CombatConstants::kDamageCooldown;
        dealt = damage;
    }
    // actuallyHurt: no armor, resistance, protection or absorption in scope,
    // so the damage reaches health unchanged.
    if (dealt != 0.0F) victim.health = mth::clamp(victim.health - dealt, 0.0F, CombatConstants::kMaxHealth);
    if (tookFullDamage) victim.hurtTime = CombatConstants::kHurtDuration;
    if (tookFullDamage) {
        victim.syncVelocity = true;  // markHurt
        // dealDefaultKnockback: from the damage source's position (the attacker).
        knockback(victim, static_cast<double>(0.4F), attacker.body.x - victim.body.x, attacker.body.z - victim.body.z);
    }
    if (victim.health <= 0.0F) victim.body.unsupported = true;  // death and totems not ported
    return true;
}

// Player.attack (bare hand) followed by causeExtraKnockback.
MCP_HD inline void attack(ServerCopy& a, ServerCopy& target, ServerReplies& targetReplies, const float* sinTab) {
    float baseDamage = CombatConstants::kAttackDamage;
    float strength = a.attackStrengthScale(0.5F);
    float magicBoost = strength * (baseDamage - baseDamage);  // no enchantments
    baseDamage *= 0.2F + strength * strength * 0.8F;         // baseDamageScaleFactor
    a.attackStrengthTicker = 0;                                // onAttack -> resetOnlyAttackStrengthTicker
    bool fullStrengthAttack = strength > 0.9F;
    bool knockbackAttack = a.body.sprinting && fullStrengthAttack;
    baseDamage += 0.0F;  // item attack damage bonus
    bool crit = fullStrengthAttack && a.fallDistance > 0.0 && !a.body.onGround && !a.body.sprinting;
    if (crit) baseDamage *= 1.5F;
    float totalDamage = baseDamage + magicBoost;
    Vec3 oldMovement = target.body.vel;
    if (!hurt(target, a, totalDamage)) return;
    // causeExtraKnockback: getKnockback is ATTACK_KNOCKBACK (0) / 2.
    float knockbackAmount = 0.0F / 2.0F + (knockbackAttack ? 0.5F : 0.0F);
    if (knockbackAmount > 0.0F) {
        float rad = a.body.yRot * static_cast<float>(3.141592653589793 / 180.0);
        knockback(target, static_cast<double>(knockbackAmount), static_cast<double>(mth::sin(sinTab, static_cast<double>(rad))),
                  static_cast<double>(-mth::cos(sinTab, static_cast<double>(rad))));
        a.body.vel = a.body.vel.multiply(0.6, 1.0, 0.6);
        a.setSprinting(false);
    }
    if (target.syncVelocity) {
        targetReplies.motion = true;
        targetReplies.motionVelocity = target.body.vel;
        target.syncVelocity = false;
        target.body.vel = oldMovement;
    }
}

// handleAttack: range check, then Player.attack.
MCP_HD inline void handleAttack(ServerCopy& a, ServerCopy& target, ServerReplies& targetReplies, const float* sinTab) {
    AABB box = target.body.boundingBox();
    double eyeY = a.body.y + static_cast<double>(CombatConstants::kEyeHeightStanding);
    double distance = ::sqrt(distanceToSqr(box, a.body.x, eyeY, a.body.z));
    double minReach = static_cast<double>(0.0F - 0.0F) - CombatConstants::kAttackRangeBuffer;
    double maxReach = static_cast<double>(CombatConstants::kEntityInteractionRange + 0.0F) + CombatConstants::kAttackRangeBuffer;
    if (!(distance >= minReach && distance <= maxReach)) return;
    attack(a, target, targetReplies, sinTab);
}

// ServerGamePacketListenerImpl.tickPlayer -> ServerPlayer.doTick -> Player.tick.
// The server simulates the copy's movement with no input (it never sees the
// client's keys as movement), then snaps it back to where the client said it
// was; only the velocity and ground state carry over.
template <typename World>
MCP_HD void serverTickPlayer(ServerCopy& sp, const World& w) {
    using C = PlayerConstants;
    Player& b = sp.body;
    double firstGoodX = b.x, firstGoodY = b.y, firstGoodZ = b.z;  // resetPosition
    sp.lastGoodX = b.x;
    sp.lastGoodY = b.y;
    sp.lastGoodZ = b.z;
    // LivingEntity.baseTick
    if (sp.hurtTime > 0) sp.hurtTime--;
    // LivingEntity.aiStep
    if (b.noJumpDelay > 0) b.noJumpDelay--;
    {
        double dx = b.vel.x, dy = b.vel.y, dz = b.vel.z;
        if (b.vel.horizontalDistanceSqr() < 9.0E-6) {
            dx = 0.0;
            dz = 0.0;
        }
        if (::fabs(b.vel.y) < 0.003) dy = 0.0;
        b.vel = Vec3{dx, dy, dz};
    }
    b.noJumpDelay = 0;  // not jumping
    // travel -> travelInAir with zero input
    float blockFriction = b.onGround
        ? detail::computeModifiedFriction(blockFrictionOf(Block::Grass), static_cast<float>(C::kFrictionModifier))
        : 1.0F;
    b.vel = b.vel.add(Vec3{});  // moveRelative: getInputVector returns Vec3.ZERO
    move(b, w, b.vel, /*authoritative=*/false);
    Vec3 movement = b.vel;
    double movementY = movement.y - C::kGravity;
    float airDrag = detail::computeModifiedFriction(0.91F, static_cast<float>(C::kAirDragModifier));
    float friction = blockFriction * airDrag;
    float verticalFriction = detail::computeModifiedFriction(0.98F, static_cast<float>(C::kAirDragModifier));
    b.vel = Vec3{movement.x * static_cast<double>(friction), movementY * static_cast<double>(verticalFriction),
                 movement.z * static_cast<double>(friction)};
    // Player.tick
    sp.attackStrengthTicker++;
    // absSnapTo(firstGood)
    b.x = firstGoodX;
    b.y = firstGoodY;
    b.z = firstGoodZ;
}

}  // namespace duel

// A zero-latency duel, stepped exactly like the oracle harness.
struct Duel {
    DuelPlayer p[2];
    int32_t clientPushTicks = 0;  // ticks where a real client would have been pushed (not modelled)

    // Place both players and run the login handshake (three server ticks).
    template <typename World>
    MCP_HD void spawn(int32_t i, double x, double y, double z, float yaw, const World& w) {
        DuelPlayer& d = p[i];
        d = DuelPlayer{};
        d.client.x = x;
        d.client.y = y;
        d.client.z = z;
        d.client.yRot = yaw;
        d.send.xLast = x;
        d.send.yLast = y;
        d.send.zLast = z;
        d.send.yRotLast = yaw;
        ServerCopy& s = d.server;
        s.body.x = x;
        s.body.y = y;
        s.body.z = z;
        s.body.yRot = duel::wrapDegrees(yaw);  // from the teleport acknowledgement
        s.lastGoodX = x;
        s.lastGoodY = y;
        s.lastGoodZ = z;
        for (int32_t k = 0; k < 3; ++k) duel::serverTickPlayer(s, w);
    }

    template <typename World>
    MCP_HD void step(const DuelInput& a, const DuelInput& b, const World& w, const float* sinTab) {
        duel::clientTick(p[0], a, w, sinTab);
        duel::clientTick(p[1], b, w, sinTab);
        // Server: each client's packets in send order, then one server tick.
        for (int32_t i = 0; i < 2; ++i) {
            DuelPlayer& me = p[i];
            DuelPlayer& other = p[1 - i];
            const ClientPackets& pk = me.packets;
            if (pk.attack) duel::handleAttack(me.server, other.server, other.replies, sinTab);
            if (pk.input && pk.inputKeys.shift) me.server.body.unsupported = true;  // server-side sneaking not ported
            if (pk.sprintCommand != 0) me.server.setSprinting(pk.sprintCommand > 0);
            if (pk.move) duel::handleMove(me.server, pk.movePacket, w, sinTab);
        }
        // MinecraftServer.tickServer: ServerEntity.sendChanges echoes dirty entity
        // data (and with it dirty attributes) to the player itself...
        for (int32_t i = 0; i < 2; ++i) {
            ServerCopy& s = p[i].server;
            ServerReplies& r = p[i].replies;
            if (s.sprintFlagDirty) {
                r.sprintFlag = true;
                r.sprintFlagValue = s.body.sprinting;
                if (s.speedAttributeDirty) {
                    r.speedAttribute = true;
                    r.speedModifierValue = s.body.sprintModifier;
                }
                s.speedAttributeDirty = false;  // sent with the entity data
            } else if (s.speedAttributeDirty) {
                // Attribute-only changes wait for the tracker's update interval;
                // in scope they never change the modifier, so dropping them is exact.
                s.speedAttributeDirty = false;
            }
            s.sprintFlagDirty = false;
        }
        // ...then ServerPlayer.tick for every player (entity tick list)...
        for (int32_t i = 0; i < 2; ++i) {
            ServerCopy& s = p[i].server;
            if (s.damageCooldownTime > 0) s.damageCooldownTime--;
        }
        // ...then each connection's tick (tickPlayer).
        for (int32_t i = 0; i < 2; ++i) duel::serverTickPlayer(p[i].server, w);
        // Overlapping players: the server copies do not push each other (the
        // oracle's server velocities are unaffected), but a real client is
        // pushed locally by the remote player's interpolated position. Neither
        // the oracle nor this port models that yet; count it so callers can see.
        if (overlapping(p[0].client, p[1].client)) clientPushTicks++;
    }

    MCP_HD static bool overlapping(const Player& a, const Player& b) {
        AABB x = a.boundingBox(), y = b.boundingBox();
        return x.intersects(y.minX, y.minY, y.minZ, y.maxX, y.maxY, y.maxZ);
    }

    // Apply the server's replies; call after reading the tick's state.
    MCP_HD void deliver() {
        duel::deliver(p[0]);
        duel::deliver(p[1]);
    }

    MCP_HD bool unsupported() const {
        return p[0].client.unsupported || p[1].client.unsupported || p[0].server.body.unsupported ||
               p[1].server.body.unsupported;
    }
};

}  // namespace mcp
