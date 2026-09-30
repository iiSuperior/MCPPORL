# Water / Lava Buckets — 26.3 mechanics

Source of truth: decompiled vanilla 26.3 client jar (Vineflower). Citations are
`[Class.method]`. Everything below was read from decompiled bodies unless marked
UNVERIFIED.

## Summary

- `BucketItem.use`: raytrace via `getPlayerPOVHitResult(level, player, getFluidContext())`
  where the fluid clip context is `SOURCE_ONLY` for an empty bucket and `NONE`
  for a filled one. `MISS` or non-`BLOCK` → `PASS`. Placement requires
  `level.mayInteract(player, pos) && player.mayUseItemAt(directionOffsetPos, direction, itemStack)`,
  else `FAIL`. [BucketItem.use]
- Placement (`emptyContents`): target block must `canBeReplaced(content)` or be a
  `LiquidBlockContainer` accepting the fluid; `blockState.isAir() || placeLiquid && (!shiftKeyDown || hitResult == null)`;
  otherwise it recurses to `hitResult.getBlockPos().relative(hitResult.getDirection())` with a null
  hitResult. Nether-type dims evaporate water (`WATER_EVAPORATES` attribute) with
  a cosmetic sound. Replaceable non-liquid blocks are destroyed with drops.
  Actual set: `level.setBlock(pos, content.defaultFluidState().createLegacyBlock(), 11)`.
  [BucketItem.emptyContents]
- Pickup (empty bucket): target block must be `BucketPickup`; `LiquidBlock.pickupBlock`
  only succeeds when `LEVEL == 0` (source); sets air, returns the fluid's bucket.
  [BucketItem.use, LiquidBlock.pickupBlock]
- Flow tick delays: water `5`; lava `10` in fast-lava dims (nether,
  `EnvironmentAttributes.FAST_LAVA` dimension value) else `30`.
  [WaterFluid.getTickDelay, LavaFluid.getTickDelay, LavaFluid.isFastLava]
- Lava standing damage/ignite moved to the block-effect system in 26.3:
  `LavaFluid.entityInside` → `CLEAR_FREEZE`, `LAVA_IGNITE` (`Entity::lavaIgnite`),
  `runAfter(LAVA_IGNITE, Entity::lavaHurt)`. `lavaHurt`: `4.0F` lava damage
  (server only); `lavaIgnite`: `igniteForSeconds(15.0F)`. Water:
  `WaterFluid.entityInside` → `EXTINGUISH` (`Entity::clearFire`).
  [LavaFluid.entityInside, Entity.lavaHurt, Entity.lavaIgnite, WaterFluid.entityInside]
- Fire ticks: `remainingFireTicks % 20 == 0 && !isInLava()` → `1.0F` onFire damage
  per second. Fire Resistance negates all `IS_FIRE` damage in
  `LivingEntity.hurtServer`. [Entity.baseTick, LivingEntity.hurtServer]
- Swim drag per tick (`LivingEntity.travelFlying`): water `0.8F`, lava `0.5`,
  air `0.91F` applied to `deltaMovement` after `move`. [LivingEntity.travelFlying]

## Data

### Bucket use / placement

| Name | Value | Location |
|---|---|---|
| raytrace | `getPlayerPOVHitResult(level, player, this.getFluidContext())` | `BucketItem.use` |
| fluid context, empty bucket | `ClipContext.Fluid.SOURCE_ONLY` | `BucketItem.getFluidContext` |
| fluid context, filled bucket | `ClipContext.Fluid.NONE` | `BucketItem.getFluidContext` |
| miss handling | `MISS` → `PASS`; non-`BLOCK` → `PASS` | `BucketItem.use` |
| permission gate | `level.mayInteract(player, pos) && player.mayUseItemAt(directionOffsetPos, direction, itemStack)` else `FAIL` | `BucketItem.use` |
| waterloggable target | `clicked.getBlock() instanceof LiquidBlockContainer && content == Fluids.WATER ? pos : directionOffsetPos` | `BucketItem.use` |
| place gate | `mayReplace = blockState.canBeReplaced(this.content)`; `placeLiquid = mayReplace \|\| (block instanceof LiquidBlockContainer && container.canPlaceLiquid(...))`; `canPlaceFluidInsideBlock = blockState.isAir() \|\| placeLiquid && (!shiftKeyDown \|\| hitResult == null)` | `BucketItem.emptyContents` |
| fallback | `emptyContents(user, level, hitResult.getBlockPos().relative(hitResult.getDirection()), null)` when not placeable | `BucketItem.emptyContents` |
| nether evaporation | `level.environmentAttributes().getValue(EnvironmentAttributes.WATER_EVAPORATES, pos) && content.is(FluidTags.WATER)` → extinguish sound + smoke, returns `true` (no block) | `BucketItem.emptyContents` |
| evaporation sound pitch | `2.6F + (random.nextFloat() - random.nextFloat()) * 0.8F`, `random = level.getRandom()` (cosmetic) | `BucketItem.emptyContents` |
| destroy replaceable | `!level.isClientSide() && mayReplace && !blockState.liquid()` → `level.destroyBlock(pos, true)` | `BucketItem.emptyContents` |
| set block | `level.setBlock(pos, this.content.defaultFluidState().createLegacyBlock(), 11)`; fail if false and target not source | `BucketItem.emptyContents` |
| success item | `getEmptySuccessItem`: `hasInfiniteMaterials() ? itemStack : new ItemStack(Items.BUCKET)` | `BucketItem.getEmptySuccessItem` |

