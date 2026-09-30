package mcporl.oracle;

import com.mojang.authlib.GameProfile;
import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import io.netty.channel.ChannelHandler;
import io.netty.channel.embedded.EmbeddedChannel;
import java.io.IOException;
import java.io.Writer;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.UUID;
import java.util.function.BooleanSupplier;
import net.minecraft.network.Connection;
import net.minecraft.network.LpVec3;
import net.minecraft.network.protocol.Packet;
import net.minecraft.network.protocol.PacketFlow;
import net.minecraft.network.protocol.game.ClientboundAddEntityPacket;
import net.minecraft.network.protocol.game.ClientboundBundlePacket;
import net.minecraft.network.protocol.game.ClientboundChunkBatchFinishedPacket;
import net.minecraft.network.protocol.game.ClientboundEntityPositionSyncPacket;
import net.minecraft.network.protocol.game.ClientboundMoveEntityPacket;
import net.minecraft.network.protocol.game.ClientboundPlayerPositionPacket;
import net.minecraft.network.protocol.game.ClientboundSetEntityDataPacket;
import net.minecraft.network.protocol.game.ClientboundSetEntityMotionPacket;
import net.minecraft.network.protocol.game.ClientboundUpdateAttributesPacket;
import net.minecraft.network.protocol.game.ServerboundAcceptTeleportationPacket;
import net.minecraft.network.protocol.game.ServerboundAttackPacket;
import net.minecraft.network.protocol.game.ServerboundChunkBatchReceivedPacket;
import net.minecraft.network.protocol.game.ServerboundClientTickEndPacket;
import net.minecraft.network.protocol.game.ServerboundMovePlayerPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerCommandPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerInputPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerActionPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerLoadedPacket;
import net.minecraft.network.protocol.game.ServerboundPunchPacket;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.CommonListenerCookie;
import net.minecraft.util.Mth;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntitySelector;
import net.minecraft.world.entity.EquipmentSlot;
import net.minecraft.world.item.Item;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.entity.PositionMoveRotation;
import net.minecraft.world.entity.PositionPath;
import net.minecraft.network.protocol.game.VecDeltaCodec;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.BlockHitResult;
import net.minecraft.world.phys.EntityHitResult;
import net.minecraft.world.phys.HitResult;
import net.minecraft.world.entity.ai.attributes.AttributeInstance;
import net.minecraft.world.entity.ai.attributes.AttributeModifier;
import net.minecraft.world.entity.ai.attributes.Attributes;
import net.minecraft.world.entity.player.Input;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.gamerules.GameRules;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.phys.Vec3;

/**
 * Zero-latency two-player combat oracle (docs/ARCHITECTURE.md, "Two-player
 * combat oracle"). Each player is a client copy (OraclePlayer, owns movement)
 * plus a real ServerPlayer on a mock connection. The harness only carries
 * packets between them, in the order the real client and server use; all game
 * logic is vanilla.
 */
public final class CombatOracle {

    /** One player's client copy, its server twin, and the client-side send state from LocalPlayer. */
    static final class Side {
        final String name;
        final OraclePlayer client;
        final ServerPlayer server;
        final EmbeddedChannel channel;
        final List<Packet<?>> toServer = new ArrayList<>();
        final List<Packet<?>> toClient = new ArrayList<>();
        final Map<String, Integer> ignoredToClient = new TreeMap<>();
        // LocalPlayer.sendPosition / sendChanges state
        double xLast, yLast, zLast;
        float yRotLast, xRotLast;
        boolean lastOnGround, lastHorizontalCollision, wasSprinting;
        int positionReminder;
        Input lastSentInput = Input.EMPTY;
        // Minecraft.missTime: clicks are ignored while it is positive (set by a whiff)
        int missTime;
        HitResult pick;  // this tick's crosshair pick
        // MultiPlayerGameMode block-breaking state
        boolean isDestroying;
        BlockPos destroyBlockPos;
        int destroyStartTick = -1, sequence, tick;
        // per-tick trace flags
        boolean gotVelocity, sentAttack;
        int teleports;  // server position corrections applied during the scenario
        // The other player as this client sees it (RemotePlayer), created by the
        // tracker's ClientboundAddEntityPacket, and the movement packets for it
        // handled before this tick (bits: 1 Pos, 2 PosRot, 4 Rot, 8 PositionSync, 16 AddEntity).
        RemoteView view;
        int viewRecv;

        Side(String name, OraclePlayer client, ServerPlayer server, EmbeddedChannel channel) {
            this.name = name;
            this.client = client;
            this.server = server;
            this.channel = channel;
        }
    }

    static final String[] FIELDS = {
            "pos.x:f64", "pos.y:f64", "pos.z:f64", "vel.x:f64", "vel.y:f64", "vel.z:f64",
            "yRot:f32", "onGround:bool", "sprinting:bool",
            "server.sprinting:bool", "server.health:f32", "server.hurtTime:i32", "server.damageCooldown:i32",
            "server.onGround:bool", "server.fallDistance:f64", "server.attackStrength:f32",
            "server.vel.x:f64", "server.vel.y:f64", "server.vel.z:f64", "server.yRot:f32", "speedAttr:f64",
            "pick:i32", "pick.x:f64", "pick.y:f64", "pick.z:f64", "missTime:i32",
            "gotVelocity:bool", "sentAttack:bool", "teleports:i32",
            "view.x:f64", "view.y:f64", "view.z:f64", "view.yRot:f32", "view.recv:i32"};

    private final MinecraftServer server;
    private final ServerLevel level;
    private final Method tickServer;

    CombatOracle(MinecraftServer server) throws Exception {
        this.server = server;
        this.level = server.overworld();
        this.tickServer = MinecraftServer.class.getDeclaredMethod("tickServer", BooleanSupplier.class);
        this.tickServer.setAccessible(true);
    }

