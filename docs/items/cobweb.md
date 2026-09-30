# Cobweb (WebBlock) — 26.3 mechanics

Source of truth: decompiled vanilla 26.3 client jar (Vineflower). Citations are
`[Class.method]`. Everything below was read from decompiled bodies unless marked
UNVERIFIED.

## Summary

- `WebBlock.entityInside` passes an exact multiplier `Vec3` to
  `entity.makeStuckInBlock(state, speedMultiplier)`:
  - normal: `new Vec3(0.25, 0.05F, 0.25)`
  - with WEAVING effect: `new Vec3(0.5, 0.25, 0.5)`
  [WebBlock.entityInside]
- `Entity.makeStuckInBlock` = `resetFallDistance()` + `stuckSpeedMultiplier = speedMultiplier`.
  [Entity.makeStuckInBlock]
- The multiplier is **not** applied the tick it is set. Hook path per server tick:
  `LivingEntity.tick` → travel → `Entity.move` (consumes last tick's multiplier) →
  … → `applyEffectsFromBlocks()` → `checkInsideBlocks` → `WebBlock.entityInside`
  → `makeStuckInBlock` (sets multiplier for the *next* tick's `move`).
  [LivingEntity (tick travel section), Entity.move, Entity.checkInsideBlocks]
- On consumption: `delta = delta.multiply(stuckSpeedMultiplier)`,
  `stuckSpeedMultiplier = Vec3.ZERO`, `setDeltaMovement(Vec3.ZERO)`.
  Piston movement is exempt from the multiplier. [Entity.move]
- **No randomness anywhere in the cobweb path** (verified: no `random` draws in
  `WebBlock.entityInside`, `makeStuckInBlock`, or the `move` consumption block).

## Data

| Name | Value | Location |
|---|---|---|
| normal multiplier | `new Vec3(0.25, 0.05F, 0.25)` (mixed double/float literals) | `WebBlock.entityInside` |
| WEAVING multiplier | `new Vec3(0.5, 0.25, 0.5)` | `WebBlock.entityInside` |
| consume gate | `stuckSpeedMultiplier.lengthSqr() > 1.0E-7` | `Entity.move` |
| movement delta | `delta.multiply(stuckSpeedMultiplier)` (component-wise) | `Entity.move` |
| post-consume state | `stuckSpeedMultiplier = Vec3.ZERO`; `setDeltaMovement(Vec3.ZERO)` | `Entity.move` |
| piston exemption | multiplier skipped when `moverType == MoverType.PISTON` (still reset to ZERO) | `Entity.move` |
| fall distance | `resetFallDistance()` on `makeStuckInBlock` | `Entity.makeStuckInBlock` |

## Logic (ordered)

### Setting the multiplier (end of tick N)

1. `LivingEntity.tick` runs the AI/travel section: `travel(...)` →
   `Entity.move(MoverType.SELF, delta)`. [LivingEntity tick, Entity.move]
2. After travel, still in the same tick: `if (!level.isClientSide() || isLocalInstanceAuthoritative()) this.applyEffectsFromBlocks();`
   [LivingEntity tick ~line 3128]
3. `applyEffectsFromBlocks()` → `applyEffectsFromBlocks(finalMovementsThisTick)` →
   `checkInsideBlocks(movements, insideEffectCollector)` →
   per intersected block: `state.entityInside(level, pos, entity, effectCollector, isPrecise)`.
   [Entity.applyEffectsFromBlocks, Entity.checkInsideBlocks]
4. `WebBlock.entityInside(state, level, pos, entity, effectApplier, isPrecise)`:
   `Vec3 speedMultiplier = new Vec3(0.25, 0.05F, 0.25)`;
   if `entity instanceof LivingEntity && livingEntity.hasEffect(MobEffects.WEAVING)`:
   `speedMultiplier = new Vec3(0.5, 0.25, 0.5)`;
   `entity.makeStuckInBlock(state, speedMultiplier)`.
   [WebBlock.entityInside]
5. `Entity.makeStuckInBlock`: `this.resetFallDistance(); this.stuckSpeedMultiplier = speedMultiplier;`
   [Entity.makeStuckInBlock]

The `insideEffectCollector` (`StepBasedCollector`) applies effects after the
block walk; `checkInsideBlocks` uses the entity's recorded movements this tick
(`finalMovementsThisTick`), intersecting the (deflated) bounding box along the
path — so the web must actually overlap the entity's swept box for the effect
to trigger. [Entity.checkInsideBlocks]

### Consuming the multiplier (start of tick N+1)

In `Entity.move`, before piston limiting and collision:

```java
if (this.stuckSpeedMultiplier.lengthSqr() > 1.0E-7) {
   if (moverType != MoverType.PISTON) {
      delta = delta.multiply(this.stuckSpeedMultiplier);
   }
   this.stuckSpeedMultiplier = Vec3.ZERO;
   this.setDeltaMovement(Vec3.ZERO);
}
```
[Entity.move]

Effects:
- The movement delta for the tick is component-wise scaled: horizontal ×0.25,
  vertical ×0.05 (×0.5/×0.25 with WEAVING).
- The entity's `deltaMovement` is zeroed immediately — any accumulated velocity
  (e.g. from gravity applied during travel before `move`) is discarded that tick.
- The multiplier is single-use: it is reset to `Vec3.ZERO` whether or not this
  `move` call was piston-driven, and re-armed only if the entity is still inside
  a web at the end of the next tick.

Net behavior: entering a web on tick N sets the multiplier; the slowdown first
applies to movement on tick N+1. While continuously inside the web the
multiplier is re-armed every tick, so movement stays scaled every tick. Falling
into a web: `resetFallDistance()` prevents fall damage, and the ×0.05 vertical
scale plus zeroed `deltaMovement` kills downward velocity each tick.

## Tick placement

Per server tick for a `LivingEntity` in a web:

1. `travel` computes input-driven `deltaMovement` (walk/fly/swim).
2. `Entity.move`: stuck multiplier from **previous** tick applied to `delta`;
   `deltaMovement` zeroed.
3. Collision, position update.
4. `applyEffectsFromBlocks` → web re-arms `stuckSpeedMultiplier` for next tick
   (and resets fall distance again).
5. Everything downstream (e.g. `updateSwimming`, baseTick fire handling) sees
   the slowed position.

Client side: `applyEffectsFromBlocks()` is called only `if (!level.isClientSide()
|| this.isLocalInstanceAuthoritative())` — for a normal remote player the server
is authoritative and applies the effect; the client predicts via its own
movement code. [LivingEntity tick]

## Randomness

| Draw | Exact RandomSource expression | Bit-for-bit reproducible? |
|---|---|---|
| (none) | — | **Yes** — `WebBlock.entityInside`, `Entity.makeStuckInBlock`,
and the `Entity.move` consumption block contain zero random draws (verified by
reading all three bodies). Cobweb slowdown is fully deterministic given
positions, tick phase, and the WEAVING effect flag. |

## Proposed oracle scenarios (140+)

- **140_web_slow_walk**: player walks into a web at full sprint; next tick's
  `move` delta = computed delta × `(0.25, 0.05, 0.25)`; `deltaMovement` zeroed;
  fall distance reset. Verify one-tick delay: the entry tick itself is unslowed.
- **141_web_fall_arrest**: player falls 20 blocks into a web; on the arming tick
  `resetFallDistance()` fires and subsequent vertical delta ×`0.05F`; no fall
  damage on landing inside.
- **142_web_weaving**: same as 140 with WEAVING active → multiplier
  `(0.5, 0.25, 0.5)`; verify the effect check is `hasEffect(MobEffects.WEAVING)`.
- **143_web_piston_exempt**: entity inside web moved by piston — delta NOT
  scaled, but `stuckSpeedMultiplier` still reset to ZERO and `deltaMovement`
  zeroed.
- **144_web_exit**: entity leaves the web; the last armed multiplier still
  applies once on the first tick outside, then movement is free (multiplier not
  re-armed because `entityInside` no longer fires).

## Open questions

- `getEntityInsideCollisionShape` for `WebBlock` (whether the check uses the
  full block shape) was not read; assumed full-block from the entityInside call
  being reached.
- `StepBasedCollector` / `InsideBlockEffectApplier` deferral semantics not read
  in detail; the multiplier is set synchronously via `makeStuckInBlock` per the
  decompiled `WebBlock.entityInside`.
- Client prediction of web slowdown for the local player not traced (client
  movement code out of scope for this pass).
