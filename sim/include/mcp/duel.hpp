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
// Each client sees the other player through the entity tracker and the
// client-side interpolation of tracker.hpp, and aims at that view.
//
// Hotbar switching and shields: ServerGamePacketListenerImpl.handleSetCarriedItem /
// handleUseItem / handlePlayerAction(RELEASE_USE_ITEM), LivingEntity.startUsingItem /
// stopUsingItem / updatingUsingItem / getItemBlockingWith / applyItemBlocking,
// Player.blockUsingItem, BlocksAttacks, ItemCooldowns; client side
// Minecraft.handleKeybinds / startUseItem, MultiPlayerGameMode.useItem /
// releaseUsingItem / ensureHasSentCarriedItem, LocalPlayer.onSyncedDataUpdated.
//
// Scope: bare hands, swords, axes and shields (weapons.hpp), no armor or effects, no totems, no fall
// damage, no sneaking, full-cube flat arenas, no health regeneration (the
// oracle disables it). Leaving that scope sets
// `unsupported` instead of silently diverging. A death ends the episode: the
// killing tick is simulated in full, nothing after it.
#pragma once

#include <cstdint>

#include "mcp/lpvec3.hpp"
#include "mcp/pick.hpp"
#include "mcp/player.hpp"
#include "mcp/rng.hpp"
#include "mcp/tracker.hpp"
#include "mcp/weapons.hpp"

namespace mcp {

struct CombatConstants {
    static constexpr float kMaxHealth = 20.0F;
    static constexpr float kEntityInteractionRange = 3.0F;        // Attributes.ENTITY_INTERACTION_RANGE
    static constexpr double kAttackRangeBuffer = 3.0;             // handleAttack's extra buffer
    static constexpr float kEyeHeightStanding = 1.62F;
    static constexpr int32_t kDamageCooldown = 20;
    static constexpr int32_t kHurtDuration = 10;
    static constexpr double kSafeFallDistance = 3.0;
};

// Inputs for one player on one tick (CombatScenario.Input).
//   attack:     the attack key was pressed since the last tick (a click). What
//               it does is decided by the crosshair pick, as in vanilla.
//   attackHeld: the key is down when the tick samples it (Minecraft.
//               continueAttack). Releasing it clears the whiff lockout;
//               holding it on a block mines.
//   yaw/pitch:  the rotation during the tick (the mouse moves between ticks),
//               used by the pick, the movement and the rotation packet.
struct DuelInput {
    Keys keys{};
    bool attack = false;
    bool attackHeld = false;
    float yaw = 0.0F, pitch = 0.0F;
    // Scripted-opponent cheat (never set for a learner, never in oracle
    // scenarios): a click lands whenever the opponent's true hitbox is within
    // entity reach, without the crosshair pick. See env.hpp, expert panel.
    bool rangeHit = false;
    // slot: a hotbar key pressed this tick (0-8), or -1. use: the use key is
    // down when the tick samples it (a press on the first such tick).
    int32_t slot = -1;
    bool use = false;
};

// Serverbound packets sent from Minecraft.handleKeybinds, in send order (the
// movement packets of sendChanges follow them).
enum class ActionKind : uint8_t { Carried, Attack, Punch, Interact, UseItemOn, UseItem, Release };
struct ActionPacket {
    ActionKind kind = ActionKind::Punch;
    int8_t arg = 0;  // Carried: the slot; Interact / UseItemOn / UseItem: 1 for the off hand
    float yRot = 0.0F, xRot = 0.0F;  // UseItem: the rotation it carries
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
    ActionPacket actions[16];
    int32_t actionCount = 0;
    bool attack = false;
    int32_t punches = 0;      // ServerboundPunchPacket (startAttack, and continueAttack on a block)
    int32_t blockActions = 0; // ServerboundPlayerActionPacket START/ABORT_DESTROY_BLOCK (no combat effect)
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
    // Inventory: the hotbar with the selected slot (the main hand) and the off
    // hand, with the durability used of each item (a broken item is flagged
    // instead of silently kept).
    Weapon hotbar[9] = {};
    int32_t hotbarDamage[9] = {};
    int32_t selected = 0;
    Weapon offhand = Weapon::Hand;
    int32_t offhandDamage = 0;
    // The item whose attribute modifiers are applied (detectEquipmentUpdates,
    // in the server tick: an attack handled before it uses the old item's), and
    // Player.lastItemInMainHand (a different item resets the attack strength).
    Weapon attrWeapon = Weapon::Hand;
    Weapon lastMainHand = Weapon::Hand;
    // Using an item (a shield): the living-entity flags (bit 1 using, bit 2 off
    // hand; the flags are the use state, LivingEntity.isUsingItem), the item and
    // useItemRemaining.
    bool flagUsing = false, flagOffhand = false, flagsDirty = false;
    Weapon useItem = Weapon::Hand;
    int32_t useItemRemaining = 0;
    // ItemCooldowns, shield group: ticks left and the duration it was set with.
    int32_t shieldCooldown = 0, shieldCooldownDuration = 0;
    float yHeadRot = 0.0F;  // set to yRot at the end of Player.aiStep
    bool syncVelocity = false;
    // ServerPlayer.die -> markClientUnloadedAfterDeath: hasClientLoaded() turns
    // false, so the dead player's later packets are ignored and it takes no damage.
    bool dead = false;
    // Stand-in for the entity's own RandomSource, used only where vanilla draws
    // from it in scope (knockback between players on the same spot). Its state
    // cannot be reproduced, so each draw is a counted non-parity event.
    SplitMix64 rng{0};
    int32_t randomKnockbacks = 0;
    double lastGoodX = 0.0, lastGoodY = 0.0, lastGoodZ = 0.0;
    // Entity data and attribute changes not yet echoed to the player's client.
    bool sprintFlagDirty = false, speedAttributeDirty = false;
    // Other synched data changed (health), and Entity.needsSync (knockback,
    // pushing): either makes the tracker consider a move packet off its cadence.
    bool dataDirty = false, needsSync = false;

