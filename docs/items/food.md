# Food, golden apples, enchanted golden apples (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

Eating is the "using item" flow with a `Consumable` component: 32 ticks of
eating, then the food's nutrition/saturation apply and its status effects are
granted. Golden apples are `alwaysEdible` (4 nutrition, 9.6 saturation).
Sprinting needs food level > 6. While eating (or blocking), movement input is
scaled to 0.2× on the client and sprinting is suppressed.

## Data

| Constant | Value (source literal) | Source |
|---|---|---|
| Eat duration | `(int)(1.6F * 20.0F)` = **32 ticks** | `Consumable.consumeTicks()`; `Consumables.defaultFood()` sets `consumeSeconds(1.6F)` |
| Eat animation / sound | `EAT`, `GENERIC_EAT`, particles on | `Consumables.defaultFood()` |
| Golden apple nutrition | `4` | `Foods.GOLDEN_APPLE` (`nutrition(4)`) |
| Golden apple saturation | `4 * 1.2F * 2.0F` = **9.6F** | `saturationModifier(1.2F)` via `FoodConstants.saturationByModifier` |
| Always edible | `true` | `Foods.*.alwaysEdible()` |
| Golden apple effects | Regen `100` ticks amp `1`; Absorption `2400` ticks amp `0` | `Consumables.GOLDEN_APPLE` |
| Enchanted golden apple effects | Regen `400`/`1`; Resistance `6000`/`0`; Fire Resistance `6000`/`0`; Absorption `2400`/`3` | `Consumables.ENCHANTED_GOLDEN_APPLE` |
| Max food / saturation | `20`, `20.0F`; start saturation `5.0F` | `FoodConstants.MAX_FOOD`, `MAX_SATURATION`, `START_SATURATION` |
| Sprint food requirement | `foodLevel > 6` (`SPRINT_LEVEL = 6`) | `FoodData.hasEnoughFood()`; used by `Player.hasEnoughFoodToDoExhaustiveManoeuvres()` |
| Natural regen (saturated) | every `10` ticks, heal `min(sat,6)/6`, exhaustion += spent | `FoodData.tick` (`HEALTH_TICK_COUNT_SATURATED = 10`) |
| Natural regen (unsaturated) | every `80` ticks, heal `1.0F`, exhaustion `6.0F`, needs food ≥ 18 | `FoodData.tick` (`HEALTH_TICK_COUNT = 80`, `HEAL_LEVEL = 18`) |
| Exhaustion drop | `> 4.0F` → `-4.0F`, then saturation −1 or food −1 | `FoodData.tick` (`EXHAUSTION_DROP = 4.0F`) |
| Exhaustion cap | `40.0F` | `FoodData.addExhaustion` |
| Attack exhaustion | `0.1F` | `FoodConstants.EXHAUSTION_ATTACK`; applied in `Player.attack` |
| Sprint exhaustion | `0.1F`/tick | `FoodConstants.EXHAUSTION_SPRINT` |
| Use-effects while eating | input ×`0.2F`, `canSprint=false` | `UseEffects.DEFAULT` (golden apples set no override) |
| Starve | every `80` ticks, `1.0` damage when food ≤ 0 | `FoodData.tick` |

## Logic

### Eating (both sides tick, server applies)

1. Right-click: `Item.use` → `Consumable.startConsuming` → if
   `consumeTicks() > 0`: `user.startUsingItem(hand)` (same "using" state as
   the shield; `useItemRemaining = 32`).
2. Each tick `LivingEntity.updatingUsingItem` decrements; particles/sounds
   at intervals (`CONSUME_EFFECTS_INTERVAL = 4`,
   `CONSUME_EFFECTS_START_FRACTION = 0.21875F` — cosmetic).
3. `completeUsingItem` (server, or client if `isUsingItem()`):
   `useItem.finishUsingItem(level, this)` → `Consumable.onConsume` →
   `FoodProperties.onConsume`: eat sound (`1.0F` vol,
   `random.triangle(1.0F, 0.4F)` pitch — **entity random**), then for players
   `foodData.eat(nutrition, saturation)` and the burp
   (`0.5F` vol, `Mth.randomBetween(random, 0.9F, 1.0F)`); then each
   `onConsumeEffects` applies (the `MobEffectInstance`s above).
4. `FoodData.eat(FoodProperties)`: `add(nutrition, saturation)`:
   `foodLevel = min(foodLevel + n, 20)`, `saturationLevel =
   min(saturationLevel + s, 20.0F)`.

