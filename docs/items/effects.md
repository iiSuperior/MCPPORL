# Status effects used in kits (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

Most kit effects are plain attribute modifiers applied while the effect is
active; a few have tick logic (`Regeneration`, `Poison`), damage-pipeline
hooks (`Resistance`, `Fire Resistance`), or instant application
(`Instant Health`/`Instant Damage`). Effect durations tick down once per
server tick in `LivingEntity.tickEffects` → `MobEffectInstance.tickServer`.

## Data

### Attribute-modifier effects (`MobEffects` registration)

| Effect | Attribute | Amount (literal) | Operation |
|---|---|---|---|
| SPEED | `MOVEMENT_SPEED` | `0.2F` | `ADD_MULTIPLIED_TOTAL` |
| SLOWNESS | `MOVEMENT_SPEED` | `-0.15F` | `ADD_MULTIPLIED_TOTAL` |
| STRENGTH | `ATTACK_DAMAGE` | `3.0` (double) | `ADD_VALUE` |
| WEAKNESS | `ATTACK_DAMAGE` | `-4.0` (double) | `ADD_VALUE` |
| ABSORPTION | `MAX_ABSORPTION` | `4.0` (double) | `ADD_VALUE` |
| JUMP_BOOST | `SAFE_FALL_DISTANCE` | `1.0` (double) | `ADD_VALUE` |

(Modifier ids `minecraft:effect.<name>`; applied/removed by
`MobEffect.addAttributeModifiers`/`removeAttributeModifiers` on effect add,
remove, and amplifier change.)

Amplifier scaling: `createModifiers(amplifier, …)` multiplies the template
amount by `(amplifier + 1)` — verified in `MobEffect`; i.e. Strength II adds
`3.0 * 2 = 6.0` attack damage. (Unverified for 26.3? No — verified: the
`AttributeTemplate` application multiplies by amplifier+1. See open
questions if this needs a second look.)

### Tick-logic effects

| Effect | Class | Interval | Action |
|---|---|---|---|
| REGENERATION | `RegenerationMobEffect` | `50 >> amp` ticks (`tickCount % interval == 0`; `interval ≤ 0` → every tick) | `heal(1.0F)` if `health < maxHealth` |
| POISON | `PoisonMobEffect` (`DAMAGE_INTERVAL = 25`) | `25 >> amp` ticks | `hurtServer(magic, 1.0F)` if `health > 1.0F` (cannot kill) |
| ABSORPTION | `AbsorptionMobEffect` | every tick | `onEffectStarted`: `setAbsorptionAmount(max(current, 4·(1+amp)))`; tick continues while absorption > 0 |

### Pipeline-hook effects

- **RESISTANCE**: in `LivingEntity.getDamageAfterMagicAbsorb`
  (after armor, before protection enchantments):
  `absorbValue = (amplifier + 1) * 5`; `damage = max(damage * (25 -
  absorbValue) / 25.0F, 0.0F)`. Resistance I → ×0.8, II → ×0.6, IV → ×0.2.
  Skipped if the damage type is `BYPASSES_RESISTANCE`; whole step skipped if
  `BYPASSES_EFFECTS`. If damage hits 0 the method returns early (no
  enchantment step).
- **FIRE_RESISTANCE**: in `LivingEntity.hurtServer`, before everything:
  `source.is(IS_FIRE) && hasEffect(FIRE_RESISTANCE)` → return false
  (no damage, no knockback, no cooldown changes).
- **INSTANT_HEALTH / INSTANT_DAMAGE** (`HealOrHarmMobEffect extends
  InstantaneousMobEffect`): on tick (e.g. suspicious stew — n/a in kits) or
  instant application: heal `max(4 << amp, 0)` or magic damage `6 << amp`;
  the instantaneous path scales: `(int)(scale * (4 << amp) + 0.5)` /
  `(int)(scale * (6 << amp) + 0.5)`, damage via `magic()` or
  `indirectMagic(source, owner)`. Undead invert heal/harm
  (`isInvertedHealAndHarm`).

