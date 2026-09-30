# Ender Pearl — net.minecraft.world.item.EnderpearlItem / net.minecraft.world.entity.projectile.throwableitemprojectile.ThrownEnderpearl

Source: decompiled vanilla 26.3 client jar (Vineflower 1.12.0). All literals quoted exactly. Extract log: `work/extract-20260929-220428.log` (EnderpearlItem, ThrownEnderpearl sections) plus targeted decompiles of `Projectile`, `ThrowableProjectile`, `ThrowableItemProjectile`, `UseCooldown`, `RandomSource`, `RandomSupport`, `Mth` (same pipeline).

## Summary

- Throw (`EnderpearlItem.use`): `Projectile.spawnProjectileFromRotation(ThrownEnderpearl::new, serverLevel, itemStack, player, 0.0F, 1.5F, 1.0F)` — power `1.5F`, inaccuracy `1.0F`; `PROJECTILE_SHOOT_POWER = 1.5F` declared but the `use()` body inlines `1.5F` [EnderpearlItem.use].
- **Inaccuracy draws come from the pearl's own `this.random` (the `Entity.random` of the freshly spawned pearl), NOT the player's** — `Projectile.getMovementToShoot` calls `this.random.triangle(0.0, 0.0172275 * uncertainty)` ×3 [Projectile.getMovementToShoot].
- Flight physics (`ThrowableProjectile.tick`): `applyGravity()` → `−0.03` per tick (`getDefaultGravity()` returns `0.03`, double); `applyInertia()` → `×0.99F` in air (`getAirDrag()`), `×0.8F` in water; then collision sweep, `setPos`, `updateRotation`, `applyEffectsFromBlocks`, `super.tick()`, then `hitTargetOrDeflectSelf` [ThrowableProjectile].
- Pearl spawn position: `(owner.getX(), owner.getEyeY() − 0.1F, owner.getZ())` [ThrowableItemProjectile].
- `onHitEntity`: victim hurt with `damageSources().thrown(this, getOwner())`, amount **`0.0F`** — no damage; then falls through to `onHit` teleport logic [ThrownEnderpearl.onHitEntity, Projectile.onHit → ThrownEnderpearl.onHit].
- `onHit` teleport (server, player owner, connection accepting messages): destination = `this.oldPosition()` = pearl's position at the **start of the tick it hit**; `player.teleport(new TeleportTransition(level, teleportPos, Vec3.ZERO, 0.0F, 0.0F, Relative.ROTATION, TeleportTransition.DO_NOTHING))` → motion zeroed, look set to (yRot 0.0, xRot 0.0). Then `resetFallDistance()`, `resetCurrentImpulseContext()`, then `newOwner.hurtServer(player.level(), this.damageSources().enderPearl(), 5.0F)` — **exact literal `5.0F`**, damage type `ender_pearl` (`data/minecraft/damage_type/ender_pearl.json`). No effects are cleared anywhere in this path (verified absent) [ThrownEnderpearl.onHit].
- Endermite: `if (this.random.nextFloat() < 0.05F && level.isSpawningMonsters() && difficulty != PEACEFUL)` → spawn at the **owner's pre-teleport** position [ThrownEnderpearl.onHit].
- Cooldown: **not in `EnderpearlItem.use`** — it's the `UseCooldown` data component: `Items.ENDER_PEARL` registered with `.useCooldown(1.0F)`; `UseCooldown.ticks() = (int)(seconds * 20.0F)` = **20 ticks**, applied by `UseCooldown.apply(stack, user)` → `player.getCooldowns().addCooldown(stack, 20)` inside `ItemStack.applyAfterUseComponentSideEffects` after a successful instant `use()` [Items, UseCooldown, ItemStack.use/applyAfterUseComponentSideEffects].
- Non-player owners: teleported to `oldPosition()` keeping their own yaw/pitch (`owner.getYRot(), owner.getXRot()`), `resetFallDistance()`, **no `5.0F` damage** [ThrownEnderpearl.onHit].
- **Critical randomness verdict:** the pearl's `Entity.random` is per-instance unique-seeded (`RandomSupport.generateUniqueSeed()`, nanoTime-based) — trajectory inaccuracy (6 `nextDouble`s), portal-particle positions, and the 5% endermite roll are all **not reproducible** from world state alone; the pearl seed/state must be captured.

## Data — constants