    // LivingEntity.setSprinting with SynchedEntityData / AttributeInstance dirtiness.
    MCP_HD void setSprinting(bool value) {
        if (body.sprinting != value) sprintFlagDirty = true;
        if (body.sprintModifier || value) speedAttributeDirty = true;  // removeModifier hit, or addTransientModifier
        body.setSprinting(value);
    }

    MCP_HD Weapon mainHand() const { return hotbar[selected]; }
    MCP_HD Weapon inHand(bool off) const { return off ? offhand : mainHand(); }
    MCP_HD int32_t& damageOf(bool off) { return off ? offhandDamage : hotbarDamage[selected]; }

    // LivingEntity.setLivingEntityFlag: synched data, dirty only on a change.
    MCP_HD void setFlags(bool usingFlag, bool offFlag) {
        if (flagUsing != usingFlag || flagOffhand != offFlag) {
            flagsDirty = true;
            dataDirty = true;
        }
        flagUsing = usingFlag;
        flagOffhand = offFlag;
    }

    // LivingEntity.startUsingItem (server): only a BLOCKS_ATTACKS item is used in scope.
    MCP_HD void startUsingItem(bool off) {
        Weapon item = inHand(off);
        if (item == Weapon::Hand || flagUsing) return;
        useItem = item;
        useItemRemaining = ShieldStats::kUseDuration;
        setFlags(true, off);
    }

    // LivingEntity.stopUsingItem (server): clears bit 1 only.
    MCP_HD void stopUsingItem() {
        setFlags(false, flagOffhand);
        useItem = Weapon::Hand;
        useItemRemaining = 0;
    }

    // LivingEntity.getItemBlockingWith != null (BlocksAttacks.blockDelayTicks = round(0.25 * 20)).
    MCP_HD bool blocking() const {
        if (!flagUsing || !stats(useItem).blocksAttacks) return false;
        int32_t delay = static_cast<int32_t>(::floorf(ShieldStats::kBlockDelaySeconds * 20.0F + 0.5F));  // Math.round(float)
        return ShieldStats::kUseDuration - useItemRemaining >= delay;
    }

    // LivingEntity.getTicksUsingItem
    MCP_HD int32_t ticksUsingItem() const {
        return flagUsing ? (stats(useItem).blocksAttacks ? ShieldStats::kUseDuration : 0) - useItemRemaining : 0;
    }

    // ItemCooldowns.getCooldownPercent(shield, 0)
    MCP_HD float shieldCooldownPercent() const {
        if (shieldCooldown <= 0) return 0.0F;
        return mth::clamp(static_cast<float>(shieldCooldown) / static_cast<float>(shieldCooldownDuration), 0.0F, 1.0F);
    }

