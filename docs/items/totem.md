# Totem of Undying (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

The totem is a `DEATH_PROTECTION` data component, not item logic:
`DeathProtection.TOTEM_OF_UNDYING`. When `LivingEntity.hurtServer` would
otherwise kill the player, `checkTotemDeathProtection` consumes one totem
from either hand (main hand first), sets health to 1.0, clears all effects,
and grants Regeneration II (45s), Absorption II (5s) and Fire Resistance (40s).

## Data

| Constant | Value (source literal) | Source |
|---|---|---|
| Component | `DataComponents.DEATH_PROTECTION` → `DeathProtection.TOTEM_OF_UNDYING` | `Items.TOTEM_OF_UNDYING` (stacks to 1, `UNCOMMON`) |
| Death effects | `ClearAllStatusEffectsConsumeEffect`, then `ApplyStatusEffectsConsumeEffect` with `Regeneration(900, 1)`, `Absorption(100, 1)`, `Fire Resistance(800, 0)` | `DeathProtection.TOTEM_OF_UNDYING` |
| Health set | `1.0F` | `LivingEntity.checkTotemDeathProtection` |
| Bypass tag | `DamageTypeTags.BYPASSES_INVULNERABILITY` → no proc | same |
| Client event | `broadcastEntityEvent(this, (byte)35)` | same |

Effect durations in ticks: Regen 900 (45 s) amp 1; Absorption 100 (5 s) amp 1
(→ 8 absorption hearts: `4·(1+1)`); Fire Resistance 800 (40 s) amp 0.

## Logic

`LivingEntity.checkTotemDeathProtection(killingDamage)` (private), called at
the **end** of `hurtServer`, only if `isDeadOrDying()` after `actuallyHurt`
has already reduced health (possibly below 0 — `setHealth` clamps at 0):

1. `killingDamage.is(BYPASSES_INVULNERABILITY)` → return false (void,
   `/kill`-style damage).
2. Hands in `InteractionHand.values()` order (MAIN_HAND, then OFF_HAND):
   first stack with a `DEATH_PROTECTION` component wins. `protectionItem =
   itemStack.copy()`; `itemStack.shrink(1)`; stop searching.
3. If found and this is a `ServerPlayer`: `ITEM_USED` stat,
   `CriteriaTriggers.USED_TOTEM`, `ITEM_INTERACT_FINISH` vibration.
4. `setHealth(1.0F)`; `protection.applyEffects(protectionItem, this)` —
   clears all status effects first, then applies the three instances;
   `broadcastEntityEvent(35)` (totem particles/animation on clients).
5. Return `protection != null`. `hurtServer`: if true, `die()` is skipped
   (and the death sounds are skipped); the method still returns `success`.

Notes:
- Only **one** totem is consumed even if both hands hold one (loop breaks).
- The totem works from either hand, including off-hand while blocking.
- Absorption from a previous totem/apple is cleared first (clear-all runs
  before the new effects), so absorption does not stack across procs.
- `setHealth(1.0F)` happens before `applyEffects`, so Regeneration ticks
  from full effect duration.

## Tick placement

Inside the server tick's attack-packet handling, at the tail of
`hurtServer` — after damage, armor, absorption, knockback, and after
`actuallyHurt` set health ≤ 0. A same-tick trade (both players lethal):
the first-processed attacker's victim procs the totem and survives;
per `docs/ARCHITECTURE.md` the dead player's remaining packets are ignored.

## Randomness

None in the totem path itself.

## Oracle scenarios

1. `80_totem_proc`: B at 1 health holding totem in off-hand; A diamond-sword
   crit (9 damage). Expect: B survives at 1.0 health, totem consumed,
   effects [Regen 900/1, Absorption 100/1, FireRes 800/0], no death.
   Trace: health, effects, off-hand stack count.
2. `81_totem_main_hand_priority`: totems in both hands; lethal hit.
   Expect: main-hand stack shrinks, off-hand untouched.
3. `82_totem_bypass`: lethal `BYPASSES_INVULNERABILITY` damage (harness
   only) → no proc, death.
4. `83_totem_trade`: both at 1 health, A and B lethal same tick. Expect: A's
   packets first → B procs totem and survives at 1.0; B's attack packet is
   ignored (connection marked unloaded only on actual death — B didn't die,
   so does B's attack still land? This scenario pins the ordering: totem
   proc happens inside A's packet handling, B is alive when its own packets
   are handled → B's attack lands on A. Contrast with `28_trade_kill`.)
5. `84_totem_absorption_clear`: B with absorption 8.0 from an apple takes
   lethal damage with totem. Expect absorption reset to 8.0 (not 16).

New trace fields: `totemProcced` (bool), per-hand stack counts after the hit.

## Open questions

- What `broadcastEntityEvent(35)` drives on the client (particles + sound +
  animation) — cosmetic; the sim may want to count it as an observable.
- Interaction with `BYPASSES_INVULNERABILITY` vs `BYPASSES_COOLDOWN` tags
  for exotic damage types — data-side, in `data/minecraft/damage_type/`.