    /** Runs one scenario. Must be called on the server thread; nothing else ticks meanwhile. */
    void run(CombatScenario s, Path out) throws Exception {
        // Health regeneration depends on hunger (FoodData), which is not in the
        // simulator's known domain yet; keep it out of the goldens.
        level.getGameRules().set(GameRules.NATURAL_HEALTH_REGENERATION, false, server);
        // A real client keeps every chunk around it loaded (view distance). Without
        // tickets these chunks drop out of FULL status, and Entity.doCheckFallDamage
        // silently skips fall tracking (touchingUnloadedChunk), which disables crits.
        for (int dx = -8; dx <= 8; dx++) for (int dz = -8; dz <= 8; dz++) {
            level.setChunkForced(dx, dz, true);
            level.getChunk(dx, dz);
        }
        Side a = spawn("A", s.a());
        Side b = spawn("B", s.b());
        try (Writer w = Files.newBufferedWriter(out)) {
            w.write(header(s));
            w.write('\n');
            // Login handshake. As over a real network, the client's replies arrive
            // after at least one server tick; the first ServerGamePacketListenerImpl
            // tick (tickPlayer -> resetPosition) initialises the anti-cheat's
            // last-good position before the teleport acknowledgement is handled.
            serverStep(List.of(a, b));
            handshakeDiag(s, 0, a, b);
            deliver(a, b);  // applies the login teleport and queues its acknowledgement
            deliver(b, a);
            for (Side side : List.of(a, b)) side.toServer.add(new ServerboundPlayerLoadedPacket());
            for (int i = 0; i < 2; i++) {
                serverStep(List.of(a, b));
                handshakeDiag(s, i + 1, a, b);
                deliver(a, b);
                deliver(b, a);
            }
            // Vanilla finds nearby entities (pushing, picks) through the level's
            // entity sections, which only answer once the arena chunks are fully
            // ticking. Earlier runs raced this and silently lost player pushing.
            for (Side side : List.of(a, b)) {
                if (!level.getEntities((Entity) null, side.server.getBoundingBox()).contains(side.server)) {
                    throw new IllegalStateException("entity lookup does not see " + side.name + "; arena chunks are not ticking");
                }
            }
            for (Side side : List.of(a, b)) {
                if (side.view == null) {
                    throw new IllegalStateException(side.name + " has not been sent the other player after the handshake");
                }
                System.out.println("[oracle] " + s.name() + " " + side.name + " view after handshake: recv=" + side.viewRecv
                        + " pos=" + side.view.position() + " other=" + (side == a ? b : a).server.position());
                side.teleports = 0;
                side.xLast = side.client.getX();
                side.yLast = side.client.getY();
                side.zLast = side.client.getZ();
            }
            int t = 0;
            for (CombatScenario.Tick tick : s.ticks()) {
                step(a, tick.a(), b.server.getId());
                step(b, tick.b(), a.server.getId());
                serverStep(List.of(a, b));
                requireLoaded(a, t);
                requireLoaded(b, t);
                w.write(line(t++, tick, a, b));
                w.write('\n');
                // The episode ends on the tick a player dies; that tick is traced in full.
                if (a.server.isDeadOrDying() || b.server.isDeadOrDying()) {
                    System.out.println("[oracle] " + s.name() + ": " + (a.server.isDeadOrDying() ? "A" : "")
                            + (b.server.isDeadOrDying() ? "B" : "") + " died at t=" + (t - 1));
                    break;
                }
                deliver(a, b);
                deliver(b, a);
            }
        } finally {
            for (Side side : List.of(a, b)) {
                if (side.teleports > 0) {
                    System.out.println("[oracle] WARNING " + s.name() + " " + side.name + ": " + side.teleports
                            + " server position correction(s); the client copy disagreed with the server");
                }
                server.getPlayerList().remove(side.server);
                if (!side.ignoredToClient.isEmpty()) {
                    System.out.println("[oracle] " + s.name() + " " + side.name + " ignored client packets: " + side.ignoredToClient);
                }
            }
        }
    }

    private Side spawn(String name, CombatScenario.Start start) {
        int bx = Mth.floor(start.x()), bz = Mth.floor(start.z());
        double y = level.getHeight(Heightmap.Types.MOTION_BLOCKING, bx, bz);
        GameProfile profile = new GameProfile(UUID.nameUUIDFromBytes(("oracle-" + name).getBytes()), "oracle" + name);
        CommonListenerCookie cookie = CommonListenerCookie.createInitial(profile, false);
        ServerPlayer sp = new ServerPlayer(server, level, cookie.gameProfile(), cookie.clientInformation());
        sp.snapTo(start.x(), y, start.z(), start.yaw(), 0.0F);
        Connection connection = new Connection(PacketFlow.SERVERBOUND);
        EmbeddedChannel channel = new EmbeddedChannel(new ChannelHandler[] {connection});
        server.getPlayerList().placeNewPlayer(connection, sp, cookie);
        if (start.health() != sp.getMaxHealth()) sp.setHealth(start.health());
        ItemStack held = heldItem(start.item());
        sp.setItemSlot(EquipmentSlot.MAINHAND, held.copy());

        OraclePlayer cp = new OraclePlayer(level, profile);
        cp.setItemSlot(EquipmentSlot.MAINHAND, held.copy());
        cp.setPos(start.x(), y, start.z());
        cp.setYRot(start.yaw());
        cp.setOldPosAndRot();
        Side side = new Side(name, cp, sp, channel);
        side.xLast = cp.getX();
        side.yLast = cp.getY();
        side.zLast = cp.getZ();
        side.yRotLast = cp.getYRot();
        side.xRotLast = cp.getXRot();
        collect(side);
        return side;
    }

