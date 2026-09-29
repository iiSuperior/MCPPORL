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
import net.minecraft.network.protocol.game.ClientboundBundlePacket;
import net.minecraft.network.protocol.game.ClientboundPlayerPositionPacket;
import net.minecraft.network.protocol.game.ClientboundSetEntityDataPacket;
import net.minecraft.network.protocol.game.ClientboundSetEntityMotionPacket;
import net.minecraft.network.protocol.game.ServerboundAcceptTeleportationPacket;
import net.minecraft.network.protocol.game.ServerboundAttackPacket;
import net.minecraft.network.protocol.game.ServerboundClientTickEndPacket;
import net.minecraft.network.protocol.game.ServerboundMovePlayerPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerCommandPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerInputPacket;
import net.minecraft.network.protocol.game.ServerboundPlayerLoadedPacket;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.ServerPlayer;
import net.minecraft.server.network.CommonListenerCookie;
import net.minecraft.util.Mth;
import net.minecraft.world.entity.PositionMoveRotation;
import net.minecraft.world.entity.player.Input;
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
        // per-tick trace flags
        boolean gotVelocity, sentAttack;
        int teleports;  // server position corrections applied during the scenario

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
            "gotVelocity:bool", "sentAttack:bool", "teleports:i32"};

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
            for (Side side : List.of(a, b)) {
                deliver(side);  // applies the login teleport and queues its acknowledgement
                side.toServer.add(new ServerboundPlayerLoadedPacket());
            }
            for (int i = 0; i < 2; i++) {
                serverStep(List.of(a, b));
                deliver(a);
                deliver(b);
            }
            for (Side side : List.of(a, b)) {
                side.teleports = 0;
                side.xLast = side.client.getX();
                side.yLast = side.client.getY();
                side.zLast = side.client.getZ();
            }
            int t = 0;
            for (CombatScenario.Tick tick : s.ticks()) {
                step(a, b, tick.a(), b.server.getId());
                step(b, a, tick.b(), a.server.getId());
                serverStep(List.of(a, b));
                requireLoaded(a, t);
                requireLoaded(b, t);
                w.write(line(t++, tick, a, b));
                w.write('\n');
                deliver(a);
                deliver(b);
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

        OraclePlayer cp = new OraclePlayer(level, profile);
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

    /** One client tick for `me`: keybinds (attack), movement, then sendChanges. */
    private void step(Side me, Side other, CombatScenario.Input in, int targetId) {
        OraclePlayer c = me.client;
        me.sentAttack = false;
        // Minecraft.tick: handleKeybinds runs before level.tickEntities. The attack
        // packet therefore precedes this tick's movement/rotation packet. The
        // client-side Player.attack deals no damage (hurtClient is false), so the
        // only local effect is resetting the attack strength ticker.
        if (in.attack()) {
            me.toServer.add(new ServerboundAttackPacket(targetId));
            c.resetAttackStrengthTicker();
            me.sentAttack = true;
        }
        c.setYRot(in.yaw());
        c.setXRot(in.pitch());
        c.setKeys(new Input(in.forward(), in.backward(), in.left(), in.right(), in.jump(), in.shift(), in.sprint()));
        c.setOldPosAndRot();
        c.tickCount++;
        c.tick();
        sendChanges(me);
        // Minecraft.tick ends every client tick with this packet; the server uses it
        // to reset per-tick bookkeeping such as receivedPositionThisTick.
        me.toServer.add(ServerboundClientTickEndPacket.INSTANCE);
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
    private static void deliver(Side s) {
        s.gotVelocity = false;
        int self = s.server.getId();
        for (Packet<?> p : s.toClient) {
            if (p instanceof ClientboundPlayerPositionPacket pos) {
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
            } else {
                s.ignoredToClient.merge(p.getClass().getSimpleName(), 1, Integer::sum);
            }
        }
        s.toClient.clear();
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
                + ", \"attack\": " + in.attack() + ", \"yaw\": " + OracleMain.hex(in.yaw())
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
                + ", \"gotVelocity\": " + s.gotVelocity + ", \"sentAttack\": " + s.sentAttack
                + ", \"teleports\": " + s.teleports + "}";
    }

    static void runAll(MinecraftServer server, Path dir, Path outDir, List<String> failures) throws IOException {
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
    }
}