| Name | Literal | Location |
|---|---|---|
| `PROJECTILE_SHOOT_POWER` | `1.5F` (declared; `use()` inlines `1.5F`) | `EnderpearlItem` |
| shoot args | `yOffset 0.0F, pow 1.5F, uncertainty 1.0F` | `EnderpearlItem.use` → `Projectile.spawnProjectileFromRotation` |
| inaccuracy spread | `this.random.triangle(0.0, 0.0172275 * uncertainty)` ×3, then `scale(pow)` | `Projectile.getMovementToShoot` |
| spawn position | `(owner.getX(), owner.getEyeY() − 0.1F, owner.getZ())` | `ThrowableItemProjectile.<init>` |
| gravity | `getDefaultGravity()` returns `0.03` (double); applied as `−gravity` | `ThrowableProjectile` / `Entity.applyGravity` |
| air drag | `getAirDrag()` returns `0.99F` | `ThrowableProjectile` |
| water drag | `inertia = 0.8F` (+ 4 bubble particles per tick) | `ThrowableProjectile.applyInertia` |
| onHitEntity damage | `0.0F` via `damageSources().thrown(this, this.getOwner())` | `ThrownEnderpearl.onHitEntity` |
| teleport damage | `5.0F` via `damageSources().enderPearl()` | `ThrownEnderpearl.onHit` (`hurtServer(player.level(), …, 5.0F)`) |
| teleport motion/look (player) | `Vec3.ZERO`, `0.0F, 0.0F`, `Relative.ROTATION` | `ThrownEnderpearl.onHit` |
| teleport destination | `this.oldPosition()` = `(xOld, yOld, zOld)`, start-of-tick position | `ThrownEnderpearl.onHit`, `Entity.oldPosition` |
| endermite chance | `this.random.nextFloat() < 0.05F` (+ `isSpawningMonsters()` + difficulty ≠ `PEACEFUL`) | `ThrownEnderpearl.onHit` |
| item cooldown | `useCooldown(1.0F)` → `(int)(1.0F * 20.0F)` = **20 ticks** | `Items.ENDER_PEARL`, `UseCooldown.ticks` |
| portal particles on hit | 32 × `addParticle(PORTAL, x, y + random.nextDouble() * 2.0, z, random.nextGaussian(), 0.0, random.nextGaussian())` | `ThrownEnderpearl.onHit` |
| stack size | 16 (`.stacksTo(16)`) | `Items.ENDER_PEARL` |
| consume | `itemStack.consume(1, player)` | `EnderpearlItem.use` |
| throw sound pitch | `0.4F / (level.getRandom().nextFloat() * 0.4F + 0.8F)` | `EnderpearlItem.use` |
| vanish-on-death gamerule | `GameRules.ENDER_PEARLS_VANISH_ON_DEATH` checked in `tick()` for dead owners | `ThrownEnderpearl.tick` |
| chunk ticket | `registerAndUpdateEnderPearlTicket`, `ticketTimer` refresh on chunk change | `ThrownEnderpearl.tick` |
| portal-cooldown transfer | `if (this.isOnPortalCooldown()) owner.setPortalCooldown();` | `ThrownEnderpearl.onHit` |

## Logic (ordered)