    // Player.getAttackStrengthScale
    MCP_HD float attackStrengthScale(float a) const {
        float delay = attackStrengthDelay(attrWeapon);
        return mth::clamp((static_cast<float>(attackStrengthTicker) + a) / delay, 0.0F, 1.0F);
    }
};

// What the server sends back to one player's client during one step, in order.
struct ServerReplies {
    // ClientboundCooldownPacket (the shield's group)
    bool cooldown = false;
    int32_t cooldownTicks = 0;
    // The living-entity flags in ClientboundSetEntityDataPacket: this player's own
    // (LocalPlayer.onSyncedDataUpdated) and the other player's (its RemotePlayer).
    bool useFlags = false, flagUsing = false, flagOffhand = false;
    bool viewFlags = false, viewUsing = false;
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
    int32_t missTime = 0;  // Minecraft.missTime: clicks are eaten while positive
    HitResult pick{};      // this tick's crosshair pick
    // MultiPlayerGameMode block-breaking state (a tap on a block starts and aborts mining)
    bool isDestroying = false;
    int32_t destroyX = 0, destroyY = 0, destroyZ = 0;
    int64_t destroyStartTick = -1, tickCount = 0;
    Vec3 pickFrom{};  // eye position of the last pick
    bool lastAttackHeld = false;  // the attack key state sampled last tick
    // Hotbar selection (Inventory.selected) and the slot last sent to the server
    // (MultiPlayerGameMode.carriedIndex); Minecraft.rightClickDelay; the use key last tick.
    int32_t selected = 0, carriedIndex = 0, rightClickDelay = 0;
    bool lastUseDown = false;
    // LocalPlayer's own use state (startedUsingItem, usingItemHand, the item).
    bool usingItem = false, useOffhand = false;
    Weapon useItem = Weapon::Hand;
    int32_t useItemRemaining = 0;
    // The client's ItemCooldowns (shield group), fed by ClientboundCooldownPacket.
    int32_t cooldown = 0, cooldownDuration = 0;
    bool viewUsing = false;  // the other player's use flag as this client sees it
    ServerCopy server;
    EntityTracker tracker;  // the server's tracker for this player (ServerEntity)
    ClientSendState send;
    ClientPackets packets;
    ServerReplies replies;
    // The other player as this client sees it (RemotePlayer), the tracker
    // packet about it waiting for delivery, and the kinds delivered before
    // this tick (MoveKind bits, 16 for the AddEntity packet).
    RemoteView view;
    EntityMove viewInbox;
    int32_t viewRecv = 0;
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

// The block a pick hit: the cube the hit location lies in, stepped back from
// the face it entered through (BlockHitResult.getBlockPos). Full cubes only.
MCP_HD inline void hitBlock(const HitResult& h, const Vec3& from, int32_t& x, int32_t& y, int32_t& z) {
    // The location is on the entered face; nudge it along the ray into the block.
    double dx = h.location.x - from.x, dy = h.location.y - from.y, dz = h.location.z - from.z;
    x = mth::floor(h.location.x + dx * 1.0E-9);
    y = mth::floor(h.location.y + dy * 1.0E-9);
    z = mth::floor(h.location.z + dz * 1.0E-9);
}

MCP_HD inline bool sameBlock(const DuelPlayer& me, const HitResult& h) {
    int32_t x, y, z;
    hitBlock(h, me.pickFrom, x, y, z);
    return x == me.destroyX && y == me.destroyY && z == me.destroyZ;
}

// MultiPlayerGameMode.startDestroyBlock (survival, never an instant break in scope).
MCP_HD inline void startDestroyBlock(DuelPlayer& me, const HitResult& h) {
    if (me.isDestroying && sameBlock(me, h)) return;
    if (me.isDestroying) me.packets.blockActions++;  // ABORT the old target
    hitBlock(h, me.pickFrom, me.destroyX, me.destroyY, me.destroyZ);
    me.packets.blockActions++;  // START_DESTROY_BLOCK
    me.isDestroying = true;
    me.destroyStartTick = me.tickCount;
}

MCP_HD inline void pushAction(ClientPackets& out, ActionKind kind, int32_t arg = 0, float yRot = 0.0F, float xRot = 0.0F) {
    if (out.actionCount >= 16) return;  // cannot happen: at most ~10 per tick
    out.actions[out.actionCount++] = ActionPacket{kind, static_cast<int8_t>(arg), yRot, xRot};
}

// The client's copy of its items is the server's: nothing but death changes them in scope.
MCP_HD inline Weapon clientInHand(const DuelPlayer& me, bool off) {
    return off ? me.server.offhand : me.server.hotbar[me.selected];
}

// MultiPlayerGameMode.ensureHasSentCarriedItem
MCP_HD inline void ensureHasSentCarriedItem(DuelPlayer& me) {
    if (me.selected != me.carriedIndex) {
        me.carriedIndex = me.selected;
        pushAction(me.packets, ActionKind::Carried, me.selected);
    }
}

// LocalPlayer.startUsingItem / stopUsingItem (the client's prediction)
MCP_HD inline void clientStartUsingItem(DuelPlayer& me, bool off) {
    Weapon item = clientInHand(me, off);
    if (item == Weapon::Hand || me.usingItem) return;
    me.usingItem = true;
    me.useOffhand = off;
    me.useItem = item;
    me.useItemRemaining = ShieldStats::kUseDuration;
}

MCP_HD inline void clientStopUsingItem(DuelPlayer& me) {
    me.usingItem = false;
    me.useItem = Weapon::Hand;
    me.useItemRemaining = 0;
}

// Minecraft.startUseItem. Per hand: the entity or block under the crosshair is
// interacted with first (no sword, axe or shield interaction does anything to a
// player, grass or a barrier, so those packets change nothing), then the item is
// used: MultiPlayerGameMode.useItem always sends the packet; a shield off
// cooldown is raised (CONSUME, which ends the loop), anything else passes.
MCP_HD inline void startUseItem(DuelPlayer& me, const DuelInput& in) {
    if (me.isDestroying) return;  // gameMode.isDestroying()
    me.rightClickDelay = 4;
    for (int32_t hand = 0; hand < 2; ++hand) {
        bool off = hand == 1;
        if (me.pick.type == HitType::Entity) {
            // isWithinEntityInteractionRange(entity, 0.0) holds for any entity pick
            ensureHasSentCarriedItem(me);
            pushAction(me.packets, ActionKind::Interact, hand);
        } else if (me.pick.type == HitType::Block) {
            ensureHasSentCarriedItem(me);
            pushAction(me.packets, ActionKind::UseItemOn, hand);
        }
        Weapon item = clientInHand(me, off);
        if (item == Weapon::Hand) continue;
        ensureHasSentCarriedItem(me);
        pushAction(me.packets, ActionKind::UseItem, hand, in.yaw, in.pitch);
        if (stats(item).blocksAttacks && !(me.cooldown > 0)) {  // Item.use -> startUsingItem, CONSUME
            clientStartUsingItem(me, off);
            return;
        }
    }
}

// MultiPlayerGameMode.releaseUsingItem
MCP_HD inline void releaseUsingItem(DuelPlayer& me) {
    ensureHasSentCarriedItem(me);
    pushAction(me.packets, ActionKind::Release);
    clientStopUsingItem(me);  // LivingEntity.releaseUsingItem: a shield's releaseUsing does nothing
}

// One client tick (Minecraft.tick): pick, keybinds (click), movement,
// sendChanges. `target` is the opponent's box as this client last heard it.
template <typename World>
MCP_HD void clientTick(DuelPlayer& me, const DuelInput& in, const AABB& target, const World& w, const float* sinTab) {
    ClientPackets& out = me.packets;
    out = ClientPackets{};
    me.sentAttack = false;
    Player& cp = me.client;
    // A human camera cannot look past straight up or down (Entity.turn clamps);
    // the fairness gate guarantees it, so anything else is a bug upstream.
    if (!(in.pitch >= -90.0F && in.pitch <= 90.0F) || !(in.yaw == in.yaw)) cp.unsupported = true;
    cp.yRot = in.yaw;
    cp.xRot = in.pitch;
    PickView view{cp.xo, cp.yo, cp.zo, cp.x, cp.y, cp.z, CombatConstants::kEyeHeightStanding, cp.xRot, cp.yRot,
                  cp.boundingBox()};
    me.pick = clientPick(view, target, w, sinTab);
    if (in.rangeHit && me.pick.type != HitType::Entity) {
        // Cheat: resolve the click against the box's nearest point instead of the ray.
        double ex = pick::lerp(1.0, cp.xo, cp.x), ey = pick::lerp(1.0, cp.yo, cp.y) + static_cast<double>(view.eyeHeight),
               ez = pick::lerp(1.0, cp.zo, cp.z);
        double nx = mth::clamp(ex, target.minX, target.maxX), ny = mth::clamp(ey, target.minY, target.maxY),
               nz = mth::clamp(ez, target.minZ, target.maxZ);
        double dd = (nx - ex) * (nx - ex) + (ny - ey) * (ny - ey) + (nz - ez) * (nz - ez);
        double r = static_cast<double>(PickConstants::kEntityInteractionRange);
        if (dd < r * r) me.pick = HitResult{HitType::Entity, Vec3{nx, ny, nz}};
    }
    me.pickFrom = Vec3{pick::lerp(1.0, cp.xo, cp.x), pick::lerp(1.0, cp.yo, cp.y) + static_cast<double>(view.eyeHeight),
                       pick::lerp(1.0, cp.zo, cp.z)};
    me.tickCount++;
    // Minecraft.tick: rightClickDelay--, gameMode.tick (sends a hotbar change made
    // last tick), then handleKeybinds.
    if (me.rightClickDelay > 0) me.rightClickDelay--;
    ensureHasSentCarriedItem(me);
    // handleKeybinds: hotbar keys; then, unless an item is in use (which eats the
    // clicks and releases the item once the use key is up), startAttack for the
    // click and startUseItem for a use press; a held use key retries every
    // rightClickDelay; then continueAttack(key down).
    if (in.slot >= 0 && in.slot < 9) me.selected = in.slot;
    bool usePressed = in.use && !me.lastUseDown;
    me.lastUseDown = in.use;
    if (me.usingItem) {
        if (!in.use) releaseUsingItem(me);
    } else {
        if (in.attack && me.missTime <= 0) {
            switch (me.pick.type) {
                case HitType::Entity:  // MultiPlayerGameMode.attack
                    ensureHasSentCarriedItem(me);
                    pushAction(out, ActionKind::Attack);
                    out.attack = true;
                    me.clientAttackStrengthTicker = 0;
                    me.sentAttack = true;
                    break;
                case HitType::Block:  // startDestroyBlock does not send the carried slot
                    startDestroyBlock(me, me.pick);
                    break;
                case HitType::Miss:
                    me.missTime = 10;  // survival has miss time
                    me.clientAttackStrengthTicker = 0;
                    break;
            }
            pushAction(out, ActionKind::Punch);
            out.punches++;
        }
        if (usePressed) startUseItem(me, in);
    }
    if (in.use && me.rightClickDelay == 0 && !me.usingItem) startUseItem(me, in);
    // continueAttack
    if (!in.attackHeld) me.missTime = 0;
    if (me.missTime <= 0 && !me.usingItem) {
        if (in.attackHeld && me.pick.type == HitType::Block) {
            ensureHasSentCarriedItem(me);  // continueDestroyBlock
            if (me.isDestroying && sameBlock(me, me.pick)) {
                // continueDestroyBlock: mining progress beyond the tick it started is not ported
                if (me.destroyStartTick != me.tickCount) cp.unsupported = true;
            } else {
                startDestroyBlock(me, me.pick);
            }
            pushAction(out, ActionKind::Punch);
            out.punches++;
        } else if (me.isDestroying) {  // stopDestroyBlock
            out.blockActions++;
            me.isDestroying = false;
            me.clientAttackStrengthTicker = 0;
        }
    }
    if (me.missTime > 0) me.missTime--;
    me.lastAttackHeld = in.attackHeld;
    // LivingEntity.tick before the movement: updatingUsingItem stops a use whose
    // item left the hand (a main-hand item swapped away), else counts it down.
    if (me.usingItem) {
        if (clientInHand(me, me.useOffhand) == me.useItem) me.useItemRemaining--;
        else clientStopUsingItem(me);
    }
    me.client.usingItem = me.usingItem;
    tick(me.client, in.keys, in.yaw, in.pitch, w, sinTab);
    me.clientAttackStrengthTicker++;
    if (me.cooldown > 0) me.cooldown--;  // Player.tick -> cooldowns.tick()

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

// The remote player's hitbox as a client sees it (EntityDimensions.makeBoundingBox).
MCP_HD inline AABB viewBox(const RemoteView& v) {
    double w = static_cast<double>(PlayerConstants::kHalfWidth);
    return AABB{v.pos.x - w, v.pos.y, v.pos.z - w, v.pos.x + w, v.pos.y + static_cast<double>(PlayerConstants::kHeightStanding),
                v.pos.z + w};
}

// ClientPacketListener: apply what the server sent (before the next client tick).
MCP_HD inline void deliver(DuelPlayer& me) {
    ServerReplies& r = me.replies;
    me.gotVelocity = false;
    me.viewRecv = static_cast<int32_t>(me.viewInbox.kind);
    me.view.receive(me.viewInbox);
    me.viewInbox = EntityMove{};
    if (r.motion) {
        me.client.vel = lpvec3::roundTrip(r.motionVelocity);  // Entity.lerpMotion
        me.gotVelocity = true;
    }
    if (r.cooldown) {  // ClientPacketListener.handleItemCooldown
        me.cooldown = r.cooldownTicks;
        me.cooldownDuration = r.cooldownTicks;
    }
    if (r.useFlags) {  // LocalPlayer.onSyncedDataUpdated: follow the server's use state
        if (r.flagUsing && !me.usingItem) clientStartUsingItem(me, r.flagOffhand);
        else if (!r.flagUsing && me.usingItem) clientStopUsingItem(me);
    }
    if (r.viewFlags) me.viewUsing = r.viewUsing;
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
    sp.needsSync = true;
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
    victim.needsSync = true;
    while (xd * xd + zd * zd < static_cast<double>(1.0E-5F)) {
        // Vanilla picks a random direction from the victim's RandomSource: same
        // distribution here, not the same numbers.
        auto nextDouble = [&victim]() { return static_cast<double>(victim.rng.next() >> 11) * 0x1.0p-53; };
        xd = (nextDouble() - nextDouble()) * 0.01;
        zd = (nextDouble() - nextDouble()) * 0.01;
        victim.randomKnockbacks++;
    }
    Vec3 dv = Vec3{xd, 0.0, zd}.normalize().scale(power);
    const Vec3& m = victim.body.vel;
    double vy = victim.body.onGround ? ::fmin(0.4, m.y / 2.0 + power) : m.y;  // Math.min: operands are never NaN here
    victim.body.vel = Vec3{m.x / 2.0 - dv.x, vy, m.z / 2.0 - dv.z};
}

// LivingEntity.applyItemBlocking for a melee hit (the source position is the
// attacker's), with Player.blockUsingItem's disable. Returns the damage blocked.
MCP_HD inline float applyItemBlocking(ServerCopy& victim, const ServerCopy& attacker, float damage,
                                      ServerReplies& victimReplies, const float* sinTab) {
    if (damage <= 0.0F || !victim.blocking()) return 0.0F;
    // angle = acos(horizontal direction to the source . calculateViewVector(0, yHeadRot))
    Vec3 to = Vec3{attacker.body.x - victim.body.x, 0.0, attacker.body.z - victim.body.z}.normalize();
    float realYRot = -victim.yHeadRot * static_cast<float>(3.141592653589793 / 180.0);
    float ySin = mth::sin(sinTab, static_cast<double>(realYRot)), yCos = mth::cos(sinTab, static_cast<double>(realYRot));
    float xCos = mth::cos(sinTab, 0.0), xSin = mth::sin(sinTab, 0.0);
    Vec3 view{static_cast<double>(ySin * xCos), static_cast<double>(-xSin), static_cast<double>(yCos * xCos)};
    double angle = ::acos(to.x * view.x + to.y * view.y + to.z * view.z);
    // BlocksAttacks.resolveBlockedDamage: one DamageReduction(90, any type, 0, 1)
    float blocked = 0.0F;
    if (!(angle > static_cast<double>(static_cast<float>(3.141592653589793 / 180.0) * ShieldStats::kHorizontalBlockingAngle)))
        blocked += mth::clamp(0.0F + 1.0F * damage, 0.0F, damage);
    blocked = mth::clamp(blocked, 0.0F, damage);
    // hurtBlockingItem: the shield's durability
    int32_t itemDamage = blocked < ShieldStats::kItemDamageThreshold
        ? 0 : mth::floor(static_cast<double>(ShieldStats::kItemDamageBase + ShieldStats::kItemDamageFactor * blocked));
    if (itemDamage > 0) {
        int32_t& used = victim.damageOf(victim.flagOffhand);
        used += itemDamage;
        if (used >= stats(victim.useItem).durability) victim.body.unsupported = true;  // breaking not ported
    }
    if (blocked > 0.0F) {
        // blockUsingItem: attacker.blockedByItem shoves the defender only on a
        // partial block (impossible with a shield); then the disable, when the
        // attacker's weapon item is its active item (not using anything).
        float seconds = attacker.flagUsing ? 0.0F : stats(attacker.mainHand()).disableSeconds;
        if (seconds > 0.0F) {
            float scaled = seconds * ShieldStats::kDisableCooldownScale;
            int32_t ticks = scaled > 0.0F ? static_cast<int32_t>(::floorf(scaled * 20.0F + 0.5F)) : 0;  // Math.round
            if (ticks > 0) {
                victim.shieldCooldown = ticks;
                victim.shieldCooldownDuration = ticks;
                victimReplies.cooldown = true;  // ServerItemCooldowns.onCooldownStarted
                victimReplies.cooldownTicks = ticks;
                victim.stopUsingItem();
            }
        }
    }
    return blocked;
}

// LivingEntity.hurtServer for a player hit by a player's melee attack.
MCP_HD inline bool hurt(ServerCopy& victim, const ServerCopy& attacker, float damage, ServerReplies& victimReplies,
                        const float* sinTab) {
    if (victim.health <= 0.0F) return false;  // isDeadOrDying
    if (damage == 0.0F) return false;         // Player.hurtServer
    float damageBlocked = applyItemBlocking(victim, attacker, damage, victimReplies, sinTab);
    damage -= damageBlocked;
    bool blocked = damageBlocked > 0.0F;
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
    if (dealt != 0.0F) {  // setHealth: the synched value is dirty only if it changed
        float h = mth::clamp(victim.health - dealt, 0.0F, CombatConstants::kMaxHealth);
        if (h != victim.health) victim.dataDirty = true;
        victim.health = h;
    }
    if (tookFullDamage) victim.hurtTime = CombatConstants::kHurtDuration;
    if (tookFullDamage) {
        // Blocked: onBlocked (a sound) instead of the damage event; no markHurt
        // and no knockback when fully blocked.
        if (!blocked || damage > 0.0F) victim.syncVelocity = true;  // markHurt
        bool fullyBlocked = blocked && damage <= 0.0F;
        // dealDefaultKnockback: from the damage source's position (the attacker).
        if (!fullyBlocked)
            knockback(victim, static_cast<double>(0.4F), attacker.body.x - victim.body.x, attacker.body.z - victim.body.z);
    }
    if (victim.health <= 0.0F) {  // no totem in scope: ServerPlayer.die
        victim.dead = true;
        // dropAllDeathLoot empties the inventory; the item change resets the
        // attack strength at the dead player's next Player.tick (still this step).
        for (int32_t k = 0; k < 9; ++k) {
            victim.hotbar[k] = Weapon::Hand;
            victim.hotbarDamage[k] = 0;
        }
        victim.offhand = Weapon::Hand;
        victim.offhandDamage = 0;
    }
    return !blocked || damage > 0.0F;
}

// Player.attack (bare hand) followed by causeExtraKnockback.
MCP_HD inline void attack(ServerCopy& a, ServerCopy& target, ServerReplies& targetReplies, const float* sinTab) {
    // The attributes are those of the item held at the last server tick; the
    // weapon item (durability, the shield disable) is the one in hand now.
    float baseDamage = static_cast<float>(attackDamageAttribute(a.attrWeapon));
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
    if (!hurt(target, a, totalDamage, targetReplies, sinTab)) return;
    // itemAttackInteraction: the weapon loses durability for the entity hit.
    Weapon held = a.mainHand();
    if (stats(held).damagePerHit > 0) {
        a.hotbarDamage[a.selected] += stats(held).damagePerHit;
        if (a.hotbarDamage[a.selected] >= stats(held).durability) a.body.unsupported = true;  // breaking not ported
    }
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
// LivingEntity.isPushable for a server copy in a ticking arena chunk:
// alive, not a spectator, not on a climbable.
MCP_HD inline bool pushable(const ServerCopy& s) { return s.health > 0.0F; }

// Entity.push(Entity pusher) called on `target` (LivingEntity.doPush).
MCP_HD inline void push(ServerCopy& target, ServerCopy& pusher) {
    double xa = pusher.body.x - target.body.x;
    double za = pusher.body.z - target.body.z;
    double dd = ::fmax(::fabs(xa), ::fabs(za));  // Mth.absMax
    if (!(dd >= static_cast<double>(0.01F))) return;
    dd = ::sqrt(dd);
    xa /= dd;
    za /= dd;
    double pow = 1.0 / dd;
    if (pow > 1.0) pow = 1.0;
    xa *= pow;
    za *= pow;
    xa *= static_cast<double>(0.05F);
    za *= static_cast<double>(0.05F);
    if (pushable(target)) {  // Entity.push(x, y, z) also sets needsSync
        target.body.vel = target.body.vel.add(-xa, 0.0, -za);
        target.needsSync = true;
    }
    if (pushable(pusher)) {
        pusher.body.vel = pusher.body.vel.add(xa, 0.0, za);
        pusher.needsSync = true;
    }
}

// LivingEntity.pushEntities on the server: every pushable entity whose box
// intersects this one (EntitySelector.pushableBy, no teams in scope).
MCP_HD inline void pushEntities(ServerCopy& self, ServerCopy& other) {
    if (!pushable(other)) return;
    AABB a = self.body.boundingBox(), b = other.body.boundingBox();
    if (!pick::intersects(b, a)) return;
    push(other, self);  // doPush(entity) -> entity.push(this)
}

template <typename World>
MCP_HD void serverTickPlayer(ServerCopy& sp, const World& w, ServerCopy* other = nullptr) {
    using C = PlayerConstants;
    Player& b = sp.body;
    double firstGoodX = b.x, firstGoodY = b.y, firstGoodZ = b.z;  // resetPosition
    sp.lastGoodX = b.x;
    sp.lastGoodY = b.y;
    sp.lastGoodZ = b.z;
    // LivingEntity.baseTick
    if (sp.hurtTime > 0) sp.hurtTime--;
    // LivingEntity.tick: updatingUsingItem, then detectEquipmentUpdates
    if (sp.flagUsing) {
        if (sp.inHand(sp.flagOffhand) == sp.useItem) sp.useItemRemaining--;  // never reaches 0 in an episode
        else sp.stopUsingItem();
    }
    sp.attrWeapon = sp.mainHand();
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
    // End of aiStep: push the other player, from where travel just moved this copy.
    if (other != nullptr) pushEntities(sp, *other);
    sp.yHeadRot = b.yRot;  // Player.aiStep
    // Player.tick
    sp.attackStrengthTicker++;
    if (sp.mainHand() != sp.lastMainHand) {  // !ItemStack.isSameItem(lastItemInMainHand, mainHand)
        sp.attackStrengthTicker = 0;
        sp.lastMainHand = sp.mainHand();
    }
    if (sp.shieldCooldown > 0) sp.shieldCooldown--;  // cooldowns.tick()
    // absSnapTo(firstGood)
    b.x = firstGoodX;
    b.y = firstGoodY;
    b.z = firstGoodZ;
}

}  // namespace duel

// Where a player starts an episode (on the arena floor).
struct DuelStart {
    double x = 0.5, z = 0.5;
    float yaw = 0.0F;
    float health = CombatConstants::kMaxHealth;
    Weapon hotbar[9] = {};  // slot 0 is selected
    Weapon offhand = Weapon::Hand;
};

// A zero-latency duel, stepped exactly like the oracle harness.
struct Duel {
    DuelPlayer p[2];

    // Place both players and run the login handshake (three server ticks).
    template <typename World>
    MCP_HD void spawn(int32_t i, double x, double y, double z, float yaw, const World& w,
                      float health = CombatConstants::kMaxHealth, Weapon weapon = Weapon::Hand,
                      const Weapon* hotbar = nullptr, Weapon offhand = Weapon::Hand) {
        DuelPlayer& d = p[i];
        d = DuelPlayer{};
        d.client.x = d.client.xo = x;
        d.client.y = d.client.yo = y;
        d.client.z = d.client.zo = z;
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
        s.health = health;
        // Equipped at login: the first Player.tick applies the modifiers and resets the strength.
        for (int32_t k = 0; k < 9; ++k) s.hotbar[k] = hotbar != nullptr ? hotbar[k] : Weapon::Hand;
        if (hotbar == nullptr) s.hotbar[0] = weapon;
        s.offhand = offhand;
        for (int32_t k = 0; k < 3; ++k) duel::serverTickPlayer(s, w);
        // The tracker starts when the player joins (not yet on the ground, the
        // yaw not yet wrapped by the teleport acknowledgement). The first two
        // handshake ticks reach nobody: the other client is paired after the
        // second one (see pairViews).
        Vec3 pos{x, y, z};
        d.tracker.start(pos, yaw, 0.0F, false);
        d.tracker.sendChanges(pos, yaw, 0.0F, false, false, false);
        d.tracker.sendChanges(pos, s.body.yRot, s.body.xRot, false, false, false);
    }

    // The end of the login handshake for the views: each client gets the other
    // player's ClientboundAddEntityPacket, then the third tick's tracker packet
    // (a position sync, since the server copy has just landed). Call after
    // spawning both players.
    MCP_HD void pairViews() {
        for (int32_t i = 0; i < 2; ++i) {
            DuelPlayer& viewer = p[1 - i];
            EntityTracker& tr = p[i].tracker;
            const Player& b = p[i].server.body;
            viewer.view.add(tr.base, tr.lastSentYRot, tr.lastSentXRot);
            viewer.viewInbox = tr.sendChanges(Vec3{b.x, b.y, b.z}, b.yRot, b.xRot, b.onGround, false, false);
            duel::deliver(viewer);
        }
    }

    template <typename World>
    MCP_HD void step(const DuelInput& a, const DuelInput& b, const World& w, const float* sinTab) {
        // Each client aims at its view of the other player, then ticks its
        // entities: itself first, then the remote player (ClientLevel's tick
        // list is in insertion order).
        for (int32_t i = 0; i < 2; ++i) {
            const DuelInput& in = i == 0 ? a : b;
            // A range-hit cheat aims at the true position; everyone else at their view.
            AABB target = in.rangeHit ? p[1 - i].client.boundingBox() : duel::viewBox(p[i].view);
            duel::clientTick(p[i], in, target, w, sinTab);
            p[i].view.clientTick();
        }
        // Server: each client's packets in send order, then one server tick.
        for (int32_t i = 0; i < 2; ++i) {
            DuelPlayer& me = p[i];
            DuelPlayer& other = p[1 - i];
            const ClientPackets& pk = me.packets;
            // Handlers that require hasClientLoaded() skip a player who died earlier
            // in this step; handlePunch does not check it.
            bool loaded = !me.server.dead;
            ServerCopy& sv = me.server;
            for (int32_t k = 0; k < pk.actionCount; ++k) {
                const ActionPacket& ap = pk.actions[k];
                switch (ap.kind) {
                    case ActionKind::Carried:  // handleSetCarriedItem (no hasClientLoaded check)
                        if (sv.selected != ap.arg && sv.flagUsing && !sv.flagOffhand) sv.stopUsingItem();
                        sv.selected = ap.arg;
                        break;
                    case ActionKind::Attack:
                        if (loaded) duel::handleAttack(sv, other.server, other.replies, sinTab);
                        break;
                    case ActionKind::Punch:  // handlePunch -> resetAttackStrengthTicker
                        sv.attackStrengthTicker = 0;
                        break;
                    case ActionKind::Interact:   // Player.interactOn: PASS for these items and a player
                    case ActionKind::UseItemOn:  // no block in the arena reacts to these items
                        break;
                    case ActionKind::UseItem: {  // handleUseItem
                        Weapon item = sv.inHand(ap.arg == 1);
                        if (!loaded || item == Weapon::Hand) break;
                        float ty = duel::wrapDegrees(ap.yRot), tx = duel::wrapDegrees(ap.xRot);
                        if (tx != sv.body.xRot || ty != sv.body.yRot) {  // absSnapRotationTo
                            sv.body.yRot = ::fmodf(ty, 360.0F);
                            sv.body.xRot = ::fmodf(mth::clamp(tx, -90.0F, 90.0F), 360.0F);
                        }
                        // ServerPlayerGameMode.useItem: PASS on cooldown; a shield is raised
                        // (CONSUME: no swing); anything else passes.
                        if (stats(item).blocksAttacks && !(sv.shieldCooldown > 0)) sv.startUsingItem(ap.arg == 1);
                        break;
                    }
                    case ActionKind::Release:  // handlePlayerAction RELEASE_USE_ITEM -> releaseUsingItem
                        if (loaded) sv.stopUsingItem();
                        break;
                }
            }
            if (pk.input && pk.inputKeys.shift && loaded) me.server.body.unsupported = true;  // server-side sneaking not ported
            if (pk.sprintCommand != 0 && loaded) me.server.setSprinting(pk.sprintCommand > 0);
            if (pk.move && loaded) duel::handleMove(me.server, pk.movePacket, w, sinTab);
        }
        // MinecraftServer.tickServer: ServerEntity.sendChanges sends the other
        // client a move packet when due and echoes dirty entity data (and with
        // it dirty attributes) to the player itself...
        for (int32_t i = 0; i < 2; ++i) {
            ServerCopy& s = p[i].server;
            ServerReplies& r = p[i].replies;
            p[1 - i].viewInbox = p[i].tracker.sendChanges(Vec3{s.body.x, s.body.y, s.body.z}, s.body.yRot, s.body.xRot,
                                                          s.body.onGround, s.needsSync, s.sprintFlagDirty || s.dataDirty);
            s.needsSync = false;
            s.dataDirty = false;
            if (s.flagsDirty) {  // the living-entity flags, to the player and to its trackers
                r.useFlags = true;
                r.flagUsing = s.flagUsing;
                r.flagOffhand = s.flagOffhand;
                p[1 - i].replies.viewFlags = true;
                p[1 - i].replies.viewUsing = s.flagUsing;
                s.flagsDirty = false;
            }
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
        // Only the server copies push each other: a client is never pushed by
        // the remote player it sees (Entity.push skips the noPhysics
        // RemotePlayer, and a client's pushableBy only admits the local player).
        for (int32_t i = 0; i < 2; ++i) duel::serverTickPlayer(p[i].server, w, &p[1 - i].server);
    }

    // Apply the server's replies; call after reading the tick's state.
    MCP_HD void deliver() {
        duel::deliver(p[0]);
        duel::deliver(p[1]);
    }

    // Start a new episode: both players respawn from scratch (no state carries over).
    template <typename World>
    MCP_HD void reset(const DuelStart& a, const DuelStart& b, const World& w, uint64_t seed = 0) {
        double y = static_cast<double>(w.surfaceY);
        spawn(0, a.x, y, a.z, a.yaw, w, a.health, a.hotbar[0], a.hotbar, a.offhand);
        spawn(1, b.x, y, b.z, b.yaw, w, b.health, b.hotbar[0], b.hotbar, b.offhand);
        pairViews();
        p[0].server.rng = SplitMix64{seed * 2 + 1};
        p[1].server.rng = SplitMix64{seed * 2 + 2};
    }

    // Draws that cannot match vanilla bit for bit (see ServerCopy::rng).
    MCP_HD int32_t nonParityEvents() const { return p[0].server.randomKnockbacks + p[1].server.randomKnockbacks; }

    // Episode end: a death (the killing tick is complete once step() returns).
    MCP_HD bool done() const { return p[0].server.dead || p[1].server.dead; }
    // 0 or 1 for the surviving player, -1 while nobody (or, impossibly, both) died.
    MCP_HD int32_t winner() const {
        if (p[0].server.dead == p[1].server.dead) return -1;
        return p[0].server.dead ? 1 : 0;
    }

    MCP_HD bool unsupported() const {
        return p[0].client.unsupported || p[1].client.unsupported || p[0].server.body.unsupported ||
               p[1].server.body.unsupported || p[0].view.unsupported || p[1].view.unsupported;
    }
};

}  // namespace mcp