### Pickup

| Name | Value | Location |
|---|---|---|
| pickup gate | `blockState.getBlock() instanceof BucketPickup` | `BucketItem.use` |
| source-only | `state.getValue(LEVEL) == 0` else `ItemStack.EMPTY` | `LiquidBlock.pickupBlock` |
| removal | `level.setBlock(pos, Blocks.AIR.defaultBlockState(), 11)`; returns `new ItemStack(this.fluid.getBucket())` | `LiquidBlock.pickupBlock` |

### Liquid scheduling / flow constants

| Name | Value | Location |
|---|---|---|
| water `getTickDelay` | `5` | `WaterFluid.getTickDelay` |
| lava `getTickDelay` | `isFastLava(level) ? 10 : 30` | `LavaFluid.getTickDelay` |
| `isFastLava` | `level.environmentAttributes().getDimensionValue(EnvironmentAttributes.FAST_LAVA)` | `LavaFluid.isFastLava` |
| schedule sites | `LiquidBlock.onPlace`, `neighborChanged`, `updateShape` (when `shouldSpreadLiquid`) → `scheduleTick(pos, fluidType, fluid.getTickDelay(level))` | `LiquidBlock` |
| lava spread delay | `getSpreadDelay`: `result = getTickDelay`; if height rising and `level.getRandom().nextInt(4) != 0` → `result *= 4` | `LavaFluid.getSpreadDelay` |
| water `getDropOff` / `getSlopeFindDistance` | `1` / `4` | `WaterFluid` |
| lava `getDropOff` / `getSlopeFindDistance` | `isFastLava ? 1 : 2` / `isFastLava ? 4 : 2` | `LavaFluid` |
| lava obsidian/cobble | neighbor water + source → `OBSIDIAN` else `COBBLESTONE`, `fizz` (levelEvent 1501), no spread | `LiquidBlock.shouldSpreadLiquid` |
| lava randomTick (fire spread) | `random.nextInt(3)` draws; fire placed above flammable blocks | `LavaFluid.randomTick` |

### Entity-in-fluid constants

| Name | Value | Location |
|---|---|---|
| lava entity effects | `CLEAR_FREEZE`, `LAVA_IGNITE` (`Entity::lavaIgnite`), `runAfter(LAVA_IGNITE, Entity::lavaHurt)` | `LavaFluid.entityInside` |
| `lavaHurt` | `hurtServer(level, damageSources().lava(), 4.0F)` if `!fireImmune()` (server only) + burn sound pitch `2.0F + this.random.nextFloat() * 0.4F` (cosmetic) | `Entity.lavaHurt` |
| `lavaIgnite` | `if (!fireImmune()) igniteForSeconds(15.0F)` | `Entity.lavaIgnite` |
| `igniteForTicks` (LivingEntity) | `super.igniteForTicks(ceil(ticks * getAttributeValue(Attributes.BURNING_TIME)))` | `LivingEntity.igniteForTicks` |
| water entity effect | `EXTINGUISH` (`Entity::clearFire`) | `WaterFluid.entityInside` |
| on-fire damage | `remainingFireTicks % 20 == 0 && !isInLava()` → `hurtServer(onFire(), 1.0F)`; `setRemainingFireTicks(-1)` each tick | `Entity.baseTick` |
| fire resistance | `source.is(DamageTypeTags.IS_FIRE) && hasEffect(FIRE_RESISTANCE)` → `hurtServer` returns `false` | `LivingEntity.hurtServer` |
| swim drag (`travelFlying`) | water `scale(0.8F)`, lava `scale(0.5)`, air `scale(0.91F)` on `deltaMovement` after `move` | `LivingEntity.travelFlying` |
| lava fall cushion | `if (isInLava()) fallDistance *= 0.5` | `Entity.baseTick` |

