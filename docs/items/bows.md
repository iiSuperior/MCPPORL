# Bows and crossbows — vanilla 26.3 reference

## Summary

Both weapons are `ProjectileWeaponItem`s. The bow is draw-and-release: holding
draws for up to 20 ticks, and releasing fires one arrow whose speed is
`power * 3.0F` with `1.0F` inaccuracy; a full 20-tick draw crits. The crossbow
is charge-and-fire: charging takes 25 ticks (Quick Charge reduces it), then
firing is instant at `3.15F` arrow velocity, with Multishot firing 3 arrows at
0/−10/+10 degrees.

The single most important Phase 3 finding: **every projectile's inaccuracy
draws come from the projectile entity's own `Entity.random`**, which is
`RandomSource.create()` seeded by
`RandomSupport.generateUniqueSeed() = SEED_UNIQUIFIER * 1181783497276652981L ^ System.nanoTime()`
(`RandomSupport.generateUniqueSeed` [RandomSupport:41-42],
`Entity.random` field [Entity:267]). A live server therefore cannot reproduce
an arrow's exact trajectory bit for bit — the seed contains wall-clock time.
The deterministic oracle can only check projectile *invariants* (speed bounds,
damage formula structure, angles), never exact positions. Level-random draws
(bow/crossbow sound pitches) are reproducible if the simulator tracks the
server level's random state.

## Data

| Constant | Value | Source |
|---|---|---|
| Bow max draw | `MAX_DRAW_DURATION = 20` ticks | [BowItem:16] |
| Bow default range | `DEFAULT_RANGE = 15` | [BowItem:17] |
| Bow use duration | `72000` ticks | [BowItem.getUseDuration] |
| Draw → power | `pow = timeHeld / 20.0F; pow = (pow*pow + pow*2.0F) / 3.0F;` clamped `1.0F` | [BowItem.getPowerForTime] |
| Fire threshold | `pow < 0.1F` → no shot | [BowItem.releaseUsing] |
| Arrow velocity | `pow * 3.0F`, uncertainty `1.0F` | [BowItem.releaseUsing] |
| Crit | `pow == 1.0F` (i.e. `timeHeld >= 20`) | [BowItem.releaseUsing] |
| Arrow spawn pos | `(shooter.getX(), shooter.getEyeY() - 0.1F, shooter.getZ())` | [AbstractArrow ctor] |
| Arrow base damage | `baseDamage = 2.0` (double) | [AbstractArrow:77] |
| Arrow damage | `Mth.ceil(Mth.clamp(speed * arrowDamage, 0.0, 2.147483647E9))`, `speed = deltaMovement.length()` at impact | [AbstractArrow.onHitEntity] |
| Power enchant | `minecraft:damage` add linear `1.0 + 0.5*(lvl-1)` (arrows only) | data/minecraft/enchantment/power.json |
| Crit bonus | `dmgIncrease = this.random.nextInt(damage / 2 + 2)`; `damage = min(dmgIncrease + damage, 2147483647L)` | [AbstractArrow.onHitEntity] |
| Arrow gravity | `getDefaultGravity() = 0.05` | [AbstractArrow:346-347] |
| Arrow air drag | `0.99F` applied after the move when not in water | [AbstractArrow.getAirDrag / tick] |
| Arrow water drag | `0.6F` applied at tick start when in water | [AbstractArrow.getWaterInertia / tick] |
| Hit search box | arrow BB `expandTowards(deltaMovement).inflate(1.0)` | [AbstractArrow.findHitEntities] |
| Hit margin | `computeMargin = max(0.0F, min(0.3F, (tickCount - 2) / 20.0F))` | [ProjectileUtil:150-152] |
| Arrow despawn | `life >= 1200` ticks in ground | [AbstractArrow.tickDespawn] |
| Default arrow KB | `0.4F` along horizontal motion via `calculateHorizontalHurtKnockbackDirection` (arrow not in `NO_KNOCKBACK`) | [LivingEntity.dealDefaultKnockback] |
| Punch | `minecraft:knockback` add `1.0` per level; `mob.push(x, 0.1, z)` scaled `knockback * 0.6 * (1 - kbRes)` | [AbstractArrow.doKnockback], punch.json |
| Flame | `minecraft:projectile_spawned` → ignite duration `100.0`; burning arrow → `igniteForSeconds(5.0F)` on hit | flame.json, [AbstractArrow.onHitEntity] |
| Bow durability | 1 per arrow (`getDurabilityUse` default) | [ProjectileWeaponItem:80-82] |
| Crossbow charge | `floor(modifyCrossbowChargingTime(1.25F) * 20.0F)` = 25 ticks base | [CrossbowItem.getChargeDuration] |
| Quick Charge | `crossbow_charge_time` add `-0.25` per level → QC3: `1.25-0.75=0.5` → 10 ticks | quick_charge.json |
| Crossbow power | arrows `3.15F`, fireworks `1.6F`, mobs `1.6F` | [CrossbowItem:22-24] |
| Crossbow range | `DEFAULT_RANGE = 8` | [CrossbowItem:17] |
| Multishot spread | `projectile_spread` add `10.0`; angles `0, -10, +10` deg | multishot.json, [ProjectileWeaponItem.shoot] |
| Multishot durability | 1 per arrow, 3 per firework | [CrossbowItem.getDurabilityUse] |
| Bow shot pitch | `1.0F / (level.getRandom().nextFloat() * 0.4F + 1.2F) + pow * 0.5F` | [BowItem.releaseUsing] |
| Crossbow extra-shot pitch | index 0 → `1.0F`, else `1.0F / (shooter.getRandom().nextFloat() * 0.5F + 1.8F) + (0.63F\|0.43F)` | [CrossbowItem.getShotPitch] |
| Crossbow charge-end pitch | `1.0F / (level.getRandom().nextFloat() * 0.5F + 1.0F) + 0.2F` | [CrossbowItem.onUseTick] |

