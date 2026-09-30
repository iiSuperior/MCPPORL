# Fishing Rod — net.minecraft.world.item.FishingRodItem / net.minecraft.world.entity.projectile.FishingHook

Source: decompiled vanilla 26.3 client jar (Vineflower 1.12.0). All literals quoted exactly. Extract log: `work/extract-20260929-220428.log` (FishingRodItem, FishingHook sections) plus targeted decompiles of `Projectile`, `RandomSource`, `RandomSupport`, `Mth` (same pipeline).

## Summary

- Casting (`FishingRodItem.use`, no hook active): server-only hook spawn via `Projectile.spawnProjectile(new FishingHook(player, level, luck, lureSpeed), serverLevel, itemStack)`. The hook's **initial velocity is computed inline in the `FishingHook(Player, Level, int, int)` constructor** — `shootFromRotation`/`getMovementToShoot` are NOT used [FishingHook(FishingHook.<init>)].
- Reeling (`use`, `player.fishing != null`): server-only `player.fishing.retrieve(itemStack)` returns durability damage, then `itemStack.hurtAndBreak(dmg, player, hand.asEquipmentSlot())` [FishingRodItem.use].
- Hooking an entity (`FishingHook.onHitEntity`): **no damage, no knockback at hook time** — it only calls `setHookedEntity` (server-side). The pull happens only on `retrieve()` via `pullEntity` [FishingHook.onHitEntity, FishingHook.retrieve, FishingHook.pullEntity].
- `retrieve()` durability: hooked entity = `5` (`3` if the hooked entity is an `ItemEntity`); successful catch (`nibble > 0`) = `1`; **then `if (this.onGround()) dmg = 2;` overrides any of the above** [FishingHook.retrieve].
- Fishing rod item: `.durability(64)`, no `useCooldown` component, `.enchantable(1)` [Items.FISHING_ROD].
- **Critical randomness verdict:** everything gameplay-relevant (`FishingHook.this.random`, i.e. `Entity.random`) is a per-instance `RandomSource.create()` seeded by `RandomSupport.generateUniqueSeed()` = `SEED_UNIQUIFIER.updateAndGet(c -> c * 1181783497276652981L) ^ System.nanoTime()` — unknown to an oracle unless the live seed/state is captured. The one exception is `syncronizedRandom`, re-seeded every tick to `getUUID().getLeastSignificantBits() ^ level.getGameTime()`, which is reproducible from (hook UUID, game tick) [RandomSource.create, RandomSupport.generateUniqueSeed, FishingHook.tick].

## Data — constants

