# Splash / Lingering Potions — 26.3 mechanics

Source of truth: decompiled vanilla 26.3 client jar (Vineflower). Citations are
`[Class.method]`. Everything below was read from decompiled bodies unless marked
UNVERIFIED.

## Summary

- Throwing a splash/lingering potion runs `SplashPotionItem.use` /
  `LingeringPotionItem.use` → `ThrowablePotionItem.use`, which **only on the
  server** calls
  `Projectile.spawnProjectileFromRotation(this::createPotion, serverLevel, itemStack, player, -20.0F, 0.5F, 1.0F)`.
  Power `0.5F` (`PROJECTILE_SHOOT_POWER`), uncertainty `1.0F`, y-offset `-20.0F`.
  [ThrowablePotionItem.use]
- The inaccuracy draw uses **`this.random` of the thrown-potion entity itself**
  (`Projectile.getMovementToShoot` → `this.random.triangle(0.0, 0.0172275 * uncertainty)` ×3),
  and each `Entity`'s `random` is `RandomSource.create()` — a
  non-deterministically seeded `LegacyRandomSource`. Thrown-potion spread is
  therefore **not bit-for-bit reproducible** across runs.
  [Projectile.getMovementToShoot, Entity (field), RandomSource.create]
- Thrown potions: gravity `0.05` per tick, air drag `0.99F`, water drag `0.8F`.
  [AbstractThrownPotion.getDefaultGravity, ThrowableProjectile.applyInertia/getAirDrag]
- Splash hit (`ThrownSplashPotion.onHitAsPotion`): box centered on the hit
  location, inflated `(4.0, 2.0, 4.0)`; entities with `dist < 16.0`;
  `scale = 1.0 - Math.sqrt(dist) / 4.0`; duration effects get
  `(int)(scale * d * durationScale + 0.5)` and are **skipped if `duration <= 20`**;
  instant effects use `scale` directly. Harming: `(int)(scale * (6 << amplification) + 0.5)`
  → 6.0 / 12.0 at point blank. [ThrownSplashPotion.onHitAsPotion, HealOrHarmMobEffect.applyInstantaneousEffect]
- Lingering hit spawns `AreaEffectCloud`: radius `3.0F`, `radiusOnUse -0.5F`,
  duration `600`, waitTime `10`, `radiusPerTick = -3.0F / 600 = -0.005F`;
  effect application every 5 ticks, per-victim reapplication delay 20 ticks;
  cloud instant effects use fixed scale `0.5`. [ThrownLingeringPotion.onHitAsPotion, AreaEffectCloud.serverTick]

## Data

### Throw / flight constants

| Name | Value | Location |
|---|---|---|
| `ThrowablePotionItem.PROJECTILE_SHOOT_POWER` | `0.5F` | `ThrowablePotionItem` field |
| spawn args `(yOffset, pow, uncertainty)` | `-20.0F, 0.5F, 1.0F` | `ThrowablePotionItem.use` |
| inaccuracy triangle half-width | `0.0172275 * uncertainty` (double) | `Projectile.getMovementToShoot` |
| `AbstractThrownPotion.getDefaultGravity` | `0.05` | override |
| `ThrowableProjectile.getAirDrag` | `0.99F` | override (Entity default `0.98F`) |
| water drag (inertia) | `0.8F` | `ThrowableProjectile.applyInertia` |
| throw sound pitch | `0.4F / (level.getRandom().nextFloat() * 0.4F + 0.8F)`, volume `0.5F` | `SplashPotionItem.use`, `LingeringPotionItem.use` |

### Splash application constants