    /** The main-hand item a scenario names: a field of {@code Items}, e.g. "diamond_sword". */
    static ItemStack heldItem(String name) {
        if (name.isEmpty()) return ItemStack.EMPTY;
        try {
            return new ItemStack((Item) Items.class.getField(name.toUpperCase(java.util.Locale.ROOT)).get(null));
        } catch (ReflectiveOperationException e) {
            throw new IllegalArgumentException("no item named " + name, e);
        }
    }

    /** One client tick for `me`: pick, keybinds (click), movement, then sendChanges (Minecraft.tick). */
    private void step(Side me, CombatScenario.Input in, int targetId) {
        OraclePlayer c = me.client;
        me.sentAttack = false;
        // The mouse turns the player between ticks, so this tick's rotation is in
        // place before the pick, the click and the movement.
        c.setYRot(in.yaw());
        c.setXRot(in.pitch());
        // Minecraft.tick: pick(1.0F), then handleKeybinds (startAttack per click,
        // then continueAttack with the key state), then missTime--.
        me.tick++;
        me.pick = pick(c, me.view);
        boolean instantAttack = false;
        if (in.attack()) instantAttack |= startAttack(me, targetId);
        continueAttack(me, !instantAttack && in.attackHeld());
        if (me.missTime > 0) me.missTime--;
        c.setKeys(new Input(in.forward(), in.backward(), in.left(), in.right(), in.jump(), in.shift(), in.sprint()));
        c.setOldPosAndRot();
        c.tickCount++;
        c.tick();
        // ClientLevel.tickEntities runs in insertion order: the local player (added
        // at login) first, then the remote player (added on its AddEntity packet).
        // Only commonTick moves the remote player; see RemoteView.
        me.view.clientCommonTick();
        sendChanges(me);
        // Minecraft.tick ends every client tick with this packet; the server uses it
        // to reset per-tick bookkeeping such as receivedPositionThisTick.
        me.toServer.add(ServerboundClientTickEndPacket.INSTANCE);
    }

    /** Minecraft.startAttack for a bare hand in survival; returns endAttack. */
    private boolean startAttack(Side me, int targetId) {
        if (me.missTime > 0) return false;
        switch (me.pick.getType()) {
            case ENTITY -> {
                // MultiPlayerGameMode.attack: the packet, then the client-side
                // Player.attack (a no-op against another player) and the reset.
                me.toServer.add(new ServerboundAttackPacket(targetId));
                me.client.resetAttackStrengthTicker();
                me.sentAttack = true;
            }
            case BLOCK -> {
                BlockHitResult hit = (BlockHitResult) me.pick;
                startDestroyBlock(me, hit.getBlockPos(), hit.getDirection());  // never an instant break in scope
            }
            case MISS -> {
                me.missTime = 10;  // gameMode.hasMissTime(): survival
                me.client.resetAttackStrengthTicker();
            }
        }
        me.toServer.add(ServerboundPunchPacket.INSTANCE);  // after player.swing
        return false;
    }

    /** Minecraft.continueAttack: releasing the key clears missTime; holding it mines. */
    private void continueAttack(Side me, boolean down) {
        if (!down) me.missTime = 0;
        if (me.missTime > 0) return;
        if (down && me.pick instanceof BlockHitResult hit && me.pick.getType() == HitResult.Type.BLOCK) {
            // MultiPlayerGameMode.continueDestroyBlock (survival, destroyDelay 0)
            if (me.isDestroying && hit.getBlockPos().equals(me.destroyBlockPos)) {
                if (me.destroyStartTick != me.tick) {
                    throw new IllegalStateException(me.name + " held attack on a block: mining is out of scope");
                }
                // destroyProgress grows; one tick of a bare hand never breaks these blocks
            } else {
                startDestroyBlock(me, hit.getBlockPos(), hit.getDirection());
            }
            me.toServer.add(ServerboundPunchPacket.INSTANCE);  // swing + punch
        } else {
            stopDestroyBlock(me);
        }
    }

    private void startDestroyBlock(Side me, BlockPos pos, Direction direction) {
        if (me.isDestroying && pos.equals(me.destroyBlockPos)) return;
        if (me.isDestroying) {
            me.toServer.add(new ServerboundPlayerActionPacket(ServerboundPlayerActionPacket.Action.ABORT_DESTROY_BLOCK, me.destroyBlockPos, direction));
        }
        me.toServer.add(new ServerboundPlayerActionPacket(ServerboundPlayerActionPacket.Action.START_DESTROY_BLOCK, pos, direction, ++me.sequence));
        me.isDestroying = true;
        me.destroyBlockPos = pos;
        me.destroyStartTick = me.tick;
    }

    private void stopDestroyBlock(Side me) {
        if (!me.isDestroying) return;
        me.toServer.add(new ServerboundPlayerActionPacket(ServerboundPlayerActionPacket.Action.ABORT_DESTROY_BLOCK, me.destroyBlockPos, Direction.DOWN));
        me.isDestroying = false;
        me.client.resetAttackStrengthTicker();
    }