### Absorption hearts in the damage pipeline

`actuallyHurt`: `dmg = max(dmg - absorptionAmount, 0)`; absorption reduced by
the absorbed part first, then by remaining `dmg` again after health is
reduced (`setAbsorptionAmount(getAbsorption - dmg)`). Absorption is capped by
`setAbsorptionAmount` → `Mth.clamp(amount, 0, getMaxAbsorption())`
(`MAX_ABSORPTION` attribute, default `0.0`, max `2048.0`).

## Logic

### Effect ticking (server)

`LivingEntity.tickEffects`, every server tick:
- For each active effect: `MobEffectInstance.tickServer(level, this, …)`:
  - `!hasRemainingDuration()` → effect ends (removed).
  - `tickCount = duration` (or `tickCount` of the entity if infinite);
    if `shouldApplyEffectTickThisTick(tickCount, amp)` → `applyEffectTick`;
    returning false ends the effect.
  - `tickDownDuration()` (duration − 1).
- Note `tickCount` here is the *remaining* duration for the interval test:
  Regen II procs when `duration % 25 == 0`.

### Effect application order for consumables/death

`ApplyStatusEffectsConsumeEffect`: each `MobEffectInstance` added via the
normal `addEffect` path (refreshes duration/amplifier per the usual rules —
stronger/longer wins; exact merge rules not re-verified in 26.3, see open
questions). Totem clears all effects first.

## Tick placement

- `tickEffects` runs in the entity's server tick (part of `LivingEntity`
  base tick, after movement/physics on the server copy — exact position
  relative to `tickPlayer` not yet mapped; for the oracle what matters is
  once per server tick, before/after the attack packet handling of that
  tick — the attack packet is handled before the server tick per
  `docs/ARCHITECTURE.md`, so a regen tick and a hit in the same tick apply
  regen first only if `tickEffects` runs before packet handling… **this
  ordering is currently unverified**, see open questions).
- Client side ticks only particles (`tickClient`); durations shown on the
  client come from synced entity data.

## Randomness

None in any of these effects' math (poison/regen/instant have no rolls).

## Oracle scenarios

1. `90_regen_timing`: B at 10 health, Regen II 100t (golden apple). Expect
   heals at durations 100, 75, 50, 25 (interval `50>>1 = 25`, tested on
   remaining duration): +1.0 each → 14.0 after 100 ticks.
2. `91_strength_damage`: A with Strength I (attack damage +3) diamond sword:
   expect 9.0 per full hit; Strength II → 12.0.
3. `92_resistance`: B Resistance I, A diamond sword: 6 → 4.8.
4. `93_poison`: B Poison I; expect 1.0 magic damage every 25 ticks while
   health > 1 (verify it cannot kill: health floors at 1.0).
5. `94_absorption_stack`: golden apple then enchanted apple: absorption =
   max(4, 16) = 16 (re-application takes the max, capped by MAX_ABSORPTION).
6. `95_fire_res`: B with Fire Resistance takes `IS_FIRE` damage → no damage,
   no knockback, `hurtServer` returns false immediately.

New trace fields: active effects with `(effect, duration, amplifier)` per
tick; `absorptionAmount`.

## Open questions

- `MobEffectInstance` merge rules on re-application (same effect, higher
  amplifier vs longer duration) — assumed classic rules, not re-verified.
- Exact position of `tickEffects` within `ServerPlayer.tick` relative to
  packet handling — needed for same-tick regen+hit ordering.
- `createModifiers` amplifier multiplication verified: `AttributeTemplate.create`
  builds `new AttributeModifier(id, amount * (amplifier + 1), operation)`
  (`MobEffect`, line ~201) — Strength II = +6.0 attack damage.
- `SAFE_FALL_DISTANCE` interplay with crit fall-distance accounting is
  phase-1 territory; jump boost itself adds `1.0·(amp+1)`.
