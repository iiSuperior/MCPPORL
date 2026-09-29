package mcporl.oracle;

import com.mojang.authlib.GameProfile;
import java.io.IOException;
import java.io.Writer;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.stream.Stream;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.world.entity.player.Input;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.phys.Vec3;

/**
 * Boots a vanilla dedicated server in-process on a superflat world, then runs
 * every scenario on the server thread with an {@link OraclePlayer}, writing
 * one trace per scenario (docs/TRACE_FORMAT.md).
 *
 * Usage: OracleMain <scenario-dir> <trace-out-dir> [<combat-scenario-dir>]
 * Must run in a scratch directory; it writes server.properties and eula.txt.
 * Needs --add-opens java.base/java.lang=ALL-UNNAMED to find the server object.
 */
public final class OracleMain {
    static final String[] FIELDS = {
            "pos.x:f64", "pos.y:f64", "pos.z:f64", "vel.x:f64", "vel.y:f64", "vel.z:f64",
            "yRot:f32", "xRot:f32", "xxa:f32", "zza:f32",
            "onGround:bool", "horizontalCollision:bool", "sprinting:bool", "crouching:bool"};

    public static void main(String[] args) throws Exception {
        Path scenarioDir = Path.of(args[0]).toAbsolutePath();
        Path outDir = Path.of(args[1]).toAbsolutePath();
        Files.createDirectories(outDir);

        Files.writeString(Path.of("eula.txt"), "eula=true\n");
        Files.writeString(Path.of("server.properties"), String.join("\n",
                "level-name=oracle-world",
                "level-type=minecraft\\:flat",
                "generate-structures=false",
                "online-mode=false",
                "server-port=25599",
                "spawn-protection=0",
                "sync-chunk-writes=false",
                "enable-rcon=false",
                "enable-query=false",
                "difficulty=peaceful",
                "max-tick-time=-1",
                "") );

        net.minecraft.server.Main.main(new String[] {"--nogui"});
        MinecraftServer server = findServer();
        long deadline = System.currentTimeMillis() + 300_000;
        while (!server.isReady()) {
            if (System.currentTimeMillis() > deadline) throw new IllegalStateException("server did not become ready");
            Thread.sleep(100);
        }
        System.out.println("[oracle] server ready");

        List<Scenario> scenarios = new ArrayList<>();
        try (Stream<Path> files = Files.list(scenarioDir)) {
            for (Path p : files.filter(p -> p.toString().endsWith(".txt")).sorted().toList()) {
                scenarios.add(Scenario.parse(p));
            }
        }

        int failures = 0;
        for (Scenario s : scenarios) {
            CompletableFuture<Void> done = new CompletableFuture<>();
            server.execute(() -> {
                try {
                    run(server.overworld(), s, outDir.resolve(s.name() + ".jsonl"));
                    done.complete(null);
                } catch (Throwable t) {
                    done.completeExceptionally(t);
                }
            });
            try {
                done.join();
                System.out.println("[oracle] OK   " + s.name() + " (" + s.ticks().size() + " ticks)");
            } catch (Exception e) {
                failures++;
                System.out.println("[oracle] FAIL " + s.name() + ": " + e.getCause());
                e.getCause().printStackTrace(System.out);
            }
        }
        List<String> combatFailures = new ArrayList<>();
        if (args.length > 2) {
            CombatOracle.runAll(server, Path.of(args[2]).toAbsolutePath(), outDir, combatFailures);
        }
        int total = failures + combatFailures.size();
        server.halt(false);
        System.out.println("[oracle] done, " + total + " failures");
        System.exit(total == 0 ? 0 : 1);
    }