## Logic (ordered)

### `BucketItem.use` (filled)

1. `hitResult = getPlayerPOVHitResult(level, player, getFluidContext())`;
   `MISS` → `PASS`; type != `BLOCK` → `PASS`.
2. `pos`, `direction`, `directionOffsetPos = pos.relative(direction)`;
   gate: `level.mayInteract(player, pos) && player.mayUseItemAt(directionOffsetPos, direction, itemStack)` else `FAIL`.
3. `placePos = clicked instanceof LiquidBlockContainer && content == WATER ? pos : directionOffsetPos`.
4. `emptyContents(player, level, placePos, hitResult)`:
   - content must be `FlowingFluid` else false.
   - `mayReplace`, `placeLiquid`, `canPlaceFluidInsideBlock` gates above.
   - If not placeable and `hitResult != null`: retry at
     `hitResult.getBlockPos().relative(hitResult.getDirection())` with null hitResult
     (one step outward; note shift-sneak then allows placement since `hitResult == null`).
   - Water in evaporating dim: sound/particles, return true (bucket consumed, no block).
   - `LiquidBlockContainer` + water: `container.placeLiquid(level, pos, blockState, flowingFluid.getSource(false))`.
   - Else destroy replaceable non-liquid (server, with drops), `setBlock(pos,
     content.defaultFluidState().createLegacyBlock(), 11)`; fail if false and not already source.
5. On success: `checkExtraContent` (no-op for BucketItem), `PLACED_BLOCK` criterion
   (server), award stat, `ItemUtils.createFilledResult(itemStack, player, getEmptySuccessItem(...))`
   → `SUCCESS.heldItemTransformedTo(emptyResult)`.
   [BucketItem.use, BucketItem.emptyContents]

### `BucketItem.use` (empty → pickup)

If `emptyContents` fails and `content == Fluids.EMPTY`: if clicked block is
`BucketPickup`: `pickupBlock(player, level, pos, blockState)` → on non-empty:
award stat, pickup sound, `GameEvent.FLUID_PICKUP`, `FILLED_BUCKET` criterion
(server), `SUCCESS.heldItemTransformedTo(result)`. Else `FAIL`.
[BucketItem.use]

### Flow scheduling

`LiquidBlock.onPlace` / `neighborChanged` / `updateShape`: if
`shouldSpreadLiquid(level, pos, state)` → `scheduleTick(pos, fluidType,
fluid.getTickDelay(level))`. `shouldSpreadLiquid` for lava first checks water
contact → converts to obsidian/cobblestone + fizz and returns false (no flow
tick). Bubble-column scheduling (`20` ticks) is separate and non-PvP.
[LiquidBlock]

### Dispenser

- Filled bucket (`DispenseItemBehavior$3`): `bucket.emptyContents(null, level,
  target /* block in front */, null)`; success → `consumeWithRemainder(..., new
  ItemStack(Items.BUCKET))`; failure → default dispense (drop as item).
- Empty bucket (`DispenseItemBehavior$4`): if target is `BucketPickup`:
  `pickupBlock(null, level, target, state)`; non-empty → `consumeWithRemainder(...,
  new ItemStack(targetType))` + `FLUID_PICKUP` game event; empty → default dispense.
  [DispenseItemBehavior$3.execute, DispenseItemBehavior$4.execute]

### Swimming / lava damage ordering per tick

1. `Entity.baseTick`: fire handling first — `remainingFireTicks % 20 == 0 &&
   !isInLava()` → 1.0F onFire damage; decrement; `isInLava()` → `fallDistance *= 0.5`.
2. `LivingEntity` travel → `travelFlying` (or fluid travel): in water
   `deltaMovement.scale(0.8F)`; in lava `scale(0.5)`; air `scale(0.91F)` — applied
   after `move`, every tick.
3. End of tick `applyEffectsFromBlocks` → `checkInsideBlocks` → fluid
   `entityInside`: water → `EXTINGUISH` (clearFire); lava → `CLEAR_FREEZE`,
   `LAVA_IGNITE` (15 s ignite if not fire-immune), then `lavaHurt` (4.0F lava
   damage, server only).