## Logic

**Bow shot** (`BowItem.releaseUsing`, server side only for the spawn):
1. `timeHeld = 72000 - remainingTime`; `pow = getPowerForTime(timeHeld)`; abort if
   `pow < 0.1F`.
2. `draw()` — on the server, `EnchantmentHelper.processProjectileCount` (Multishot
   n/a for bows); consumes 1 ammo unless Infinity/creative (`processAmmoUse`).
3. `shoot(serverLevel, player, hand, firedProjectiles, pow * 3.0F, 1.0F, pow == 1.0F, null)`:
   `spawnProjectile` → `createProjectile` (arrow at eye − 0.1, owner set,
   `setCritArrow(pow == 1.0F)`) → `shootProjectile` →
   `shootFromRotation(shooter, xRot, yRot, 0.0F, power, 1.0F)` →
   `getMovementToShoot`: normalize + 3× `this.random.triangle(0.0, 0.0172275 * 1.0F)`
   (**arrow's own random**) then scale by power; adds the shooter's known
   movement (`sourceMovement`, y zeroed when on ground).
4. `applyOnProjectileSpawned` — enchantment hooks (Flame ignites the arrow).
5. `weapon.hurtAndBreak(1, shooter, hand slot)`; sound plays on both sides.

**Crossbow**: `use` while uncharged starts charging (`startUsingItem`); `onUseTick`
(server) plays start/mid/end sounds at 20%/50%/100% of the charge duration and
loads the projectile at 100% (`tryLoadProjectiles` → `CHARGED_PROJECTILES`
component). `releaseUsing` returns true only when fully charged; `use` while
charged calls `performShooting` → `shoot(...)` with spread angles, then clears
the charged component. Fireworks fly at `1.6F` and cost 3 durability.

**Arrow flight** (`AbstractArrow.tick`, server entity tick):
1. If embedded in a block shape → stop, `inGround = true`.
2. If in ground: `tickDespawn` (`life >= 1200` → discard), else:
3. Water: velocity `*= 0.6F` + bubble particles. Crit: 4 crit particles.
4. Block clip along movement → `stepMoveAndHit`: gather entity hits
   (`findHitEntities`), sort by distance, move to nearest hit (entity or block),
   call `onHitEntity` / deflect. Piercing arrows continue through up to
   `pierceLevel` entities (`piercingIgnoreEntityIds`).
5. Air: velocity `*= 0.99F`; then gravity `-0.05` y (unless in ground / noPhysics).

**Arrow hit** (`AbstractArrow.onHitEntity`):
1. `arrowDamage = baseDamage (2.0)` modified by `EnchantmentHelper.modifyDamage`
   (Power) with the bow as weapon item; `damage = ceil(clamp(speed * arrowDamage, 0, 2.147483647E9))`.
2. Piercing bookkeeping; discard when `ignoreIds.size() >= pierceLevel + 1`.
3. Crit: `damage += this.random.nextInt(damage / 2 + 2)` (**arrow random**),
   capped at `2147483647L`.
4. `entity.hurtOrSimulate(arrow damage source, damage)` — the full pipeline
   (blocking → armor → resistance → protection → absorption → health), including
   `dealDefaultKnockback`: `0.4F` along the arrow's horizontal motion, and shield
   blocking if the defender faces the arrow.
5. Punch: `doKnockback` — `modifyKnockback` (Punch N → N) then
   `mob.push(dir.x * N * 0.6 * (1-kbRes), 0.1, dir.z * ...)`; no punch → no push.
6. Hit sound pitch `1.2F / (this.random.nextFloat() * 0.2F + 0.9F)` (**arrow
   random**); discard unless piercing.

## Tick placement

`releaseUsing`/`performShooting` run inside the server's use-item packet
handling (`ServerGamePacketListenerImpl`), spawning the arrow immediately via
`addFreshEntity`; the arrow's first `tick()` runs in the next server entity-tick
pass. All in-flight physics and `onHitEntity` are server entity-tick work. The
client predicts its own arrow separately — client/server arrows diverge by the
inaccuracy draws (client uses its own entity random).

## Randomness

| Draw | Code | RandomSource | Bit-for-bit? |
|---|---|---|---|
| Launch inaccuracy (3 comps) | `getMovementToShoot`: 3× `triangle(0.0, 0.0172275*uncertainty)` = 6 `nextDouble()` | `this.random` — the **arrow entity's** own | **No** — `Entity.random` is `RandomSource.create()` per instance, seed `^ System.nanoTime()` |
| Shooter-motion add | none (uses known movement) | — | Yes |
| Crit bonus | `this.random.nextInt(damage/2 + 2)` | arrow entity's | **No** (nanoTime seed) |
| Arrow hit-sound pitch | `this.random.nextFloat()` | arrow entity's | **No** |
| Bow release sound pitch | `level.getRandom().nextFloat()` | **server level** | Yes, if level random state is tracked |
| Crossbow extra-shot pitch (multishot idx≥1) | `livingEntity.getRandom().nextFloat()` | **shooter entity's** | **No** (player entity random also nanoTime-seeded) |
| Crossbow charge-end pitch | `level.getRandom().nextFloat()` | server level | Yes, if tracked |
| Multishot angles | none — fixed `0, -10, +10` from spread `10.0` | — | Yes |
| Punch/Power amounts | none — data-driven | — | Yes |

`triangle(mean, spread) = mean + spread * (nextDouble() - nextDouble())`
[RandomSource:59-61]. Consequence: a bit-exact simulator must treat every
projectile trajectory as non-reproducible from the live server; only
level-random draws are trackable.

## Proposed oracle scenarios

(Crit/damage values below are structural — exact rolls are not reproducible.)

1. `106_bow_full_draw`: 20-tick draw, release. Expect: arrow speed in
   `[3.0 - 0.09, 3.0 + 0.09]` (inaccuracy bound `0.0172275 * 3.0 * sqrt(3)` per
   component), `crit == true`, first-tick velocity direction within
   `±0.0172275` rad of aim.
2. `107_bow_partial_draw`: 10-tick draw → `pow = (0.25 + 1.0)/3 = 0.41667`,
   speed `≈ 1.25`, `crit == false`; 2-tick draw → `pow < 0.1` → no arrow spawned.
3. `108_crossbow_multishot`: charged multishot crossbow fired. Expect: 3 arrows,
   yaw offsets `{0, -10, +10}` degrees (before inaccuracy), crossbow durability
   −3, ammo consumed 1.
4. `109_arrow_damage_shape`: full-draw arrow vs unarmored player at measured
   impact speed `v`. Expect `damage == ceil(v * 2.0)` (non-crit enforced by
   checking `crit == false`); with Power V bow expect `ceil(v * 5.0)`.

## Open questions

- Flame `projectile_spawned` ignite `duration: 100.0` — unit (ticks vs seconds)
  not pinned; the on-hit `igniteForSeconds(5.0F)` is verified.
- `hurtOrSimulate` vs `hurt` for arrows: when the simulated path is taken is not
  mapped (client-side prediction only?).
- Tipped-arrow `doPostHurtEffects` (potion effects on hit) not mapped in detail.
- `checkLeftOwner` grace window (ticks before the arrow can hit its owner) not
  extracted — relevant for point-blank self-hits.
- Exact client/server packet flow for `needsSync = true` on `shoot` not traced.