| Name | Literal | Location |
|---|---|---|
| `GRAVITY` | `0.03F` | `FishingHook` (`private static final float GRAVITY = 0.03F`); also returned by `getDefaultGravity()` as `0.03F` |
| `MAX_OUT_OF_WATER_TIME` | `10` | `FishingHook` |
| ground-life discard | `1200` ticks (`if (this.life >= 1200)`) | `FishingHook.tick` |
| stop-fishing distance² | `1024.0` (32 blocks) | `FishingHook.shouldStopFishing` |
| throw base speed | `0.6 / dist` per axis | `FishingHook.<init>` |
| throw inaccuracy | `this.random.triangle(0.5, 0.0103365)` per axis (3 calls) | `FishingHook.<init>` |
| throw spawn offset | player pos − look·`0.3`, y = `player.getEyeY()` | `FishingHook.<init>` |
| water entry damping | `multiply(0.3, 0.2, 0.3)` | `FishingHook.tick` (FLYING→BOBBING) |
| bobbing drag | `movement.x * 0.9`, `movement.z * 0.9`; y: `movement.y - force * this.random.nextFloat() * 0.2` | `FishingHook.tick` (BOBBING) |
| bobbing force | `getY() + movement.y - blockPos.getY() - liquidHeight`; if `\|force\| < 0.01`, `force += signum(force) * 0.1` | `FishingHook.tick` |
| per-tick inertia | `scale(0.92)` (`double inertia = 0.92`, literal inlined) | `FishingHook.tick` |
| gravity application | `add(0.0, -this.getDefaultGravity(), 0.0)` only when `!fluidState.is(FluidTags.WATER) && !onGround() && hookedIn == null` | `FishingHook.tick` |
| biting dip (server bobbing) | `add(0.0, -0.1 * syncronizedRandom.nextFloat() * syncronizedRandom.nextFloat(), 0.0)` | `FishingHook.tick` |
| biting dip (client, on DATA_BITING) | y = `-0.4F * Mth.nextFloat(this.syncronizedRandom, 0.6F, 1.0F)` | `FishingHook.onSyncedDataUpdated` |
| pullEntity (reel) | `delta = (owner − hook) * 0.1`; `entity.setDeltaMovement(entity.getDeltaMovement().add(delta))`, only if `owner != null && entity.canSimulateMovement()` | `FishingHook.pullEntity` |
| catch launch velocity | `entity.setDeltaMovement(xa * 0.1, ya * 0.1 + sqrt(sqrt(xa²+ya²+za²)) * 0.08, za * 0.1)` with `double speed = 0.1;` (declared but unused) | `FishingHook.retrieve` |
| catch XP | `this.random.nextInt(6) + 1` | `FishingHook.retrieve` |
| retrieve dmg: entity hooked | `5`, or `3` if `hookedIn instanceof ItemEntity` | `FishingHook.retrieve` |
| retrieve dmg: fish caught | `1` | `FishingHook.retrieve` |
| retrieve dmg: on ground | `2` (overrides) | `FishingHook.retrieve` |
| lure timing | `timeUntilLured = Mth.nextInt(random, 100, 600)` then `- lureSpeed`; `lureSpeed = (int)(getFishingTimeReduction * 20.0F)` (Lure = `5.0` s/level in data → 100 ticks/level) | `FishingHook.catchingFish`, `FishingRodItem.use`, `data/minecraft/enchantment/lure.json` |
| `timeUntilHooked` | `Mth.nextInt(this.random, 20, 80)` | `FishingHook.catchingFish` |
| `nibble` | `Mth.nextInt(this.random, 20, 40)` | `FishingHook.catchingFish` |
| tease chance | base `0.15F`; `+ (20 − t) * 0.05F` if t<20; `+ (40 − t) * 0.02F` if t<40; `+ (60 − t) * 0.01F` if t<60 | `FishingHook.catchingFish` |
| fish approach angle jitter | `random.triangle(0.0, 9.188)` per tick | `FishingHook.catchingFish` |
| fish approach orbit radius | `timeUntilHooked * 0.1F` | `FishingHook.catchingFish` |
| rain speed-up | `if (random.nextFloat() < 0.25F && level.isRainingAt(above)) fishingSpeed++` | `FishingHook.catchingFish` |
| no-sky slowdown | `if (random.nextFloat() < 0.5F && !level.canSeeSky(above)) fishingSpeed--` | `FishingHook.catchingFish` |
| bubble particle chance | `random.nextFloat() < 0.15F` | `FishingHook.catchingFish` |
| tease splash count | `2 + random.nextInt(2)` particles | `FishingHook.catchingFish` |
| bite splash pitch | `1.0F + (random.nextFloat() − random.nextFloat()) * 0.4F` | `FishingHook.catchingFish` |
| throw/retrieve sound pitch | `0.4F / (level.getRandom().nextFloat() * 0.4F + 0.8F)` | `FishingRodItem.use` |
| hooked-entity entity event | `(byte)31` broadcast; client `handleEntityEvent` → `pullEntity` | `FishingHook.retrieve`, `FishingHook.handleEntityEvent` |
| open water area | 5×5×4 region (`blockPos.offset(±2, y, ±2)`, y in −1..2) all-`INSIDE_WATER`/all-air-with-lily-pad logic | `FishingHook.calculateOpenWater` |

## Logic (ordered)

### Cast — `FishingRodItem.use` (hook == null)
1. Both sides: `FISHING_BOBBER_THROW` sound (vol `0.5F`), `GameEvent.ITEM_INTERACT_START` vibration, `Stats.ITEM_USED`.
2. Server only: `lureSpeed = (int)(EnchantmentHelper.getFishingTimeReduction(serverLevel, itemStack, player) * 20.0F)`; `luck = EnchantmentHelper.getFishingLuckBonus(serverLevel, itemStack, player)` (Lure +5.0 s/level, Luck of the Sea +1/level per enchantment JSON). **No rod durability consumed on cast.**
3. `Projectile.spawnProjectile(new FishingHook(player, level, luck, lureSpeed), serverLevel, itemStack)` → constructor computes spawn pos/velocity → `serverLevel.addFreshEntity(projectile)` → `applyOnProjectileSpawned` [FishingRodItem.use, Projectile.spawnProjectile].

