package mcporl.oracle;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * A scripted input sequence. Format (see oracle/scenarios/README.md):
 *
 * <pre>
 * start x=0.5 z=0.5 yaw=0 pitch=0
 * 10 idle
 * 40 W sprint yaw=45
 * 20 W sprint jump dyaw=2.5
 * </pre>
 *
 * Each segment line is a tick count followed by held keys (W A S D jump sneak
 * sprint, or idle) and optional rotation: yaw=/pitch= set the absolute angle
 * for the segment, dyaw=/dpitch= add that many degrees every tick.
 */
public record Scenario(String name, double startX, double startZ, float startYaw, float startPitch, List<Tick> ticks) {

    public record Tick(boolean forward, boolean backward, boolean left, boolean right,
                       boolean jump, boolean shift, boolean sprint, float yaw, float pitch) {}

    public static Scenario parse(Path file) throws IOException {
        String name = file.getFileName().toString().replaceFirst("\\.txt$", "");
        double sx = 0.5, sz = 0.5;
        float yaw = 0.0F, pitch = 0.0F;
        List<Tick> ticks = new ArrayList<>();
        int lineNo = 0;
        for (String raw : Files.readAllLines(file)) {
            lineNo++;
            String line = raw.replaceFirst("#.*", "").trim();
            if (line.isEmpty()) continue;
            String[] tok = line.toLowerCase(Locale.ROOT).split("\\s+");
            try {
                if (tok[0].equals("start")) {
                    for (int i = 1; i < tok.length; i++) {
                        String[] kv = tok[i].split("=", 2);
                        switch (kv[0]) {
                            case "x" -> sx = Double.parseDouble(kv[1]);
                            case "z" -> sz = Double.parseDouble(kv[1]);
                            case "yaw" -> yaw = Float.parseFloat(kv[1]);
                            case "pitch" -> pitch = Float.parseFloat(kv[1]);
                            default -> throw new IllegalArgumentException("unknown start key " + kv[0]);
                        }
                    }
                    continue;
                }
                int count = Integer.parseInt(tok[0]);
                boolean f = false, b = false, l = false, r = false, j = false, sh = false, sp = false;
                float dyaw = 0.0F, dpitch = 0.0F;
                for (int i = 1; i < tok.length; i++) {
                    String t = tok[i];
                    if (t.startsWith("yaw=")) yaw = Float.parseFloat(t.substring(4));
                    else if (t.startsWith("pitch=")) pitch = Float.parseFloat(t.substring(6));
                    else if (t.startsWith("dyaw=")) dyaw = Float.parseFloat(t.substring(5));
                    else if (t.startsWith("dpitch=")) dpitch = Float.parseFloat(t.substring(7));
                    else switch (t) {
                        case "w" -> f = true;
                        case "s" -> b = true;
                        case "a" -> l = true;
                        case "d" -> r = true;
                        case "jump" -> j = true;
                        case "sneak" -> sh = true;
                        case "sprint" -> sp = true;
                        case "idle" -> { }
                        default -> throw new IllegalArgumentException("unknown token " + t);
                    }
                }
                for (int i = 0; i < count; i++) {
                    yaw += dyaw;
                    pitch += dpitch;
                    ticks.add(new Tick(f, b, l, r, j, sh, sp, yaw, pitch));
                }
            } catch (RuntimeException e) {
                throw new IOException(file + ":" + lineNo + ": " + e.getMessage(), e);
            }
        }
        return new Scenario(name, sx, sz, 0.0F, 0.0F, ticks).withStart(yaw, pitch, ticks);
    }

    private Scenario withStart(float lastYaw, float lastPitch, List<Tick> ticks) {
        float y0 = ticks.isEmpty() ? lastYaw : ticks.get(0).yaw();
        float p0 = ticks.isEmpty() ? lastPitch : ticks.get(0).pitch();
        return new Scenario(name, startX, startZ, y0, p0, ticks);
    }
}