| Name | Value | Location |
|---|---|---|
| `AbstractThrownPotion.SPLASH_RANGE` | `4.0` (double) | field (method uses literal `4.0`) |
| `SPLASH_RANGE_SQ` | `16.0` | field (method uses literal `16.0`) |
| AABB inflate | `(4.0, 2.0, 4.0)` | `ThrownSplashPotion.onHitAsPotion` |
| distance cutoff | `dist < 16.0` (squared) | `ThrownSplashPotion.onHitAsPotion` |
| falloff | `scale = 1.0 - Math.sqrt(dist) / 4.0` | `ThrownSplashPotion.onHitAsPotion` |
| duration scale formula | `(int)(scale * d * durationScale + 0.5)` | `ThrownSplashPotion.onHitAsPotion` |
| duration skip gate | `!newEffect.endsWithin(20)` → skipped when `duration <= 20` | `ThrownSplashPotion.onHitAsPotion`, `MobEffectInstance.endsWithin` |
| `POTION_DURATION_SCALE` default | `1.0F` | `ThrownSplashPotion.onHitAsPotion` |
| splash margin | `Math.max(0.0F, Math.min(0.3F, (source.tickCount - 2) / 20.0F))` | `ProjectileUtil.computeMargin` |
| water-splash vs water-sensitive | `1.0F` indirectMagic | `AbstractThrownPotion.affectEntitiesAround` |

### Harming / healing instant effect

| Case | Formula | Result at scale 1.0 |
|---|---|---|
| harm (isHarm != inverted) | `(int)(scale * (6 << amplification) + 0.5)` | amp 0 → 6, amp 1 → 12 |
| heal (isHarm == inverted) | `(int)(scale * (4 << amplification) + 0.5)` | amp 0 → 4, amp 1 → 8 |

[HealOrHarmMobEffect.applyInstantaneousEffect]. Damage source: `indirectMagic(source, owner)` when
source non-null, else `magic()`. Splash passes `(level, this, this.getOwner(), entity, amp, scale)`; cloud passes
`(serverLevel, this, this.getOwner(), entity, amp, 0.5)`.

### Lingering cloud constants

| Name | Value | Location |
|---|---|---|
| initial radius | `3.0F` | `ThrownLingeringPotion.onHitAsPotion` (`cloud.setRadius(3.0F)`) |
| `radiusOnUse` | `-0.5F` | `ThrownLingeringPotion.onHitAsPotion` |
| `duration` | `600` | `ThrownLingeringPotion.onHitAsPotion` |
| `waitTime` | `10` | `ThrownLingeringPotion.onHitAsPotion` |
| `radiusPerTick` | `-cloud.getRadius() / cloud.getDuration()` = `-0.005F` | `ThrownLingeringPotion.onHitAsPotion` |
| discard radius | `< 0.5F` (`MINIMAL_RADIUS`) | `AreaEffectCloud.serverTick` |
| discard age | `tickCount - waitTime >= duration` (i.e. tickCount 610) | `AreaEffectCloud.serverTick` |
| application cadence | `tickCount % 5 == 0` | `AreaEffectCloud.serverTick` |
| reapplication delay | `20` ticks (`DEFAULT_REAPPLICATION_DELAY`) | `AreaEffectCloud` field |
| cloud instant-effect scale | `0.5` (fixed) | `AreaEffectCloud.serverTick` |
| cloud hitbox | `EntityDimensions.scalable(radius * 2.0F, 0.5F)` | `AreaEffectCloud.getDimensions` |
| cloud effect radius check | `dist <= radius * radius` (XZ only) | `AreaEffectCloud.serverTick` |

Derived: shrink rate `-0.005F`/tick from `3.0F` hits `0.5F` after 500 active ticks
(tickCount 510), before the age cap (610). Each victim hit also applies
`radiusOnUse -0.5F`. Reapplication every 20 ticks per entity.

## Logic (ordered)

### Throw (`use`)

1. `SplashPotionItem.use` / `LingeringPotionItem.use`: `level.playSound(...)` with
   pitch `0.4F / (level.getRandom().nextFloat() * 0.4F + 0.8F)` (cosmetic), then
   `super.use` → `ThrowablePotionItem.use`. [SplashPotionItem.use]
2. `ThrowablePotionItem.use`: `itemStack = player.getItemInHand(hand)`;
   if `level instanceof ServerLevel`: `Projectile.spawnProjectileFromRotation(this::createPotion, serverLevel, itemStack, player, -20.0F, 0.5F, 1.0F)`.
   Then `player.awardStat(...)`, `itemStack.consume(1, player)`, return
   `InteractionResult.SUCCESS`. Client side: no spawn, still consumes. [ThrowablePotionItem.use]
