# Combat scenarios

Two-player scripts for the combat oracle. Same keys as `oracle/scenarios`,
plus `attack` (one click on that tick, aimed at the other player). Each
segment line gives a tick count, then player A's inputs, `|`, player B's:

```
start A x=0.5 z=0.5 yaw=0        # optional placement per player
start B x=0.5 z=3.0 yaw=180
20 idle | idle                   # both idle 20 ticks (attack cooldown refills)
1  attack | idle                 # A clicks once
30 idle | idle                   # watch the knockback play out
```

`yaw=` on a segment is the rotation the client *sends* at the end of each of
those ticks. The server evaluates an attack with the rotation sent on the
previous tick, which is what the 180-hit scenario exercises.
