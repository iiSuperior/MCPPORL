import java.io.BufferedWriter;
import java.io.DataOutputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.SplittableRandom;

/**
 * Generates JVM-produced reference vectors for the simulator's Java-exact math
 * layer (sim/include/mcp/jmath.hpp). This checks that the C++ side reproduces
 * Java semantics on this JVM. It does not by itself prove the formulas match
 * Mojang's Mth; that is the oracle harness's job once the game jar is available.
 *
 * Usage: java JRef.java <out-dir>
 */
public final class JRef {
    private static final double RAD_TO_INDEX = 10430.378350470453;
    private static final float[] SIN = new float[65536];

    static {
        for (int i = 0; i < SIN.length; i++) {
            SIN[i] = (float) Math.sin((double) i * Math.PI * 2.0 / 65536.0);
        }
    }

    static float sin(double rad) { return SIN[(int) ((long) (rad * RAD_TO_INDEX) & 65535L)]; }
    static float cos(double rad) { return SIN[(int) ((long) (rad * RAD_TO_INDEX + 16384.0) & 65535L)]; }

    static int floor(double d) { int i = (int) d; return d < (double) i ? i - 1 : i; }
    static int ceil(double d) { int i = (int) d; return d > (double) i ? i + 1 : i; }

    public static void main(String[] args) throws IOException {
        Path out = Path.of(args.length > 0 ? args[0] : ".");
        Files.createDirectories(out);

        try (DataOutputStream s = new DataOutputStream(new FileOutputStream(out.resolve("sin_table.bin").toFile()))) {
            for (float f : SIN) s.writeInt(Float.floatToRawIntBits(f));
        }

        List<Double> doubles = new ArrayList<>(List.of(
                0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.5, -1.5, 2.9999999, -2.9999999,
                Double.NaN, Double.POSITIVE_INFINITY, Double.NEGATIVE_INFINITY,
                2147483647.0, 2147483647.5, 2147483648.0, -2147483648.0, -2147483648.5, -2147483649.0,
                9.223372036854776E18, -9.223372036854776E18, 1e300, -1e300,
                Double.MIN_VALUE, -Double.MIN_VALUE, Math.PI, -Math.PI, 1e-9, -1e-9));
        SplittableRandom rng = new SplittableRandom(0x4D43504FL);
        for (int i = 0; i < 20000; i++) {
            switch (i % 4) {
                case 0 -> doubles.add(rng.nextDouble(-1e3, 1e3));
                case 1 -> doubles.add(rng.nextDouble(-1e12, 1e12));
                case 2 -> doubles.add(Double.longBitsToDouble(rng.nextLong()));
                default -> doubles.add((double) rng.nextInt() + rng.nextDouble());
            }
        }

        try (BufferedWriter w = Files.newBufferedWriter(out.resolve("double_vectors.txt"))) {
            for (double d : doubles) {
                w.write(String.format("%016x %d %d %d %d %08x %08x%n",
                        Double.doubleToRawLongBits(d), (int) d, (long) d, floor(d), ceil(d),
                        Float.floatToRawIntBits(sin(d)), Float.floatToRawIntBits(cos(d))));
            }
        }

        try (BufferedWriter w = Files.newBufferedWriter(out.resolve("float_vectors.txt"))) {
            for (double d : doubles) {
                float f = (float) d;
                w.write(String.format("%08x %d %d%n", Float.floatToRawIntBits(f), (int) f, (long) f));
            }
        }
    }
}
