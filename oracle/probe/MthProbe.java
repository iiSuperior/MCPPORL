import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

/**
 * Calls the game's real net.minecraft.util.Mth on the vectors produced by
 * oracle/jref/JRef.java and reports any difference. JRef encodes our assumed
 * formulas; this checks those assumptions against the actual jar.
 *
 * Usage (game jar and its libraries on the classpath):
 *   java -cp "server.jar:libs/*:." MthProbe <jref-out-dir>
 */
public final class MthProbe {
    public static void main(String[] args) throws Exception {
        Class<?> mth = Class.forName("net.minecraft.util.Mth");
        System.out.println("== Mth public static methods named sin/cos/floor/ceil ==");
        Method[] methods = mth.getDeclaredMethods();
        Arrays.sort(methods, Comparator.comparing(Method::toString));
        for (Method m : methods) {
            if (Modifier.isStatic(m.getModifiers()) && List.of("sin", "cos", "floor", "ceil").contains(m.getName())) {
                System.out.println("  " + m);
            }
        }

        Method sin = find(mth, "sin"), cos = find(mth, "cos");
        Method floor = mth.getMethod("floor", double.class);
        Method ceil = mth.getMethod("ceil", double.class);
        System.out.println("using sin=" + sin + " cos=" + cos);

        long n = 0, bad = 0;
        for (String line : Files.readAllLines(Path.of(args[0], "double_vectors.txt"))) {
            String[] p = line.trim().split(" ");
            double d = Double.longBitsToDouble(Long.parseUnsignedLong(p[0], 16));
            int expFloor = Integer.parseInt(p[3]), expCeil = Integer.parseInt(p[4]);
            int expSin = Integer.parseUnsignedInt(p[5], 16), expCos = Integer.parseUnsignedInt(p[6], 16);
            int gotFloor = (int) floor.invoke(null, d), gotCeil = (int) ceil.invoke(null, d);
            int gotSin = Float.floatToRawIntBits((float) call(sin, d));
            int gotCos = Float.floatToRawIntBits((float) call(cos, d));
            n++;
            if (gotFloor != expFloor || gotCeil != expCeil || gotSin != expSin || gotCos != expCos) {
                if (++bad <= 20) {
                    System.out.printf("MISMATCH d=%s floor %d/%d ceil %d/%d sin %08x/%08x cos %08x/%08x%n",
                            p[0], gotFloor, expFloor, gotCeil, expCeil, gotSin, expSin, gotCos, expCos);
                }
            }
        }
        System.out.printf("MTH PROBE: %d vectors, %d mismatches (game vs our formulas)%n", n, bad);
        System.exit(bad == 0 ? 0 : 1);
    }

    // Prefer the double overload; fall back to float for older versions.
    private static Method find(Class<?> c, String name) throws NoSuchMethodException {
        try {
            return c.getMethod(name, double.class);
        } catch (NoSuchMethodException e) {
            return c.getMethod(name, float.class);
        }
    }

    private static Object call(Method m, double d) throws Exception {
        return m.getParameterTypes()[0] == double.class ? m.invoke(null, d) : m.invoke(null, (float) d);
    }
}
