package mcporl.oracle;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/** Two-player combat script (see oracle/combat/README.md). */
public record CombatScenario(String name, Start a, Start b, List<Tick> ticks) {

    /**
     * item: the main-hand item's field name in {@code Items} (lower case), or empty
     * for a bare hand; it goes in hotbar slot 0. hotbar: comma-separated items for
     * slots 0.. (overrides item; "-" leaves a slot empty). offhand: the off-hand item.
     */
    public record Start(double x, double z, float yaw, float health, String item, List<String> hotbar, String offhand) {
        public Start(double x, double z, float yaw, float health) {
            this(x, z, yaw, health, "", List.of(), "");
        }

        /** Hotbar slot contents (slot 0 = item when no hotbar is given). */
        public List<String> slots() {
            return hotbar.isEmpty() ? List.of(item) : hotbar;
        }
    }

    /**
     * Inputs for one player on one tick. yaw/pitch are the rotation during the tick
     * (sent at its end). attack: the attack key was pressed since the last tick (a
     * click). attackHeld: the key is down when the tick samples it. Tokens:
     * "attack" = press and still down, "tap" = press already released, "hold" =
     * down without a new press.
     */
    /**
     * slot: the hotbar key pressed this tick (0-8), or -1. use: the use key is down
     * when the tick samples it ("use" token; a press on the first such tick).
     */
    public record Input(boolean forward, boolean backward, boolean left, boolean right, boolean jump,
                        boolean shift, boolean sprint, boolean attack, boolean attackHeld, float yaw, float pitch,
                        int slot, boolean use) {}

    public record Tick(Input a, Input b) {}

    /** Mutable per-player parse state: rotation carries across segments. */
    private static final class Side {
        float yaw, pitch;

        Side(float yaw) {
            this.yaw = yaw;
        }
    }

    public static CombatScenario parse(Path file) throws IOException {
        String name = file.getFileName().toString().replaceFirst("\\.txt$", "");
        Start a = new Start(0.5, 0.5, 0.0F, 20.0F), b = new Start(0.5, 3.0, 180.0F, 20.0F);
        List<String[]> segments = new ArrayList<>();
        int lineNo = 0;
        for (String raw : Files.readAllLines(file)) {
            lineNo++;
            String line = raw.replaceFirst("#.*", "").trim().toLowerCase(Locale.ROOT);
            if (line.isEmpty()) continue;
            try {
                if (line.startsWith("start ")) {
                    String[] tok = line.split("\\s+");
                    double x = 0.5, z = 0.5;
                    float yaw = 0.0F, health = 20.0F;
                    String item = "", offhand = "";
                    List<String> hotbar = List.of();
                    for (int i = 2; i < tok.length; i++) {
                        String[] kv = tok[i].split("=", 2);
                        switch (kv[0]) {
                            case "x" -> x = Double.parseDouble(kv[1]);
                            case "z" -> z = Double.parseDouble(kv[1]);
                            case "yaw" -> yaw = Float.parseFloat(kv[1]);
                            case "health" -> health = Float.parseFloat(kv[1]);
                            case "item" -> item = kv[1];
                            case "hotbar" -> hotbar = List.of(kv[1].split(",")).stream().map(v -> v.equals("-") ? "" : v).toList();
                            case "offhand" -> offhand = kv[1];
                            default -> throw new IllegalArgumentException("unknown start key " + kv[0]);
                        }
                    }
                    if (hotbar.size() > 9) throw new IllegalArgumentException("a hotbar has 9 slots");
                    Start s = new Start(x, z, yaw, health, item, hotbar, offhand);
                    switch (tok[1]) {
                        case "a" -> a = s;
                        case "b" -> b = s;
                        default -> throw new IllegalArgumentException("unknown player " + tok[1]);
                    }
                    continue;
                }
                String[] halves = line.split("\\|", -1);
                if (halves.length != 2) throw new IllegalArgumentException("expected '<ticks> <A inputs> | <B inputs>'");
                String[] head = halves[0].trim().split("\\s+", 2);
                segments.add(new String[] {head[0], head.length > 1 ? head[1] : "", halves[1].trim(), file + ":" + lineNo});
            } catch (RuntimeException e) {
                throw new IOException(file + ":" + lineNo + ": " + e.getMessage(), e);
            }
        }
        Side sa = new Side(a.yaw()), sb = new Side(b.yaw());
        List<Tick> ticks = new ArrayList<>();
        for (String[] seg : segments) {
            try {
                int count = Integer.parseInt(seg[0]);
                for (int i = 0; i < count; i++) {
                    ticks.add(new Tick(input(seg[1], sa), input(seg[2], sb)));
                }
            } catch (RuntimeException e) {
                throw new IOException(seg[3] + ": " + e.getMessage(), e);
            }
        }
        return new CombatScenario(name, a, b, ticks);
    }

    private static Input input(String spec, Side side) {
        boolean f = false, bk = false, l = false, r = false, j = false, sh = false, sp = false, at = false, held = false, use = false;
        int slot = -1;
        for (String t : spec.isEmpty() ? new String[0] : spec.split("\\s+")) {
            if (t.startsWith("yaw=")) side.yaw = Float.parseFloat(t.substring(4));
            else if (t.startsWith("pitch=")) side.pitch = Float.parseFloat(t.substring(6));
            else if (t.startsWith("slot=")) {
                slot = Integer.parseInt(t.substring(5));
                if (slot < 0 || slot > 8) throw new IllegalArgumentException("slot must be 0-8");
            }
            else switch (t) {
                case "w" -> f = true;
                case "s" -> bk = true;
                case "a" -> l = true;
                case "d" -> r = true;
                case "jump" -> j = true;
                case "sneak" -> sh = true;
                case "sprint" -> sp = true;
                case "attack" -> {
                    at = true;
                    held = true;
                }
                case "tap" -> at = true;
                case "hold" -> held = true;
                case "use" -> use = true;
                case "idle" -> { }
                default -> throw new IllegalArgumentException("unknown token " + t);
            }
        }
        return new Input(f, bk, l, r, j, sh, sp, at, held, side.yaw, side.pitch, slot, use);
    }
}