### Throw — `EnderpearlItem.use`
1. Both sides: `ENDER_PEARL_THROW` sound (vol `0.5F`), `Stats.ITEM_USED`, `itemStack.consume(1, player)` (creative-aware).
2. Server only: `spawnProjectileFromRotation(ThrownEnderpearl::new, serverLevel, itemStack, player, 0.0F, 1.5F, 1.0F)`:
   - factory → `new ThrownEnderpearl(serverLevel, source, itemStack)` → spawn pos `(x, eyeY − 0.1F, z)`, owner set, pearl registered to `ServerPlayer` (`registerEnderPearl`).
   - `shootFromRotation(source, xRot, yRot, 0.0F, 1.5F, 1.0F)`:
     - `xd = −sin(yRot·π/180)·cos(xRot·π/180)`, `yd = −sin((xRot+0.0)·π/180)`, `zd = cos(yRot·π/180)·cos(xRot·π/180)`;
     - `shoot(...)`: `getMovementToShoot` — normalize + 3× `this.random.triangle(0.0, 0.0172275 * 1.0F)` (**pearl's own random, 6 `nextDouble` draws**) → `scale(1.5F)`; set rotation from vector;
     - add shooter's known movement `(sourceMovement.x, onGround ? 0.0 : sourceMovement.y, sourceMovement.z)`.
   - `serverLevel.addFreshEntity`, `applyOnProjectileSpawned` (enchantment hook, presumed no-op for pearls).
3. `ItemStack.use` wrapper: `UseCooldown` component present → `UseCooldown.apply(stackBeforeUse, user)` → `player.getCooldowns().addCooldown(stack, 20)` [ItemStack.applyAfterUseComponentSideEffects, UseCooldown].

RNG state at this point on the pearl's `this.random` (fresh `RandomSource.create()`): 2× `nextLong()` (UUID) then 6× `nextDouble()` (inaccuracy).

### Flight — `ThrownEnderpearl.tick` (server)
1. Dead-owner check: if owner is a dead `ServerPlayer` (not `wonGame`) and `ENDER_PEARLS_VANISH_ON_DEATH` → discard.
2. `super.tick()` = `ThrowableProjectile.tick()`:
   - `handleFirstTickBubbleColumn()` (bubble-column scan on first tick);
   - `applyGravity()`: `delta.y −= 0.03` (unless `isNoGravity()`);
   - `applyInertia()`: `delta *= 0.99F` (air) or `0.8F` (water, +4 bubble particles);
   - `ProjectileUtil.getHitResultOnMoveVector(this, this::canHitEntity)`;
   - `setPos(hit ? result.getLocation() : position + delta)`; `updateRotation()`; `applyEffectsFromBlocks()`; `super.tick()` (Entity base tick);
   - `hitTargetOrDeflectSelf(result)` if hit.
3. If alive: `--ticketTimer <= 0` or chunk-section change → `serverPlayer.registerAndUpdateEnderPearlTicket(this)` (keeps pearl's chunk loaded).

### Hit — `ThrownEnderpearl.onHit`
1. 32 portal particles (positions from pearl's own `random`: 32× `nextDouble` + 64× `nextGaussian`).
2. `super.onHit(hitResult)` → `Projectile.onHit` dispatch: ENTITY → `this.onHitEntity` (pearl override: zero-damage `hurt` with `thrown` source, then base no-op) + `PROJECTILE_LAND` game event; BLOCK → `onHitBlock` + game event.
3. Server, not removed: owner resolution (`findOwnerIncludingDeadPlayer`, handles cross-dimension via `getEntityInAnyDimension`):
   - `isAllowedToTeleportOwner`: same dimension → alive (and not sleeping if living); different dimension → `owner.canUsePortal(true)`.
   - **Player owner** (`connection.isAcceptingMessages()` required): endermite roll (5%, at owner pre-teleport pos) → portal-cooldown transfer → `player.teleport(TeleportTransition(level, teleportPos=oldPosition(), Vec3.ZERO, 0.0F, 0.0F, Relative.ROTATION, DO_NOTHING))` → on success: `resetFallDistance()`, `resetCurrentImpulseContext()`, `hurtServer(player.level(), enderPearl(), 5.0F)` → teleport sound at destination → `discard()`.
   - **Non-player owner**: `teleport(TeleportTransition(level, teleportPos, Vec3.ZERO, owner.getYRot(), owner.getXRot(), DO_NOTHING))`, `resetFallDistance()`, `resetCurrentImpulseContext()` if living — **no damage** → sound → `discard()`.
   - **No effects are cleared** in either path (verified absent from `onHit`).

## Tick placement (server thread ordering)
- `use()` runs in the server use-item packet handler; the pearl is constructed and `addFreshEntity`'d synchronously, first `tick()` on the next server tick. Cooldown entry (20 ticks) is written during the same packet handling via `ItemStack.applyAfterUseComponentSideEffects`.
- `onHit` → teleport → `5.0F` damage all happen inside the pearl's entity tick on the server thread, in `ThrowableProjectile.tick`'s trailing `hitTargetOrDeflectSelf` call.
- Exact packet-handler class (`ServerPlayerGameMode`) not decompiled in this batch — cooldown *gate* location UNVERIFIED (see open questions).

## Randomness — every draw pinned to its source

Legend: `pearl.random` = `ThrownEnderpearl.this.random` = `Entity.random` (per-instance `RandomSource.create()`, nanoTime-unique seed — **not reproducible** without capturing seed/state).

| Draw | Exact source expression | Reproducible bit-for-bit? |
|---|---|---|
| Pearl UUID: 2× `nextLong()` (entity field init) | `pearl.random` | No — unique seed |
| Inaccuracy: 3× `triangle(0.0, 0.0172275 * 1.0F)` = 6× `nextDouble()` | `pearl.random` (`Projectile.getMovementToShoot` ← `shootFromRotation`) | **No** — this is the key one: the *pearl's* random, not the player's, not the level's. Trajectory cannot be reproduced from world state alone |
| Portal particles on hit: 32× `nextDouble()`, 64× `nextGaussian()` | `pearl.random` (`ThrownEnderpearl.onHit`); runs on both server and client with their own seeds | Cosmetic; No |
| Endermite roll: 1× `nextFloat() < 0.05F` | `pearl.random` (server, `ThrownEnderpearl.onHit`) | No — gameplay-relevant but unknowable without captured state |
| Throw sound pitch: `level.getRandom().nextFloat()` | `level.getRandom()` (server/client levels separately) | Cosmetic |
| Water bubble particles (4/tick) | no RNG (fixed offsets) | Yes |
| `RandomSource.create()` seeding | `RandomSupport.generateUniqueSeed()` = `SEED_UNIQUIFIER.updateAndGet(c -> c * 1181783497276652981L) ^ System.nanoTime()` → `new LegacyRandomSource(seed)` | Verifies the non-reproducibility claim [RandomSource.create, RandomSupport] |

Note: unlike `FishingHook`, `ThrownEnderpearl` has no per-tick re-seeded `syncronizedRandom` — every one of its draws comes from the unique-seeded instance random.

## Proposed oracle scenarios (120+)

- **120_throw_velocity**: Player with fixed pitch/yaw throws pearl; oracle supplies the 6 `nextDouble` values (i.e. captures pearl RNG state at spawn); assert initial delta = `normalize(dir + triangle...) * 1.5F + shooterMovement`, spawn pos = `(x, eyeY − 0.1F, z)`. Documents that without captured RNG state this scenario is **not** bit-for-bit testable.
- **121_inaccuracy_source**: Two throws with identical player state but different pearl instances produce different trajectories — proves inaccuracy draws come from the pearl's own `this.random` (per-instance unique seed), not player or level random.
- **122_flight_physics**: Oracle drives 40 ticks with no collisions; assert per-tick `y −= 0.03` and `v *= 0.99F` (air) ordering: gravity first, then drag, then position — exact sequence from `ThrowableProjectile.tick`.
- **123_teleport_destination**: Pearl hits a wall; assert player lands at the pearl's start-of-tick position (`oldPosition()`), with motion exactly `(0,0,0)` and look `(yRot 0.0, xRot 0.0)`.
- **124_pearl_damage**: After teleport, player health reduced by exactly `5.0F` through the `ender_pearl` damage source; assert fall distance was reset first (no compounding) and effects are **not** cleared (e.g. a speed effect persists).
- **125_cooldown**: After throw, `player.getCooldowns()` holds an entry of exactly 20 ticks for the ender pearl item (from `UseCooldown(1.0F)` → `(int)(1.0F * 20.0F)`); assert a second `use()` during the window is rejected by the cooldown gate.
- **126_entity_hit_no_damage**: Pearl hits another player; victim takes `0.0F` from the `thrown` source (no health change) and the thrower still teleports.
- **127_mob_thrower_no_damage**: Pearl thrown by a non-player owner; assert teleport keeps owner's yaw/pitch and applies **no** `5.0F` damage.

## Open questions (honest gaps)
- Cooldown *gate*: where the server rejects `use()` while `isOnCooldown` (presumably `ServerPlayerGameMode`) — not decompiled in this batch. UNVERIFIED.
- `player.connection.isAcceptingMessages()` false case: pearl discards without teleport — edge case during disconnect; behavior verified in code, testability in an oracle UNVERIFIED.
- `EnchantmentHelper.onProjectileSpawned` in `applyOnProjectileSpawned` — presumed no-op for ender pearls; UNVERIFIED.
- Whether `entityInside`/portal blocks interact with the flying pearl (end gateway: `onInsideBlock` forwards to owner) — edge case, UNVERIFIED for dueling relevance.
- Client-side prediction of the pearl trajectory vs. server authority — UNVERIFIED.