    /**
     * LocalPlayer.pick(camera, blockRange, entityRange, 1.0F) for a bare hand
     * (raycastHitResult without an ATTACK_RANGE component). The candidate is the
     * other player as this client sees it (RemoteView), or none before it was sent.
     */
    static HitResult pick(OraclePlayer camera, @org.jspecify.annotations.Nullable Entity target) {
        double blockInteractionRange = camera.blockInteractionRange();
        double entityInteractionRange = camera.entityInteractionRange();
        double maxDistance = Math.max(blockInteractionRange, entityInteractionRange);
        double maxDistanceSq = Mth.square(maxDistance);
        Vec3 from = camera.getEyePosition(1.0F);
        HitResult blockHitResult = camera.pick(maxDistance, 1.0F, false);
        double blockDistanceSq = blockHitResult.getLocation().distanceToSqr(from);
        if (blockHitResult.getType() != HitResult.Type.MISS) {
            maxDistanceSq = blockDistanceSq;
            maxDistance = Math.sqrt(maxDistanceSq);
        }
        Vec3 direction = camera.getViewVector(1.0F);
        Vec3 to = from.add(direction.x * maxDistance, direction.y * maxDistance, direction.z * maxDistance);
        AABB box = camera.getBoundingBox().expandTowards(direction.scale(maxDistance)).inflate(1.0, 1.0, 1.0);
        EntityHitResult entityHitResult = target == null ? null : entityHitResult(camera, target, from, to, box, maxDistanceSq);
        return entityHitResult != null && entityHitResult.getLocation().distanceToSqr(from) < blockDistanceSq
                ? filterHitResult(entityHitResult, from, entityInteractionRange)
                : filterHitResult(blockHitResult, from, blockInteractionRange);
    }

    /**
     * ProjectileUtil.getEntityHitResult(except, from, to, box, CAN_BE_PICKED, maxValue)
     * for the one candidate a duel has. On a real client the opponent is found
     * through ClientLevel.getEntities; in the oracle it is a server copy, which
     * the server level's spatial lookup does not return here, so the candidate
     * test (box intersection) is inlined. The loop body is vanilla's.
     */
    private static EntityHitResult entityHitResult(Entity except, Entity entity, Vec3 from, Vec3 to, AABB box, double maxValue) {
        if (!diagnosed) {
            diagnosed = true;
            System.out.println("[oracle] diag getEntities: found=" + except.level().getEntities(except, box, e -> e == entity).size()
                    + " any=" + except.level().getEntities(except, box).size() + " intersects=" + entity.getBoundingBox().intersects(box)
                    + " byId=" + (except.level().getEntity(entity.getId()) == entity) + " pickable=" + EntitySelector.CAN_BE_PICKED.test(entity)
                    + " sameLevel=" + (except.level() == entity.level()));
        }
        if (!entity.getBoundingBox().intersects(box) || entity == except || !EntitySelector.CAN_BE_PICKED.test(entity)) return null;
        double nearest = maxValue;
        Entity hovered = null;
        Vec3 hoveredPos = null;
        AABB bb = entity.getBoundingBox().inflate(entity.getPickRadius());
        java.util.Optional<Vec3> clipPoint = bb.clip(from, to);
        if (bb.contains(from)) {
            if (nearest >= 0.0 && entity.canBePickedFromInside()) {
                hovered = entity;
                hoveredPos = clipPoint.orElse(from);
            }
        } else if (clipPoint.isPresent()) {
            Vec3 location = clipPoint.get();
            double dd = from.distanceToSqr(location);
            if ((dd < nearest || nearest == 0.0) && entity.getRootVehicle() != except.getRootVehicle()) {
                hovered = entity;
                hoveredPos = location;
            }
        }
        return hovered == null ? null : new EntityHitResult(hovered, hoveredPos);
    }

    private static boolean diagnosed;

    private static HitResult filterHitResult(HitResult hitResult, Vec3 from, double maxRange) {
        Vec3 hitLocation = hitResult.getLocation();
        if (!hitLocation.closerThan(from, maxRange)) {
            Direction direction = Direction.getApproximateNearest(hitLocation.x - from.x, hitLocation.y - from.y, hitLocation.z - from.z);
            return BlockHitResult.miss(hitLocation, direction, BlockPos.containing(hitLocation));
        }
        return hitResult;
    }

    /** Port of LocalPlayer.sendChanges / sendPosition / sendIsSprintingIfNeeded (not a passenger). */
    private static void sendChanges(Side me) {
        OraclePlayer c = me.client;
        if (!me.lastSentInput.equals(c.keys())) {
            me.toServer.add(new ServerboundPlayerInputPacket(c.keys()));
            me.lastSentInput = c.keys();
        }
        boolean sprinting = c.isSprinting();
        if (sprinting != me.wasSprinting) {
            me.toServer.add(new ServerboundPlayerCommandPacket(me.server,
                    sprinting ? ServerboundPlayerCommandPacket.Action.START_SPRINTING
                              : ServerboundPlayerCommandPacket.Action.STOP_SPRINTING));
            me.wasSprinting = sprinting;
        }
        double dx = c.getX() - me.xLast, dy = c.getY() - me.yLast, dz = c.getZ() - me.zLast;
        double dyRot = c.getYRot() - me.yRotLast, dxRot = c.getXRot() - me.xRotLast;
        me.positionReminder++;
        boolean move = Mth.lengthSquared(dx, dy, dz) > Mth.square(2.0E-4) || me.positionReminder >= 20;
        boolean rot = dyRot != 0.0 || dxRot != 0.0;
        if (move && rot) {
            me.toServer.add(new ServerboundMovePlayerPacket.PosRot(c.position(), c.getYRot(), c.getXRot(), c.onGround(), c.horizontalCollision));
        } else if (move) {
            me.toServer.add(new ServerboundMovePlayerPacket.Pos(c.position(), c.onGround(), c.horizontalCollision));
        } else if (rot) {
            me.toServer.add(new ServerboundMovePlayerPacket.Rot(c.getYRot(), c.getXRot(), c.onGround(), c.horizontalCollision));
        } else if (me.lastOnGround != c.onGround() || me.lastHorizontalCollision != c.horizontalCollision) {
            me.toServer.add(new ServerboundMovePlayerPacket.StatusOnly(c.onGround(), c.horizontalCollision));
        }
        if (move) {
            me.xLast = c.getX();
            me.yLast = c.getY();
            me.zLast = c.getZ();
            me.positionReminder = 0;
        }
        if (rot) {
            me.yRotLast = c.getYRot();
            me.xRotLast = c.getXRot();
        }
        me.lastOnGround = c.onGround();
        me.lastHorizontalCollision = c.horizontalCollision;
    }