`canAlwaysEat` gates *starting* to eat when full — golden apples bypass it
(`startConsuming` checks it; verified the flag exists on the component; the
exact check site is in `Consumable.startConsuming`, body not fully read —
see open questions).

### `FoodData.tick` (server, every `Player` tick)

1. `exhaustionLevel > 4.0F`: `-= 4.0F`; `saturationLevel > 0` → `-1`
   (floor 0), else (not peaceful) `foodLevel - 1` (floor 0).
2. Natural regen (game rule on): saturation > 0, hurt, food ≥ 20 →
   `tickTimer++`, every 10: `heal(min(sat,6)/6)`, `addExhaustion(min(sat,6))`.
   Else food ≥ 18 and hurt → every 80: `heal(1.0F)`, `addExhaustion(6.0F)`.
   Else food ≤ 0 → every 80: starve `hurtServer(…, 1.0F)` (unless peaceful
   rules protect). Else `tickTimer = 0`.
3. The oracle disables natural regen (hunger not yet a domain).

### Sprinting and movement while eating

- Client `canStartSprinting()`: needs `hasEnoughFoodToDoExhaustiveManoeuvres()`
  = `foodData.hasEnoughFood() || mayfly` = `foodLevel > 6`, plus
  `!isSlowDueToUsingItem()` — eating or blocking zeroes `sprintTriggerTime`
  and blocks sprint start. (All `LocalPlayer`, verified.)
- Input scale while using: `newInput.scale(itemUseSpeedMultiplier())` =
  ×`0.2F`, applied after the 0.98 walk multiplier, before sneaking.

## Tick placement

- Client: right-click in `handleKeybinds` → predicted using state + 0.2×
  input immediately; `ServerboundUseItemPacket` sent with the tick's packets.
- Server: packet → `startUsingItem` (32 ticks). `updatingUsingItem` ticks it
  down each server tick; `completeUsingItem` applies food + effects on the
  server copy. The client also runs `completeUsingItem` when its predicted
  state says so (prediction; the server re-syncs food via
  `ClientboundSetHealthPacket` — packet name unverified, see open questions).
- `FoodData.tick` runs in the server player tick, after movement/physics
  (exact position within `ServerPlayer.tick` not yet mapped; hunger is
  outside the known domains for now).

## Randomness

- Eat sound pitch: `random.triangle(1.0F, 0.4F)` — the **eating entity's**
  `RandomSource` (per-entity, not reproducible).
- Burp pitch: `Mth.randomBetween(random, 0.9F, 1.0F)` — same source.
- No RNG in nutrition, saturation, durations, or exhaustion.

## Oracle scenarios

1. `60_eat_golden_apple`: B at 10 health eats a golden apple uninterrupted.
   Expect after 32 ticks: food +4 (clamp 20), saturation +9.6, Regen II
   100t, Absorption I (4 hearts → absorption 4.0) 2400t. Trace: `foodLevel`,
   `saturation`, active effects with durations.
2. `61_eat_interrupted`: B starts eating, A hits at tick 10. Expect: eating
   aborted (damage → `stopUsingItem`? verify: hurt does not automatically
   stop using — check `releaseUsingItem` call sites; scenario pins it),
   no effects.
3. `62_sprint_hunger_gate`: B food set to 6, tries to sprint. Expect: no
   sprint (client rule); server copy keeps `!sprinting`.
4. `63_enchanted_apple`: full effect list with durations/amplifiers;
   absorption 2400t amp 3 → 16 absorption hearts (4·(1+3)).
5. `64_move_while_eating`: B eats while walking forward; measure displacement
   vs normal walk → 0.2× input scaling.

New trace fields: `foodLevel`, `saturationLevel`, `exhaustionLevel`,
`useItemRemaining`, active `MobEffectInstance`s (effect, duration, amplifier).

## Open questions

- Exact `canAlwaysEat` check site in `Consumable.startConsuming` (body only
  partially read).
- Whether the client predicts `FoodData` changes or waits for the server
  health packet (matters for the client copy in the oracle).
- Whether taking damage interrupts eating on the server (`stopUsingItem`
  call sites in `hurtServer` — not seen; likely the item keeps being used).
- `FoodData` sync: exact packet/fields the server sends (`ClientboundSetHealthPacket`
  assumed from older versions — unverified in 26.3).