### Hook construction — `FishingHook(Player, Level, int, int)`
Order: `Entity` field init first — `random = RandomSource.create()` (unique seed), then `uuid = Mth.createInsecureUUID(this.random)` = 2× `random.nextLong()` **before** any constructor-body draws [Entity, Mth.createInsecureUUID].
1. `setOwner(player)` → `updateOwnerInfo` → `owner.fishing = hook`.
2. Spawn at `x1 = player.getX() − ySin * 0.3`, `y1 = player.getEyeY()`, `z1 = player.getZ() − yCos * 0.3`.
3. `newMovement = (−ySin, clamp(−(xSin/xCos), −5.0F, 5.0F), −yCos)` normalized, then multiplied per-axis by `0.6 / dist + this.random.triangle(0.5, 0.0103365)` — **3 triangle calls = 6 `nextDouble()` draws on the hook's own `this.random`** (`RandomSource.triangle(double,double)` = `mean + spread * (nextDouble() − nextDouble())`).
4. `setYRot/setXRot` from the movement vector.

### Hook tick — `FishingHook.tick` (server; client runs the same minus `catchingFish`)
1. `this.syncronizedRandom.setSeed(this.getUUID().getLeastSignificantBits() ^ this.level().getGameTime());` — deterministic reseed every tick, both sides.
2. `super.tick()` (Entity base tick).
3. Owner null → discard. Server: `shouldStopFishing(owner)` → discards if the player no longer holds a rod in either hand or is >32 blocks away.
4. `onGround()` → `life++`, discard at `life >= 1200`; else `life = 0`.
5. `liquidHeight` from fluid state (water only); `isInWater = liquidHeight > 0.0F`.
6. `FLYING`: if `hookedIn != null` → zero velocity, state `HOOKED_IN_ENTITY`; else if in water → `multiply(0.3, 0.2, 0.3)`, state `BOBBING`; else `checkCollision()` → `hitTargetOrDeflectSelf` → on entity: `super.onHitEntity` + server `setHookedEntity`; on block: velocity = `normalize().scale(hitResult.distanceTo(this))`.
7. `HOOKED_IN_ENTITY`: hook follows the entity (`setPos(hookedIn.getX(), hookedIn.getY(0.8), hookedIn.getZ())`); releases if entity removed/unloadable/different dimension.
8. `BOBBING`: bobbing physics (see constants); `openWater` recomputed only while `nibble > 0 || timeUntilHooked > 0`; in water: `outOfWaterTime` decay, biting dip via `syncronizedRandom` (deterministic), server-only `catchingFish(blockPos)`; not in water: `outOfWaterTime` grows to 10.
9. Gravity: only if not in water fluid, not on ground, `hookedIn == null`.
10. `move(SELF, delta)`, `applyEffectsFromBlocks`, `updateRotation`, zero velocity on `onGround() || horizontalCollision` while FLYING, then `scale(0.92)`, `reapplyPosition()`.

### Bite sequence — `FishingHook.catchingFish` (server only)
1. `fishingSpeed = 1` ± rain/sky modifiers (2 `nextFloat` draws/tick).
2. `nibble > 0` → decrement; at 0: reset lured/hooked timers, `DATA_BITING=false`.
3. `timeUntilHooked > 0` → decrement by `fishingSpeed`; while >0: approach particles/jitter; at ≤0: splash sound+particles, `nibble = nextInt(20,40)`, `DATA_BITING=true` (client dip via `onSyncedDataUpdated`).
4. `timeUntilLured > 0` → decrement; tease rolls; at ≤0: `fishAngle = nextFloat(0,360)`, `timeUntilHooked = nextInt(20,80)`.
5. Else: `timeUntilLured = nextInt(100,600) − lureSpeed`.

### Reel — `FishingHook.retrieve(rod)` (server only; returns durability dmg)
1. If `hookedIn != null`: `pullEntity(hookedIn)` (adds `(owner−hook)*0.1` to hooked entity velocity), criteria trigger, `broadcastEntityEvent(this, (byte)31)` (client re-applies pull), dmg = `3` (ItemEntity) else `5`.
2. Else if `nibble > 0`: loot roll (`BuiltInLootTables.FISHING`, luck = `this.luck + owner.getLuck()`), spawn `ItemEntity`s with the catch velocity, XP orb (`nextInt(6)+1`), `FISH_CAUGHT` stat; dmg = `1`.
3. `if (this.onGround()) dmg = 2;` (overrides). `discard()`. Caller: `itemStack.hurtAndBreak(dmg, player, hand.asEquipmentSlot())`.

## Tick placement (server thread ordering)
- `use()` runs in the server packet handler (use-item packet, before entity ticks of that server tick). The hook is constructed + `addFreshEntity`'d immediately; its first `tick()` runs on the **next** server tick as part of `ServerLevel` entity ticking (UNVERIFIED: exact handler class name not decompiled in this batch, but the packet-then-tick order follows standard server flow).
- `retrieve()` runs synchronously in the second `use()` call (reel packet).
- `syncronizedRandom` reseed is the first statement of `tick()`, before `super.tick()`.