    /** Server handles each client's packets in send order, then runs exactly one server tick. */
    private void serverStep(List<Side> sides) throws Exception {
        for (Side s : sides) {
            for (Packet<?> p : s.toServer) handle(s.server, p);
            s.toServer.clear();
        }
        tickServer.invoke(server, (BooleanSupplier) () -> false);
        // Mock connections are not in the server's connection list, so tick them
        // here, as tickChildren -> tickConnection would (this runs ServerPlayer.doTick).
        for (Side s : sides) s.server.connection.tick();
        for (Side s : sides) collect(s);
    }

    private static void handle(ServerPlayer sp, Packet<?> p) {
        var c = sp.connection;
        if (p instanceof ServerboundMovePlayerPacket m) c.handleMovePlayer(m);
        else if (p instanceof ServerboundAttackPacket at) c.handleAttack(at);
        else if (p instanceof ServerboundPlayerCommandPacket pc) c.handlePlayerCommand(pc);
        else if (p instanceof ServerboundPlayerInputPacket pi) c.handlePlayerInput(pi);
        else if (p instanceof ServerboundAcceptTeleportationPacket t) c.handleAcceptTeleportPacket(t);
        else if (p instanceof ServerboundPlayerLoadedPacket l) c.handleAcceptPlayerLoad(l);
        else if (p instanceof ServerboundClientTickEndPacket e) c.handleClientTickEnd(e);
        else if (p instanceof ServerboundPunchPacket pp) c.handlePunch(pp);
        else if (p instanceof ServerboundPlayerActionPacket pa) c.handlePlayerAction(pa);
        else if (p instanceof ServerboundChunkBatchReceivedPacket cb) c.handleChunkBatchReceived(cb);
        else throw new IllegalStateException("no handler for " + p.getClass().getSimpleName());
    }

    private static void collect(Side s) {
        if (!s.channel.isOpen()) {
            throw new IllegalStateException("player " + s.name + " was disconnected by the server (see log above)");
        }
        s.channel.flushOutbound();
        Object o;
        while ((o = s.channel.readOutbound()) != null) {
            if (o instanceof ClientboundBundlePacket bundle) {
                for (Packet<?> sub : bundle.subPackets()) s.toClient.add(sub);
            } else if (o instanceof Packet<?> p) {
                s.toClient.add(p);
            }
        }
    }

    /** Client handles queued server packets before its next tick (ClientPacketListener). */
    private static void deliver(Side s, Side other) {
        s.gotVelocity = false;
        s.viewRecv = 0;
        int self = s.server.getId();
        int otherId = other.server.getId();
        for (Packet<?> p : s.toClient) {
            if (p instanceof ClientboundAddEntityPacket add && add.getId() == otherId) {
                // ClientPacketListener.handleAddEntity -> RemotePlayer.recreateFromPacket
                if (s.view != null) throw new IllegalStateException(s.name + " was sent the other player twice");
                RemoteView v = new RemoteView(s.client.level(), other.server.getGameProfile());
                v.recreateFromPacket(add);
                v.setOldPosAndRot();
                s.view = v;
                s.viewRecv |= 16;
            } else if (p instanceof ClientboundMoveEntityPacket m && m.getEntity(other.server.level()) == other.server) {
                // ClientPacketListener.handleMoveEntity, not locally authoritative
                RemoteView v = requireView(s, p);
                if (m.hasPosition()) {
                    VecDeltaCodec codec = v.getPositionCodec();
                    PositionPath pos = m.getPositionDelta().decode(codec);
                    codec.setBase(pos.endPosition());
                    if (m.hasRotation()) v.moveOrInterpolateTo(pos, m.getYRot(), m.getXRot());
                    else v.moveOrInterpolateTo(pos);
                } else if (m.hasRotation()) {
                    v.moveOrInterpolateTo(m.getYRot(), m.getXRot());
                }
                v.setOnGround(m.isOnGround());
                s.viewRecv |= m.hasPosition() ? (m.hasRotation() ? 2 : 1) : 4;
            } else if (p instanceof ClientboundEntityPositionSyncPacket sync && sync.id() == otherId) {
                // ClientPacketListener.handleEntityPositionSync (the view is a ticking entity)
                RemoteView v = requireView(s, p);
                PositionPath path = sync.position();
                Vec3 pos = path.endPosition();
                v.getPositionCodec().setBase(pos);
                if (v.position().distanceToSqr(pos) > 4096.0) v.snapTo(pos, sync.yRot(), sync.xRot());
                else v.moveOrInterpolateTo(path, sync.yRot(), sync.xRot());
                v.setOnGround(sync.onGround());
                s.viewRecv |= 8;
            } else if (p instanceof ClientboundChunkBatchFinishedPacket) {
                // A real client acknowledges each chunk batch; the server holds back
                // entity pairing for chunks still pending (ChunkMap.isChunkTracked).
                s.toServer.add(new ServerboundChunkBatchReceivedPacket(64.0F));
                s.ignoredToClient.merge("ClientboundChunkBatchFinishedPacket(acked)", 1, Integer::sum);
            } else if (p instanceof ClientboundPlayerPositionPacket pos) {
                OraclePlayer c = s.client;
                PositionMoveRotation now = new PositionMoveRotation(c.position(), c.getDeltaMovement(), c.getYRot(), c.getXRot());
                PositionMoveRotation to = PositionMoveRotation.calculateAbsolute(now, pos.change(), pos.relatives());
                c.setPos(to.position());
                c.setDeltaMovement(to.deltaMovement());
                c.setYRot(to.yRot());
                c.setXRot(to.xRot());
                c.setOldPosAndRot();
                s.toServer.add(new ServerboundAcceptTeleportationPacket(pos.id(), c.getX(), c.getY(), c.getZ(), c.getYRot(), c.getXRot()));
                s.teleports++;
            } else if (p instanceof ClientboundSetEntityMotionPacket m && m.id() == self) {
                s.client.lerpMotion(wire(m.movement()));  // the client sees the LpVec3-quantised value
                s.gotVelocity = true;
            } else if (p instanceof ClientboundSetEntityDataPacket d && d.id() == self) {
                s.client.getEntityData().assignValues(d.packedItems());
            } else if (p instanceof ClientboundUpdateAttributesPacket u && u.getEntityId() == self) {
                // ClientPacketListener.handleUpdateAttributes. The server echoes the
                // player's own attributes, e.g. the sprint speed modifier removed by
                // a sprint hit, and the client takes them as they are.
                for (ClientboundUpdateAttributesPacket.AttributeSnapshot snap : u.getValues()) {
                    AttributeInstance instance = s.client.getAttributes().getInstance(snap.attribute());
                    if (instance == null) continue;
                    instance.setBaseValue(snap.base());
                    instance.removeModifiers();
                    for (AttributeModifier modifier : snap.modifiers()) instance.addTransientModifier(modifier);
                }
            } else {
                s.ignoredToClient.merge(p.getClass().getSimpleName(), 1, Integer::sum);
            }
        }
        s.toClient.clear();
    }