    static void run(ServerLevel level, Scenario s, Path out) throws IOException {
        // Load the area around the start so collision queries see real blocks.
        int cx = ((int) Math.floor(s.startX())) >> 4, cz = ((int) Math.floor(s.startZ())) >> 4;
        for (int dx = -8; dx <= 8; dx++) {
            for (int dz = -8; dz <= 8; dz++) {
                level.getChunk(cx + dx, cz + dz);
            }
        }
        int surfaceY = level.getHeight(Heightmap.Types.MOTION_BLOCKING,
                (int) Math.floor(s.startX()), (int) Math.floor(s.startZ()));

        OraclePlayer p = new OraclePlayer(level, new GameProfile(UUID.nameUUIDFromBytes(s.name().getBytes()), "oracle"));
        p.setPos(s.startX(), surfaceY, s.startZ());
        p.setYRot(s.startYaw());
        p.setXRot(s.startPitch());
        p.setOldPosAndRot();

        try (Writer w = Files.newBufferedWriter(out)) {
            w.write(header(s, surfaceY));
            w.write('\n');
            int t = 0;
            for (Scenario.Tick in : s.ticks()) {
                // The client applies mouse rotation before the tick, and samples keys inside it.
                p.setYRot(in.yaw());
                p.setXRot(in.pitch());
                p.setKeys(new Input(in.forward(), in.backward(), in.left(), in.right(), in.jump(), in.shift(), in.sprint()));
                p.setOldPosAndRot();
                p.tickCount++;
                p.tick();
                w.write(tickLine(t++, in, p));
                w.write('\n');
            }
        }
    }

    static String header(Scenario s, int surfaceY) {
        StringBuilder f = new StringBuilder();
        for (String spec : FIELDS) {
            String[] kv = spec.split(":");
            if (f.length() > 0) f.append(", ");
            f.append('"').append(kv[0]).append("\": \"").append(kv[1]).append('"');
        }
        return "{\"format\": \"mcporl-trace\", \"version\": 1, \"mc_version\": \""
                + System.getProperty("mcporl.version", "unknown")
                + "\", \"source\": \"oracle\", \"domains\": [\"movement\"], \"fields\": {" + f
                + "}, \"meta\": {\"scenario\": \"" + s.name() + "\", \"surfaceY\": " + surfaceY
                + ", \"start\": [" + hex(s.startX()) + ", " + hex((double) surfaceY) + ", " + hex(s.startZ()) + "]}}";
    }

    static String tickLine(int t, Scenario.Tick in, OraclePlayer p) {
        Vec3 v = p.getDeltaMovement();
        return "{\"t\": " + t
                + ", \"inputs\": {\"p0\": {\"W\": " + in.forward() + ", \"S\": " + in.backward()
                + ", \"A\": " + in.left() + ", \"D\": " + in.right() + ", \"jump\": " + in.jump()
                + ", \"sneak\": " + in.shift() + ", \"sprint\": " + in.sprint()
                + ", \"yaw\": " + hex(in.yaw()) + ", \"pitch\": " + hex(in.pitch()) + "}}"
                + ", \"entities\": {\"p0\": {"
                + "\"pos.x\": " + hex(p.getX()) + ", \"pos.y\": " + hex(p.getY()) + ", \"pos.z\": " + hex(p.getZ())
                + ", \"vel.x\": " + hex(v.x) + ", \"vel.y\": " + hex(v.y) + ", \"vel.z\": " + hex(v.z)
                + ", \"yRot\": " + hex(p.getYRot()) + ", \"xRot\": " + hex(p.getXRot())
                + ", \"xxa\": " + hex(p.xxa) + ", \"zza\": " + hex(p.zza)
                + ", \"onGround\": " + p.onGround() + ", \"horizontalCollision\": " + p.horizontalCollision
                + ", \"sprinting\": " + p.isSprinting() + ", \"crouching\": " + p.isCrouching()
                + "}}}";
    }

    static String hex(double d) {
        return String.format("\"%016x\"", Double.doubleToRawLongBits(d));
    }

    static String hex(float f) {
        return String.format("\"%08x\"", Float.floatToRawIntBits(f));
    }

    /** Main.main keeps the server only in a local captured by its shutdown hook. */
    static MinecraftServer findServer() throws Exception {
        Class<?> hooksClass = Class.forName("java.lang.ApplicationShutdownHooks");
        Field hooksField = hooksClass.getDeclaredField("hooks");
        hooksField.setAccessible(true);
        @SuppressWarnings("unchecked")
        Map<Thread, Thread> hooks = (Map<Thread, Thread>) hooksField.get(null);
        synchronized (hooksClass) {
            for (Thread hook : hooks.keySet()) {
                for (Field f : hook.getClass().getDeclaredFields()) {
                    if (MinecraftServer.class.isAssignableFrom(f.getType())) {
                        f.setAccessible(true);
                        return (MinecraftServer) f.get(hook);
                    }
                }
            }
        }
        throw new IllegalStateException("could not find the server instance (did Main.main fail? see log above)");
    }
}