## Randomness — every draw pinned to its source

Legend: `hook.random` = `FishingHook.this.random` = `Entity.random` (per-instance `RandomSource.create()` / unique seed — **not reproducible** without capturing seed/state).

| Draw | Exact source expression | Reproducible bit-for-bit? |
|---|---|---|
| Hook UUID: 2× `nextLong()` (entity field init) | `hook.random` | No — unique seed |
| Throw velocity: 3× `triangle(0.5, 0.0103365)` = 6× `nextDouble()` | `hook.random` (`FishingHook.<init>`) | No — unique seed |
| Bobbing y: 1× `nextFloat()` per tick (BOBBING) | `hook.random` (`FishingHook.tick`) | No |
| Biting dip: 2× `nextFloat()` per tick while biting | `hook.syncronizedRandom`, seeded `uuid.least ^ gameTime` | **Yes** — given hook UUID + server game tick |
| Client DATA_BITING dip: `Mth.nextFloat(syncronizedRandom, 0.6F, 1.0F)` | `hook.syncronizedRandom` (reseeded that client tick) | **Yes** — given UUID + client game time; cosmetic |
| Bite timing: `nextInt(100,600)`, `nextInt(20,80)`, `nextInt(20,40)`, approach `triangle(0.0, 9.188)` (2× nextDouble/tick), `nextFloat()<0.25F/0.5F/0.15F`, tease `nextFloat(0,360)`, `nextFloat(25,60)`, `nextInt(2)`, splash pitch 2× nextFloat | `hook.random` (`FishingHook.catchingFish`, server) | No — unique seed; bite/nibble timings **cannot** be reproduced from world seed alone |
| Catch XP: `nextInt(6)+1` | `hook.random` (`FishingHook.retrieve`) | No |
| Throw/retrieve sound pitch: `level.getRandom().nextFloat()` | `level.getRandom()` (server level random / client level random separately) | Cosmetic; server-side level random is world-seeded |
| Loot table roll | Loot system RNG (UNVERIFIED exact source in 26.3) | UNVERIFIED |
| ItemEntity despawn/pickup etc. after catch | entity/level randoms — out of scope | — |

## Proposed oracle scenarios (110+)

- **110_cast_velocity**: Player with fixed pitch/yaw casts rod; hook's initial delta-movement = f(rot) with the 3 `triangle(0.5, 0.0103365)` draws removed — oracle supplies the triangle values; assert velocity matches `0.6/dist + triangle` per axis and spawn pos = player − look·0.3 at eye height.
- **111_reel_pull_entity**: Hook embedded in a target entity at known positions; reel; assert target's new velocity = old + `(owner − hook) * 0.1` exactly, and rod takes 5 damage (or 3 if target is an ItemEntity).
- **112_hook_no_damage**: Hook collides with a player mid-flight; assert victim takes no damage and no knockback (only `setHookedEntity` state change).
- **113_retrieve_durability_matrix**: Reel in each state — (a) hooked entity: 5; (b) hooked ItemEntity: 3; (c) bite caught: 1; (d) bobber on ground during (a)/(b)/(c): 2 (override); assert `hurtAndBreak` receives exactly that int.
- **114_bobbing_dip_deterministic**: Given hook UUID + game tick, compute `syncronizedRandom` seed = `leastBits ^ gameTime` and assert the biting dip delta-y equals `-0.1 * nextFloat() * nextFloat()` — fully reproducible without any captured state.
- **115_ground_timeout**: Hook resting on ground; advance 1199 ticks (alive) then 1200th tick → discarded.
- **116_bite_timing_not_reproducible**: Two identical casts (same world seed, same tick) produce different `timeUntilLured` values — documents that hook `this.random` is per-instance unique-seeded, so an oracle must treat bite timing as unknowable unless the entity's RNG state is captured.

## Open questions (honest gaps)
- Exact server packet-handler call site for `ItemStack.use` (cooldown/advancement gating) — `ServerPlayerGameMode` not decompiled in this batch. UNVERIFIED.
- Loot-table RNG source in 26.3 (`FishingHook.retrieve` → `lootTable.getRandomItems`) — UNVERIFIED.
- Whether `applyOnProjectileSpawned`'s `EnchantmentHelper.onProjectileSpawned` can alter hook behavior with exotic enchantments — presumed no-op for rods, UNVERIFIED.
- Client-side prediction of the hook trajectory vs. server authority on high latency — UNVERIFIED.