4. Fire Resistance: checked in `LivingEntity.hurtServer` — negates any
   `IS_FIRE`-tagged damage (lava, onFire, inFire). Note `lavaIgnite` only checks
   `fireImmune()` (entity-type), so a fire-resistance player in lava is still
   ignited (visual fire + remainingFireTicks) but takes no damage.

## Tick placement

- Bucket place/pickup runs inside the `use` packet handler; the fluid block is
  set immediately (`setBlock(..., 11)`), and `LiquidBlock.onPlace` schedules the
  first flow tick `getTickDelay` ticks out (water: 5 ticks; lava: 30 / 10 nether).
  Neighbor updates re-schedule on change.
- MLG relevance: water placed at tick T exists as a source block immediately;
  its first spread tick is T+5. Catching fall damage only needs the source block
  (fluid collision), not the spread.
- Lava damage/ignite/extinguish all fire on the **block-effect phase at end of
  the entity tick** (via `checkInsideBlocks` → fluid `entityInside`), i.e. after
  movement. On-fire 1.0F damage fires in `Entity.baseTick` (start of tick) when
  `remainingFireTicks % 20 == 0`.

## Randomness

| Draw | Exact RandomSource expression | Bit-for-bit reproducible? |
|---|---|---|
| Nether water-evaporation sound pitch | `random = level.getRandom()`; `2.6F + (random.nextFloat() - random.nextFloat()) * 0.8F` in `BucketItem.emptyContents` | Cosmetic only; level RNG is world-seeded but call timing varies — not reproducible in practice. |
| Lava `getSpreadDelay` ×4 | `level.getRandom().nextInt(4) != 0` in `LavaFluid.getSpreadDelay` | **Within a run, yes** — deterministic given world seed and call order; affects flow timing (3/4 chance of 4× delay when height rises). |
| Lava fire spread (`randomTick`) | `random.nextInt(3)` draws; `random` passed from `LiquidBlock.randomTick` (block random-tick RandomSource) | Within a run, deterministic given seed/order; gameplay-irrelevant for PvP. |
| `lavaHurt` burn sound pitch | `this.random.nextFloat()` — the **victim entity's own** RNG (cosmetic) | No (per-entity seeded RNG), cosmetic only. |
| Bucket place/pickup, flow scheduling, ignite seconds, fire damage | none | **Yes.** |

## Proposed oracle scenarios (150+)

- **150_water_mlg_timing**: water bucket placed at tick T on a replaceable
  target; verify `setBlock` immediate with flag `11`, first flow tick scheduled
  at T+5; falling player landing in the source at T+1 takes no fall damage
  (fluid, not block).
- **151_bucket_pickup_source_only**: empty bucket on `LEVEL == 0` water →
  source removed, water bucket returned; on `LEVEL == 1` (flowing) → `FAIL`,
  block unchanged.
- **152_nether_water_evaporate**: water bucket in dim with `WATER_EVAPORATES` →
  returns true, bucket → empty, no block placed, smoke/sound events fired.
- **153_lava_flow_tick_delay**: lava placed overworld → first flow tick at +30;
  nether (`FAST_LAVA`) → +10. Verify `getSpreadDelay` 4× branch via
  `level.getRandom().nextInt(4)` stub.
- **154_lava_damage_ignite**: player standing in lava source: each tick
  `lavaHurt` → `4.0F` lava damage (server) and `lavaIgnite` → 15 s fire;
  `fallDistance *= 0.5` per tick in `baseTick`. With Fire Resistance: ignite
  still applies, all `IS_FIRE` damage negated.
- **155_swim_drag**: player moving in water vs lava with same input:
  `deltaMovement` scaled `0.8F` vs `0.5` after `move` each tick
  (`travelFlying`); water also `EXTINGUISH`es fire at end of tick.
- **156_dispenser_bucket**: dispenser facing water source with empty bucket →
  filled bucket in slot; facing air with water bucket → source placed in front.

## Open questions

- Exact `getPlayerPOVHitResult` reach distance / fluid `SOURCE_ONLY` raycast
  semantics not read (assumed standard 4.5-block survival reach); matters for
  MLG placement range — worth a follow-up pass.
- `LiquidBlockContainer.canPlaceLiquid` implementations per block (which blocks
  accept waterlogging) not enumerated.
- `FlowingFluid` spread shape algorithm not traced (declared out of scope for
  PvP except timing, which is covered).
- `updateSwimming` / sprint-swim speed attribute interactions not read.
- `ItemUtils.createFilledResult` stack-merge behavior (creative/survival edge
  cases) not verified.
