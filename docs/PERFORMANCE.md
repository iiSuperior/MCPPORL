# Performance budget

End-to-end training throughput, from simulator ticks to PPO updates.
**Measured** numbers come from `sim/tools/bench_tick.cpp`; everything else
is an estimate to be replaced by measurements on the training machine
(RX 9070 XT).

## Summary

| Stage | Throughput | Status |
|---|---|---|
| Simulator, CPU, one thread (movement only) | ~4.8 M player-ticks/s | Measured with `bench_tick`, Release build (2.1 GHz Xeon cloud core, 2026-09-29) |
| Simulator, CPU, one thread (movement only) | ~6.5 M player-ticks/s | Measured with `bench_tick` on the training desktop, Ryzen 9 9950X3D at ~4.3 GHz, Ninja Release build (2026-09-29) |
| Simulator, CPU, all 16 cores (movement only) | ~100 M player-ticks/s | Estimate (16 x single-thread) |
| Simulator, CPU, one thread (with combat, items, blocks) | ~0.5–1.6 M player-ticks/s | Estimate (3–10x costlier ticks) |
| Simulator, GPU | ~50–500 M player-ticks/s | Estimate |
| Rollout inference (~1M-parameter policy) | ~1–3 M agent-samples/s | Estimate |
| **End to end (rollout + PPO update)** | **~150k–500k agent-samples/s** | Estimate |

The simulator is not the bottleneck; the policy network is. Plan around the
lower half of the end-to-end range until measured.

## Simulator

### Bit-exactness across machines

The state hash `a17145a8224fa1e7` has been reproduced on: this Linux cloud
container (GCC), GitHub's Ubuntu runner (GCC), GitHub's Windows runner (MSVC,
`/fp:precise`) and the training desktop (Windows, 9950X3D). CI enforces it
through the `bench_hash` test.

### Why the CPU numbers scale weakly with clock speed

The desktop is ~1.3x the cloud core, matching the real clock ratio (cloud
Xeons turbo well above their 2.1 GHz base). The benchmark is dominated by
unpredictable branches (random inputs) and the per-tick collision scan of
~36 blocks, not arithmetic. A flat-world fast path would be a 5-10x win if
the CPU path ever matters, but the GPU backend is the target.

Run the benchmark:

```sh
cmake --build sim/build --config Release --target bench_tick
./sim/build/bench_tick sim/data/sin_table.bin        # Windows: sim\build\Release\bench_tick.exe
```

It ticks 4096 independent players on flat ground with random but
reproducible inputs (forward, strafe, sprint, occasional jumps, random yaw)
on one thread, then prints a 64-bit hash of every player's exact state bits.
With the default arguments the hash must be **`a17145a8224fa1e7`** on every
machine and compiler; a different hash means the simulation is not
bit-identical there (for example, fused multiply-add was enabled).

### GPU notes

- Minecraft's physics state is `double`, and bit-exact parity rules out
  switching to `float`. Consumer RDNA GPUs run FP64 at a small fraction of
  FP32 rate (roughly 1/32; about 1–1.5 TFLOPS on a 9070 XT, to be confirmed
  against the spec sheet). A tick needs only a few hundred double operations,
  so FP64 still leaves the simulator far faster than the policy.
- Expect divergence costs (players in different states take different
  branches) and memory-bound collision queries; the GPU estimate is wide for
  that reason.

## Policy and PPO

Assumptions for the estimate:

- Policy of ~1M parameters (MLP, possibly with a recurrent core).
- Forward pass ~2 FLOP per parameter, so ~2 MFLOP per sample for rollouts.
- PPO update ~6 FLOP per parameter per epoch (forward and backward), 4
  epochs, so ~24 MFLOP per sample.
- Realistic small-batch efficiency on RDNA4 under PyTorch-ROCm of roughly
  10–20% of peak half-precision matrix throughput.

Reference point: PufferLib reports about 1M steps/s for small networks on an
RTX 4090. ROCm on Windows is less mature, hence the more conservative range.

## What the numbers mean in game time

At 300k agent-samples/s with two agents per duel: 150k duel-ticks/s, about
7,500x real time, or roughly 20 years of continuous duels per day of
training.

Rough and highly uncertain sample budgets: a competent sword duelist on the
order of 10^8–10^9 samples (hours); full-kit mastery 10^10 or more (days to
weeks).

## Rules for keeping throughput high

1. Keep the whole loop on the GPU. The simulator writes observations straight
   into a PyTorch tensor through DLPack; no per-tick host round trip.
2. Keep the policy small until it plateaus. Parameter count sets the speed.
3. Run thousands of arenas in parallel (4096–16384) so the GPU stays busy.
4. Verify PyTorch-ROCm on Windows with the 9070 XT early. It is the largest
   unknown in the stack. WSL2 is the fallback, and the simulator's HIP code is
   deliberately independent of PyTorch.

## To measure on the training machine

- [ ] `bench_tick` on the desktop CPU (single thread, then all cores)
- [ ] GPU simulator throughput at 4096 / 16384 arenas
- [ ] Rollout-only throughput (policy forward + simulator)
- [ ] Full PPO loop throughput
- [ ] Time to first competent sword duelist
