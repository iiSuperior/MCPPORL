import io.netty.buffer.ByteBuf;
import io.netty.buffer.Unpooled;
import java.io.BufferedWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.SplittableRandom;
import net.minecraft.network.LpVec3;
import net.minecraft.world.phys.Vec3;

/**
 * Round-trips vectors through the game's own LpVec3 (the velocity encoding of
 * ClientboundSetEntityMotionPacket) and writes "in -> out" as hex bits, one
 * vector per line. The file becomes a golden fixture for sim/include/mcp/lpvec3.hpp.
 *
 * Usage (game jar and libraries on the classpath): java LpVec3Probe <out-file>
 */
public final class LpVec3Probe {
    public static void main(String[] args) throws Exception {
        List<Vec3> vs = new ArrayList<>();
        double[] specials = {0.0, -0.0, 1e-6, 3.0519e-5, 3.052e-5, 0.1, -0.1, 0.4, -0.4, 0.5, 1.0, -1.0, 1.0000001,
                2.0, 3.9999, 4.0, 4.5, 100.0, 1e6, 1.7179869183E10, 2e10, Double.NaN};
        for (double a : specials) for (double b : specials) vs.add(new Vec3(a, b, -a * 0.5));
        SplittableRandom r = new SplittableRandom(0x4C505633L);
        for (int i = 0; i < 20000; i++) {
            switch (i % 4) {
                case 0 -> vs.add(new Vec3(r.nextDouble(-1, 1), r.nextDouble(-1, 1), r.nextDouble(-1, 1)));  // knockback range
                case 1 -> vs.add(new Vec3(r.nextDouble(-4, 4), r.nextDouble(-4, 4), r.nextDouble(-4, 4)));
                case 2 -> vs.add(new Vec3(r.nextDouble(-0.01, 0.01), r.nextDouble(-0.5, 0.5), r.nextDouble(-0.01, 0.01)));
                default -> vs.add(new Vec3(r.nextDouble(-1e4, 1e4), r.nextDouble(-50, 50), r.nextDouble(-1e-3, 1e-3)));
            }
        }
        long bad = 0;
        try (BufferedWriter w = Files.newBufferedWriter(Path.of(args[0]))) {
            for (Vec3 v : vs) {
                ByteBuf buf = Unpooled.buffer();
                LpVec3.write(buf, v);
                Vec3 o = LpVec3.read(buf);
                if (buf.readableBytes() != 0) bad++;
                w.write(String.format("%016x %016x %016x %016x %016x %016x%n",
                        Double.doubleToRawLongBits(v.x), Double.doubleToRawLongBits(v.y), Double.doubleToRawLongBits(v.z),
                        Double.doubleToRawLongBits(o.x), Double.doubleToRawLongBits(o.y), Double.doubleToRawLongBits(o.z)));
            }
        }
        System.out.printf("LPVEC3 PROBE: %d vectors, %d with leftover bytes%n", vs.size(), bad);
        System.exit(bad == 0 ? 0 : 1);
    }
}
