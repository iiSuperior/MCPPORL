# Oracle scenarios

Scripted inputs replayed by both the oracle harness (vanilla) and the
simulator. Each `.txt` file is one scenario:

```
start x=0.5 z=0.5          # optional; y is always the ground surface
10 idle                    # 10 ticks, no keys
40 W sprint yaw=45         # hold W + sprint, facing yaw 45
20 W sprint jump dyaw=2.5  # also hold jump, turning 2.5 degrees per tick
```

Keys: `W A S D jump sneak sprint` (or `idle`). Rotation: `yaw=`/`pitch=` set
the absolute angle from that segment on; `dyaw=`/`dpitch=` add degrees every
tick. Rotation is applied before each tick, as the client does.