3. `spawnProjectileFromRotation` → `creator.create(serverLevel, source, itemStack)`
   (the `ThrownSplashPotion(level, owner, itemStack)` ctor) →
   `projectile.shootFromRotation(source, source.getXRot(), source.getYRot(), -20.0F, 0.5F, 1.0F)`
   → `shoot(xd, yd, zd, pow, uncertainty)` → `getMovementToShoot(...)`:
   `new Vec3(xd, yd, zd).normalize().add(triangle×3).scale(pow)`,
   then `setDeltaMovement(movement)` and add shooter's known movement
   (`sourceMovement.x`, `source.onGround() ? 0.0 : sourceMovement.y`, `sourceMovement.z`).
   [Projectile.spawnProjectileFromRotation, Projectile.shootFromRotation, Projectile.shoot, Projectile.getMovementToShoot]
4. `serverLevel.addFreshEntity(projectile)`, `projectile.applyOnProjectileSpawned(serverLevel, itemStack)`.
   Dispenser path: `ThrowablePotionItem.createDispenseConfig` → `uncertainty(DEFAULT*0.5F)`,
   `power(DEFAULT*1.25F)`. [Projectile.spawnProjectile, ThrowablePotionItem.createDispenseConfig]

### Flight tick (`ThrowableProjectile.tick`)

Order per tick: `handleFirstTickBubbleColumn()` → `applyGravity()` →
`applyInertia()` → `ProjectileUtil.getHitResultOnMoveVector(this, this::canHitEntity)` →
`setPos(hit ? hitLocation : position + deltaMovement)` → `updateRotation()` →
`applyEffectsFromBlocks()` → `super.tick()` → if hit and alive:
`hitTargetOrDeflectSelf(result)`. [ThrowableProjectile.tick]

- `applyGravity`: `deltaMovement.y -= getDefaultGravity()` = `0.05` (no-gravity flag respected).
  [Entity.applyGravity, AbstractThrownPotion.getDefaultGravity]
- `applyInertia`: in water → 4 bubble particles + `inertia = 0.8F`; else
  `inertia = getAirDrag()` = `0.99F`; `setDeltaMovement(movement.scale(inertia))`.
  [ThrowableProjectile.applyInertia]

### Impact (`AbstractThrownPotion.onHit`) — server only

1. `super.onHit(hitResult)`.
2. `affectEntitiesAround(level, potion)`:
   - AABB = potion bbox `.inflate(4.0, 2.0, 4.0)`.
   - If `HURTS_WATER_SENSITIVE_ENTITIES` (e.g. plain water splash): water-sensitive
     entities with `dist < 16.0` take `1.0F` `indirectMagic(this, this.getOwner())`.
   - If `EXTINGUISHES_ENTITIES`: entities on fire within 16.0 → `extinguishFire()`.
   - If `REHYDRATES_AXOLOTLS`: axolotls in AABB → `rehydrate()`.
   [AbstractThrownPotion.affectEntitiesAround]
3. If `potion.hasEffects()` → `onHitAsPotion(level, potionItemStack, hitResult)`
   (abstract; splash vs lingering below).
4. `levelEvent(2007, ...)` + `1054` if potion `hasInstantEffects()`, else `2002` + `1053`
   (particle/glass-break events; silent potions skip the sound event).
5. `this.discard()`. [AbstractThrownPotion.onHit]
6. `onHitBlock` (block hits): if `DOUSES_FIRE` tag: `douseFire` at
   `blockHitPos.relative(hitDirection)`, its opposite, and all 4 horizontal
   neighbors — destroys fire blocks, extinguishes lit candles/campfires.
   [AbstractThrownPotion.onHitBlock]

### Splash effect application (`ThrownSplashPotion.onHitAsPotion`)

1. `durationScale = potionItem.getOrDefault(DataComponents.POTION_DURATION_SCALE, 1.0F)`.
2. `potionAabb = this.getBoundingBox().move(hitResult.getLocation().subtract(this.position()))`
   (potion-sized box recentered on the exact hit location);
   `effectAabb = potionAabb.inflate(4.0, 2.0, 4.0)`;
   entities = all `LivingEntity` in `effectAabb`; `margin = ProjectileUtil.computeMargin(this)`.
