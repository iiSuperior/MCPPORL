# Enchantments used in kits (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

26.3 enchantments are data-driven: each enchantment's JSON
(`data/minecraft/enchantment/*.json`, shipped **in the client jar**) lists
effect entries keyed by component type (`minecraft:damage`,
`minecraft:damage_protection`, `minecraft:knockback`, `minecraft:post_attack`,
`minecraft:attributes`, `minecraft:item_damage`, `minecraft:repair_with_xp`,
…). `EnchantmentHelper`'s static helpers iterate an item's (or victim's
equipment's) enchantments and feed each level into the matching effect list.
Numeric values below come from the 26.3 JSON; the dispatch code from the jar.

## Data

### Effect-component dispatch (`Enchantment`, verified bodies)

| Helper | Reads effect list | Iterates |
|---|---|---|
| `EnchantmentHelper.modifyDamage(level, item, victim, source, dmg)` | `EnchantmentEffectComponents.DAMAGE` | the weapon item's enchantments |
| `EnchantmentHelper.modifyKnockback(level, item, victim, source, kb)` | `EnchantmentEffectComponents.KNOCKBACK` | the weapon item's enchantments |
| `EnchantmentHelper.getDamageProtection(level, victim, source)` | `EnchantmentEffectComponents.DAMAGE_PROTECTION` | victim's equipment enchantments (accumulates into a `MutableFloat` from 0) |
| `EnchantmentHelper.modifyArmorEffectiveness(level, item, victim, source, frac)` | `EnchantmentEffectComponents.ARMOR_EFFECTIVENESS` (via the vanilla-typo'd `Enchantment.modifyArmorEffectivness`) | the attacker's weapon item's enchantments |
| `EnchantmentHelper.doPostAttackEffects(level, victim, source)` | `POST_ATTACK` | (fire aspect — victim ignite) |
| `EnchantmentHelper.processDurabilityChange(level, item, amount)` | `ITEM_DAMAGE` | the damaged item's enchantments |

`LevelBasedValue` JSON forms: `linear{base, per_level_above_first}` =
`base + per_level_above_first·(level−1)`; `fraction{numerator, denominator}`.

### Kit enchantment values (26.3 JSON)

| Enchant | Effect | Value per level (float/double as in JSON) |
|---|---|---|
| sharpness V | `damage`: `add` | `1.0 + 0.5·(L−1)` → L5 = **3.0** |
| smite / bane_of_arthropods V | `damage`: `add` | `2.5 + 2.5·(L−1)` → L5 = **15.0** (plus BoA applies slowness via `post_attack/apply_mob_effect`) |
| knockback II | `knockback`: `add` | `1.0 + 1.0·(L−1)` → L2 = **2.0** (added to `ATTACK_KNOCKBACK`, then **halved** in `getKnockback`) |
| fire_aspect II | `post_attack`: `ignite` (direct hits only) | duration `4.0 + 4.0·(L−1)` **seconds** (`Ignite.apply` → `entity.igniteForSeconds(…)`) |
| sweeping_edge III | `attributes`: `sweeping_damage_ratio`, `add_value` | `(1.0+1.0·(L−1)) / (2.0+1.0·(L−1))` → L3 = **0.75** |
| protection IV | `damage_protection`: `add` | `1.0 + 1.0·(L−1)` → L4 = **4.0** |
| fire/blast/projectile protection IV | `damage_protection`: `add` | `2.0 + 2.0·(L−1)` → L4 = **8.0** |
| feather_falling IV | `damage_protection`: `add` | `3.0 + 3.0·(L−1)` → L4 = **12.0** |
| unbreaking III | `item_damage`: `remove_binomial` | ignore chance per point: tools/weapons `L/(L+1)`; **armor** `(2+2·(L−1))/(10+5·(L−1))` (weaker on armor in 26.3 — verified JSON) |
| mending | `repair_with_xp`: `multiply` | factor `2.0` |
| breach (mace) | `armor_effectiveness` | (phase 5; hook verified, values not extracted) |

Notes:
- Protection's entry has a `requirements` predicate: skips damage types in
  `#minecraft:bypasses_invulnerability`.
- `damage_protection` values from all worn pieces **sum** (via the
  `MutableFloat` accumulator), then `CombatRules.getDamageAfterMagicAbsorb`
  clamps the total to 20 and multiplies damage by `(1 − total/25)`.
- Fire/blast protection JSONs also carry `attributes` entries (knockback
  resistance etc.) — not extracted; outside kit scope.

### `remove_binomial` semantics (`RemoveBinomial.process`, verified)

```java
float p = chance.calculate(level); int drop = 0;
if (n > 128 && n*p >= 20 && n*(1-p) >= 20) {
    drop = clamp(round(floor(n*p) + random.nextGaussian()*sqrt(n*p*(1-p))), 0, n);
} else {
    for (y in 0..n) if (random.nextFloat() < p) drop++;
}
return n - drop;
```

i.e. each point of durability damage is independently ignored with
probability `p` (gaussian approximation for large n). Uses the **server
level random**.

## Logic

### Damage (`Player.attack`, server)

`ServerPlayer.getEnchantedDamage` = `EnchantmentHelper.modifyDamage(…)`:
sharpness applies to the *unscaled* base damage, then
`magicBoost = attackStrengthScale · (modified − base)`. Crit (×1.5) does not
touch the enchantment bonus. Sweep victims get
`getEnchantedDamage(nearby, sweepDamage, …) · attackStrengthScale`.

### Knockback

`LivingEntity.getKnockback`: `ATTACK_KNOCKBACK` attribute (default `0.0`,
max `5.0`) → server: `modifyKnockback(…)/2.0F`. Knockback II on a 0-base
weapon: `(0 + 2.0)/2 = 1.0` added to the `causeExtraKnockback` strength.

### Protection

`getDamageAfterMagicAbsorb` (server only; client gets 0): sums
`damage_protection` over equipment → `CombatRules.getDamageAfterMagicAbsorb`
(clamp 20, ×(1−t/25)). Runs after the resistance effect, before absorption.

### Armor durability (unbreaking)

`ItemStack.hurtAndBreak` → `processDurabilityChange(level, item, amount)`
applies `remove_binomial` per the item's unbreaking level, then
`applyDamage`. Same path for shield blocking damage (`hurtBlockingItem`) and
armor (`doHurtEquipment`) and weapon use (`itemDamagePerAttack`).

### Mending

`repair_with_xp` ×2.0: XP orbs repair at 2 durability per XP (the orb logic
itself is outside PvP scope; value recorded for completeness).

## Tick placement

All enchantment math runs inside the server tick's attack-packet handling
(`Player.attack` → `hurtServer` → `actuallyHurt` → `getDamageAfterMagicAbsorb`)
or inside `hurtAndBreak` (same tick). No per-tick state.

## Randomness

- Unbreaking: server-level `RandomSource` per durability point (or one
  gaussian draw for large n). Per-entity irreproducible; the sim should count
  these draws.
- No RNG in sharpness/protection/knockback/fire-aspect math.

## Oracle scenarios

1. `90_sharpness`: A diamond sword sharpness V, full strength: expect
   6 + 3 = 9.0 (magicBoost = 1.0·3.0).
2. `91_knockback_ench`: knockback II diamond sword sprint hit: expect the
   extra-knockback strength 0.5 (sprint) + 1.0 (enchant) on top of base.
3. `92_protection`: B full protection IV (total 16): diamond sword 6.0 →
   armor step then ×(1−16/25)=×0.36.
4. `93_fire_aspect`: fire aspect II hit → victim `remainingFireTicks` =
   8·20 = 160 (verify `igniteForSeconds` = ×20).
5. `94_unbreaking_statistical`: 200 hits with unbreaking III diamond sword;
   durability loss distribution ≈ binomial(200·1, 0.25 kept)… (statistical,
   not bit-exact — documents the distribution for the sim's stand-in RNG).
6. `95_sweeping_edge`: sweeping edge III, sweep scenario from weapons.md:
   ratio 0.75 → sweep damage `1.0 + 0.75·6.0 = 5.5` before enchant scaling.

## Open questions

- `fire/blast_protection` `attributes` entries' exact values (knockback
  resistance?) — not extracted; rarely kit-relevant.
- Thorns effect components and retaliation damage — not extracted.
- `doPostAttackEffects` ordering vs `hurtServer` return (fire aspect applies
  after damage — verified call site in `Player.attack` via
  `itemAttackInteraction`? actually `doPostAttackEffectsWithItemSource` at
  `Player.attack` line ~1097 — after the hit; exact ordering vs knockback
  verified in source but not re-checked here).
- `processDurabilityChange` full body (how multiple `item_damage` entries
  compose) — only `RemoveBinomial.process` verified.