    /** Which tracker packets each client is about to receive during the login handshake. */
    private static void handshakeDiag(CombatScenario s, int step, Side a, Side b) {
        if (!s.name().startsWith("10_")) return;
        for (Side side : List.of(a, b)) {
            Side other = side == a ? b : a;
            StringBuilder kinds = new StringBuilder();
            for (Packet<?> p : side.toClient) {
                if (p instanceof ClientboundAddEntityPacket add && add.getId() == other.server.getId()) {
                    kinds.append(" Add(").append(add.getX()).append(',').append(add.getY()).append(',').append(add.getZ())
                            .append(" yRot=").append(add.getYRot()).append(" xRot=").append(add.getXRot()).append(')');
                } else if (p instanceof ClientboundMoveEntityPacket m && m.getEntity(other.server.level()) == other.server) {
                    kinds.append(' ').append(p.getClass().getSimpleName()).append("(steps=").append(m.getPositionDelta().stepCount()).append(')');
                } else if (p instanceof ClientboundEntityPositionSyncPacket sync && sync.id() == other.server.getId()) {
                    kinds.append(" Sync(").append(sync.position()).append(" yRot=").append(sync.yRot()).append(" xRot=").append(sync.xRot())
                            .append(" onGround=").append(sync.onGround()).append(')');
                }
            }
            System.out.println("[oracle] handshake step " + step + " to " + side.name + ":" + kinds + " | other server onGround="
                    + other.server.onGround() + " pos=" + other.server.position() + " yRot=" + other.server.getYRot()
                    + " updateInterval=" + other.server.getType().updateInterval() + " trackDeltas=" + other.server.getType().trackDeltas());
        }
    }

    private static RemoteView requireView(Side s, Packet<?> p) {
        if (s.view == null) throw new IllegalStateException(s.name + " got " + p.getClass().getSimpleName() + " before the other player was added");
        return s.view;
    }

    /** Encode and decode through the game's own LpVec3, as the network would. */
    private static Vec3 wire(Vec3 v) {
        ByteBuf buf = Unpooled.buffer();
        LpVec3.write(buf, v);
        return LpVec3.read(buf);
    }

    private static String header(CombatScenario s) {
        StringBuilder f = new StringBuilder();
        for (String spec : FIELDS) {
            String[] kv = spec.split(":");
            if (f.length() > 0) f.append(", ");
            f.append('"').append(kv[0]).append("\": \"").append(kv[1]).append('"');
        }
        return "{\"format\": \"mcporl-trace\", \"version\": 1, \"mc_version\": \""
                + System.getProperty("mcporl.version", "unknown")
                + "\", \"source\": \"oracle\", \"domains\": [\"movement\", \"melee\"], \"fields\": {" + f
                + "}, \"meta\": {\"scenario\": \"" + s.name() + "\", \"players\": [\"A\", \"B\"]}}";
    }

    private static final Method TOUCHING_UNLOADED;