3. Per entity with `isAffectedByPotions()`: `dist = potionAabb.distanceToSqr(entity.getBoundingBox().inflate(margin))`;
   if `dist < 16.0`: `scale = 1.0 - Math.sqrt(dist) / 4.0`.
4. Per effect instance:
   - instantaneous: `effect.value().applyInstantaneousEffect(level, this, this.getOwner(), entity, amp, scale)`.
   - duration: `duration = effectInstance.mapDuration(d -> (int)(scale * d * durationScale + 0.5))`;
     new instance with same effect/amp/ambient/visible; applied via
     `entity.addEffect(newEffect, effectSource)` **only if `!newEffect.endsWithin(20)`**
     (duration > 20 ticks).
   - `effectSource = this.getEffectSource()` = `MoreObjects.firstNonNull(this.getOwner(), this)`
     [Projectile.getEffectSource].
   [ThrownSplashPotion.onHitAsPotion]

PvP consequence: a splash poison II (1:00 = 1200 ticks) at scale 0.5 gives
`(int)(0.5*1200+0.5) = 600` ticks; at scale ≤ ~0.016 the scaled duration drops
to ≤ 20 and the effect is silently skipped.

### Lingering effect application (`ThrownLingeringPotion.onHitAsPotion`)

1. `cloud = new AreaEffectCloud(level, x, y, z)` at the hit **entity's** position if
   `EntityHitResult`, else the potion's position.
2. If owner is `LivingEntity`: `cloud.setOwner(livingEntity)`.
3. `setRadius(3.0F)`; `setRadiusOnUse(-0.5F)`; `setDuration(600)`;
   `setWaitTime(10)`; `setRadiusPerTick(-cloud.getRadius() / cloud.getDuration())`
   (= `-0.005F`); `cloud.applyComponentsFromItemStack(potionItem)`
   (copies `POTION_CONTENTS` + `POTION_DURATION_SCALE`); `level.addFreshEntity(cloud)`.
   [ThrownLingeringPotion.onHitAsPotion]

### Cloud tick (`AreaEffectCloud.serverTick`)

1. `super.tick()`; server: `if (duration != -1 && tickCount - waitTime >= duration) discard()`.
2. `shouldWait = tickCount < waitTime`; sync `DATA_WAITING`.
3. If not waiting:
   - `radius += radiusPerTick` (`-0.005F`); if `radius < 0.5F` → `discard()`.
   - If `tickCount % 5 == 0`: prune `victims` entries with `tickCount >= value`;
     if potion has effects: `allEffects` = contents `forEachEffect(add, potionDurationScale)`;
     for each `LivingEntity` in `this.getBoundingBox()` (radius×2 wide, 0.5 high)
     not in `victims`, `isAffectedByPotions()`, `canBeAffected` by some effect,
     with XZ `dist <= radius*radius`:
     - `victims.put(entity, tickCount + reapplicationDelay)` (20).
     - instantaneous: `applyInstantaneousEffect(serverLevel, this, this.getOwner(), entity, amp, 0.5)`.
     - duration: `entity.addEffect(new MobEffectInstance(effect), this)` — full duration.
     - `radius += radiusOnUse` (`-0.5F` per victim); `if (radius < 0.5F) discard()`.
     - `durationOnUse` is 0 for thrown lingerings (no-op).
   [AreaEffectCloud.serverTick, AreaEffectCloud.getDimensions]

## Tick placement

- Throw spawn happens inside the `use` item-use packet handling (server thread),
  same tick as the swing/use packet. The projectile entity is added via
  `addFreshEntity` and ticks starting the **next** entity tick.
- `ThrowableProjectile.tick` order per tick: gravity → inertia(drag) →
  collision raycast on the move vector → position set → rotation →
  `applyEffectsFromBlocks` → `super.tick()` (Entity.baseTick: fluid/fire/etc.) →
  `hitTargetOrDeflectSelf` on hit. Note: impact effects run **after**
  `super.tick()` within the same tick, so the potion is discarded the same tick
  the hit is detected. [ThrowableProjectile.tick]
- Cloud: `tickCount` starts at 0 on spawn; `waitTime 10` means no effects before
  tick 10; `tickCount % 5 == 0` gates application (ticks 10, 15, 20, …);
  victims re-eligible every 20 ticks.
- Throw sound pitch draw happens during `use()` (both sides, cosmetic).

## Randomness

| Draw | Exact RandomSource expression | Bit-for-bit reproducible? |
|---|---|---|
| Throw inaccuracy ×3 (`triangle`) | `this.random` in `Projectile.getMovementToShoot` — the **projectile entity's own** `RandomSource`, field `protected final RandomSource random = RandomSource.create()` = `create(RandomSupport.generateUniqueSeed())` → `new LegacyRandomSource(seed)` | **No** — seeded non-deterministically at entity construction. Two identical throws diverge. |
| Throw sound pitch | `level.getRandom().nextFloat()` in `SplashPotionItem.use` / `LingeringPotionItem.use` | Cosmetic only; level RNG is world-seeded but throw timing varies — not reproducible in practice. |
| Splash/lingering effect application | none | **Yes** — fully deterministic given positions, tickCount, contents. |
| Cloud client particles | `this.random` of the `AreaEffectCloud` entity | Cosmetic, client-side; no. |
| Cloud shrink / victim timing | none | **Yes.** |

Note: the triangle draws are the *only* randomness in the throw path, and they
come from the projectile's per-entity RNG — not the shooter, not the level.

## Proposed oracle scenarios (130+)

- **130_splash_harming_direct_hit**: thrower and target stationary, throw splash
  harming II point-blank at feet. Expect `dist≈0`, `scale≈1.0`,
  `(int)(1.0*(6<<1)+0.5) = 12` magic damage via `indirectMagic(potion, owner)`.
  (Inaccuracy is non-deterministic — scenario must fix the potion's spawn
  velocity directly rather than simulating the throw, or accept tolerance.)
- **131_splash_poison_falloff_edge**: splash poison (0:45 = 900 ticks) thrown so
  target sits at `sqrt(dist) = 3.0` → `scale = 0.25` → duration
  `(int)(0.25*900+0.5) = 225` ticks applied; amplifier unchanged.
- **132_splash_duration_skip_gate**: target at `sqrt(dist) = 3.92` →
  `scale = 0.02` → `(int)(0.02*900+0.5) = 18` ≤ 20 → no effect applied at all.
- **133_splash_harming_cloud? no — lingering_harming_cloud**: lingering harming I
  lands; cloud at tick 10+ applies `(int)(0.5*6+0.5) = 3` per application,
  reapplication every 20 ticks; verify radius shrink `-0.005F`/tick and discard
  when radius `< 0.5F` (≈ tick 510).
- **134_lingering_radius_on_use**: two entities inside cloud on the same
  5-tick application tick → radius drops `2 × 0.5F = 1.0F` that tick.
- **135_splash_water_extinguish_and_hurt**: water splash on a burning zombie and
  a blaze within 4 blocks: zombie `extinguishFire()`; blaze takes `1.0F`
  `indirectMagic`; both from `affectEntitiesAround`, independent of potion effects.
- **136_throw_velocity_construction**: unit test of `getMovementToShoot` math
  only: normalized direction, no inaccuracy (`uncertainty = 0.0F`), scaled by
  `pow = 0.5F`, plus shooter known-movement added (y only if shooter airborne).

## Open questions

- `PotionContents.getColor()` / `hasInstantEffects()` internals were not read —
  only the call sites; particle color literals not pinned.
- Exact `DispenseConfig.DEFAULT` uncertainty/power values (used only for
  dispenser-fired potions: `*0.5F`, `*1.25F` multipliers verified).
- `PotionContents.forEachEffect(adder, potionDurationScale)` scaling semantics
  (used for cloud duration effects) assumed linear; not verified.
- `Axolotl.rehydrate()` internals not read (non-PvP).
- Whether `isAffectedByPotions()` excludes anything beyond the obvious
  (undead/witches?) not verified in this pass.