    static {
        try {
            TOUCHING_UNLOADED = net.minecraft.world.entity.Entity.class.getDeclaredMethod("touchingUnloadedChunk");
            TOUCHING_UNLOADED.setAccessible(true);
        } catch (NoSuchMethodException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    /** Fall tracking (and so crits) silently stops next to unloaded chunks; never trace that state. */
    private static void requireLoaded(Side s, int t) throws ReflectiveOperationException {
        if ((Boolean) TOUCHING_UNLOADED.invoke(s.server)) {
            throw new IllegalStateException(s.name + " touches an unloaded chunk at t=" + t);
        }
    }

    private static String line(int t, CombatScenario.Tick tick, Side a, Side b) {
        return "{\"t\": " + t + ", \"inputs\": {\"p0\": " + input(tick.a()) + ", \"p1\": " + input(tick.b())
                + "}, \"entities\": {\"p0\": " + state(a) + ", \"p1\": " + state(b) + "}}";
    }

    private static String input(CombatScenario.Input in) {
        return "{\"W\": " + in.forward() + ", \"S\": " + in.backward() + ", \"A\": " + in.left() + ", \"D\": " + in.right()
                + ", \"jump\": " + in.jump() + ", \"sneak\": " + in.shift() + ", \"sprint\": " + in.sprint()
                + ", \"attack\": " + in.attack() + ", \"attackHeld\": " + in.attackHeld() + ", \"yaw\": " + OracleMain.hex(in.yaw())
                + ", \"pitch\": " + OracleMain.hex(in.pitch()) + "}";
    }

    private static String state(Side s) {
        OraclePlayer c = s.client;
        Vec3 v = c.getDeltaMovement();
        ServerPlayer sp = s.server;
        return "{\"pos.x\": " + OracleMain.hex(c.getX()) + ", \"pos.y\": " + OracleMain.hex(c.getY())
                + ", \"pos.z\": " + OracleMain.hex(c.getZ())
                + ", \"vel.x\": " + OracleMain.hex(v.x) + ", \"vel.y\": " + OracleMain.hex(v.y) + ", \"vel.z\": " + OracleMain.hex(v.z)
                + ", \"yRot\": " + OracleMain.hex(c.getYRot()) + ", \"onGround\": " + c.onGround()
                + ", \"sprinting\": " + c.isSprinting()
                + ", \"server.sprinting\": " + sp.isSprinting() + ", \"server.health\": " + OracleMain.hex(sp.getHealth())
                + ", \"server.hurtTime\": " + sp.hurtTime + ", \"server.damageCooldown\": " + sp.damageCooldownTime
                + ", \"server.onGround\": " + sp.onGround() + ", \"server.fallDistance\": " + OracleMain.hex(sp.fallDistance)
                + ", \"server.attackStrength\": " + OracleMain.hex(sp.getAttackStrengthScale(0.5F))
                + ", \"server.vel.x\": " + OracleMain.hex(sp.getDeltaMovement().x)
                + ", \"server.vel.y\": " + OracleMain.hex(sp.getDeltaMovement().y)
                + ", \"server.vel.z\": " + OracleMain.hex(sp.getDeltaMovement().z)
                + ", \"server.yRot\": " + OracleMain.hex(sp.getYRot())
                + ", \"speedAttr\": " + OracleMain.hex(c.getAttributeValue(Attributes.MOVEMENT_SPEED))
                + ", \"pick\": " + s.pick.getType().ordinal()
                + ", \"pick.x\": " + OracleMain.hex(s.pick.getLocation().x)
                + ", \"pick.y\": " + OracleMain.hex(s.pick.getLocation().y)
                + ", \"pick.z\": " + OracleMain.hex(s.pick.getLocation().z)
                + ", \"missTime\": " + s.missTime
                + ", \"gotVelocity\": " + s.gotVelocity + ", \"sentAttack\": " + s.sentAttack
                + ", \"teleports\": " + s.teleports
                + ", \"view.x\": " + OracleMain.hex(s.view.getX()) + ", \"view.y\": " + OracleMain.hex(s.view.getY())
                + ", \"view.z\": " + OracleMain.hex(s.view.getZ()) + ", \"view.yRot\": " + OracleMain.hex(s.view.getYRot())
                + ", \"view.recv\": " + s.viewRecv + "}";
    }

    /** Stone blocks the pick probe adds to the flat arena, to exercise occlusion. */
    static final int[][] PROBE_BLOCKS = {{2, -60, 2}, {2, -59, 2}, {-2, -60, 1}, {0, -58, -2}, {-1, -60, -1}};

    /**
     * Random crosshair picks resolved by vanilla (the same path as a click), for
     * the simulator's pick port. One line per case, all values as hex bits:
     * xo yo zo x y z yRot xRot targetX targetY targetZ -> type hitX hitY hitZ.
     */
    void pickProbe(Path out, int cases) throws IOException {
        for (int dx = -8; dx <= 8; dx++) for (int dz = -8; dz <= 8; dz++) {
            level.setChunkForced(dx, dz, true);
            level.getChunk(dx, dz);
        }
        for (int[] b : PROBE_BLOCKS) level.setBlockAndUpdate(new BlockPos(b[0], b[1], b[2]), Blocks.STONE.defaultBlockState());
        Side a = spawn("A", new CombatScenario.Start(0.5, 0.5, 0.0F, 20.0F));
        Side b = spawn("B", new CombatScenario.Start(0.5, 3.0, 180.0F, 20.0F));
        java.util.Random rnd = new java.util.Random(20260930L);
        int[] counts = new int[3];
        try (Writer w = Files.newBufferedWriter(out)) {
            w.write("# pick golden (26.3): xo yo zo x y z yRot xRot tx ty tz -> type hx hy hz; flat world at y=-60 plus stone at");
            for (int[] bl : PROBE_BLOCKS) w.write(" " + bl[0] + "," + bl[1] + "," + bl[2]);
            w.write('\n');
            for (int i = 0; i < cases; i++) {
                double x = 0.5 + (rnd.nextDouble() * 5.0 - 2.5);
                double z = 0.5 + (rnd.nextDouble() * 5.0 - 2.5);
                double y = -60.0 + (rnd.nextBoolean() ? 0.0 : rnd.nextDouble() * 1.3);
                double xo = x + (rnd.nextDouble() - 0.5) * 0.8;
                double yo = Math.max(-60.0, y + (rnd.nextDouble() - 0.5) * 0.8);
                double zo = z + (rnd.nextDouble() - 0.5) * 0.8;
                double tx = x + (rnd.nextDouble() * 8.0 - 4.0);
                double tz = z + (rnd.nextDouble() * 8.0 - 4.0);
                double ty = -60.0 + (rnd.nextBoolean() ? 0.0 : rnd.nextDouble() * 1.5);
                float yRot, xRot;
                if (rnd.nextInt(3) > 0) {
                    // Aim near the target's body so a good share of cases hit it or graze it.
                    double ax = tx + (rnd.nextDouble() - 0.5) * 1.2 - x;
                    double ay = ty + rnd.nextDouble() * 2.0 - (y + 1.62);
                    double az = tz + (rnd.nextDouble() - 0.5) * 1.2 - z;
                    yRot = (float) (Math.atan2(-ax, az) * 180.0 / Math.PI) + (rnd.nextFloat() - 0.5F) * 360.0F * (rnd.nextInt(4) == 0 ? 1.0F : 0.0F);
                    xRot = Mth.clamp((float) (-Math.atan2(ay, Math.sqrt(ax * ax + az * az)) * 180.0 / Math.PI), -90.0F, 90.0F);
                } else {
                    yRot = rnd.nextFloat() * 360.0F - 180.0F;
                    xRot = rnd.nextFloat() * 180.0F - 90.0F;
                }
                OraclePlayer c = a.client;
                c.setPos(xo, yo, zo);
                c.setOldPosAndRot();
                c.setPos(x, y, z);
                c.setYRot(yRot);
                c.setXRot(xRot);
                b.server.absSnapTo(tx, ty, tz);
                HitResult hit = pick(c, b.server);
                counts[hit.getType().ordinal()]++;
                Vec3 l = hit.getLocation();
                w.write(String.join(" ", hx(xo), hx(yo), hx(zo), hx(x), hx(y), hx(z), hx(yRot), hx(xRot),
                        hx(tx), hx(ty), hx(tz), Integer.toString(hit.getType().ordinal()), hx(l.x), hx(l.y), hx(l.z)));
                w.write('\n');
            }
        } finally {
            for (Side side : List.of(a, b)) server.getPlayerList().remove(side.server);
        }
        System.out.println("[oracle] pick probe: " + cases + " cases, miss/block/entity = " + counts[0] + "/" + counts[1] + "/" + counts[2]);
    }

    private static String hx(double d) {
        return String.format("%016x", Double.doubleToRawLongBits(d));
    }

    private static String hx(float f) {
        return String.format("%08x", Float.floatToRawIntBits(f));
    }

    /** Force-load the arena chunks (on the server thread). */
    void forceArena() {
        for (int dx = -8; dx <= 8; dx++) for (int dz = -8; dz <= 8; dz++) {
            level.setChunkForced(dx, dz, true);
            level.getChunk(dx, dz);
        }
    }

    /**
     * Wait, off the server thread, until the arena chunks are entity-ticking.
     * Chunks get there asynchronously through the server's own task queue, so
     * each manual tick runs as its own short task and the queue drains between
     * them; ticking in a loop inside one task starves the queue and never
     * finishes. (The manual ticks also cover a server that paused itself for
     * having no players.)
     */
    static void awaitArena(MinecraftServer server) throws Exception {
        CombatOracle oracle = new CombatOracle(server);
        int ticks = 0;
        while (!server.submit((java.util.function.Supplier<Boolean>) oracle::arenaTicking).join()) {
            if (ticks >= 2000) throw new IllegalStateException("arena chunks are still not entity-ticking after " + ticks + " ticks");
            server.submit((Runnable) oracle::tickOnce).join();
            ticks++;
            Thread.sleep(5);
        }
        for (int i = 0; i < 20; i++) server.submit((Runnable) oracle::tickOnce).join();
        System.out.println("[oracle] arena entity-ticking after " + ticks + " ticks");
    }

    private void tickOnce() {
        try {
            tickServer.invoke(server, (BooleanSupplier) () -> false);
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }

    private boolean arenaTicking() {
        for (int cx = -2; cx <= 2; cx++) for (int cz = -2; cz <= 2; cz++) {
            if (!level.isPositionEntityTicking(new BlockPos(cx * 16 + 8, -60, cz * 16 + 8))) return false;
        }
        return true;
    }

    static void runAll(MinecraftServer server, Path dir, Path outDir, List<String> failures) throws IOException {
        java.util.concurrent.CompletableFuture<Void> forced = new java.util.concurrent.CompletableFuture<>();
        server.execute(() -> {
            try {
                new CombatOracle(server).forceArena();
                forced.complete(null);
            } catch (Throwable t) {
                forced.completeExceptionally(t);
            }
        });
        forced.join();
        try {
            awaitArena(server);
        } catch (Exception e) {
            throw new IOException(e);
        }
        List<Path> files;
        try (var stream = Files.list(dir)) {
            files = stream.filter(p -> p.toString().endsWith(".txt")).sorted().toList();
        }
        for (Path f : files) {
            CombatScenario s = CombatScenario.parse(f);
            java.util.concurrent.CompletableFuture<Void> done = new java.util.concurrent.CompletableFuture<>();
            server.execute(() -> {
                try {
                    new CombatOracle(server).run(s, outDir.resolve(s.name() + ".jsonl"));
                    done.complete(null);
                } catch (Throwable t) {
                    done.completeExceptionally(t);
                }
            });
            try {
                done.join();
                System.out.println("[oracle] OK   " + s.name() + " (" + s.ticks().size() + " ticks, combat)");
            } catch (Exception e) {
                failures.add(s.name());
                System.out.println("[oracle] FAIL " + s.name() + ": " + e.getCause());
                e.getCause().printStackTrace(System.out);
            }
        }
        // Last: it adds blocks to the arena.
        java.util.concurrent.CompletableFuture<Void> probe = new java.util.concurrent.CompletableFuture<>();
        server.execute(() -> {
            try {
                new CombatOracle(server).pickProbe(outDir.resolve("pick_golden.txt"), 4000);
                probe.complete(null);
            } catch (Throwable t) {
                probe.completeExceptionally(t);
            }
        });
        try {
            probe.join();
        } catch (Exception e) {
            failures.add("pick_probe");
            System.out.println("[oracle] FAIL pick probe: " + e.getCause());
            e.getCause().printStackTrace(System.out);
        }
    }
}
