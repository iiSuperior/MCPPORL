# Armor (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

Armor is fully data-driven in 26.3: each piece is a plain `Item` with
`ItemAttributeModifiers` (`ARMOR`, `ARMOR_TOUGHNESS`, `KNOCKBACK_RESISTANCE`)
built from its `ArmorMaterial`, plus an `Equippable` component. Damage
reduction runs inside `LivingEntity.hurtServer` → `actuallyHurt` via
`CombatRules`, with the breach enchantment able to shrink the armor fraction.
Each hit also damages every worn piece by `max(1, damage/4)`.

## Data

### Armor materials (`ArmorMaterial` record; defense order boots/legs/chest/helm)

| Material | durability × | boots | legs | chest | helm | enchantability | toughness | knockbackRes |
|---|---|---|---|---|---|---|---|---|
| LEATHER | `5` | 1 | 2 | 3 | 1 | `15` | `0.0F` | `0.0F` |
| COPPER | `11` | 1 | 3 | 4 | 2 | `8` | `0.0F` | `0.0F` |
| CHAINMAIL | `15` | 1 | 4 | 5 | 2 | `12` | `0.0F` | `0.0F` |
| IRON | `15` | 2 | 5 | 6 | 2 | `9` | `0.0F` | `0.0F` |
| GOLD | `7` | 1 | 3 | 5 | 2 | `25` | `0.0F` | `0.0F` |
| DIAMOND | `33` | 3 | 6 | 8 | 3 | `10` | `2.0F` | `0.0F` |
| TURTLE_SCUTE | `25` | 2 | 5 | 6 | 2 | `9` | `0.0F` | `0.0F` |
| NETHERITE | `37` | 3 | 6 | 8 | 3 | `15` | `3.0F` | `0.1F` |
| ARMADILLO_SCUTE | `4` | 3 | 6 | 8 | 3 | `10` | `0.0F` | `0.0F` |

(`ArmorMaterials` interface constants; `makeDefense(boots, legs, chest, helm, body)`.)

- Piece durability = `ArmorType` unit durability × material multiplier.
  Unit durability: helmet `11`, chestplate `16`, leggings `15`, boots `13`
  (`ArmorType` enum). E.g. diamond chestplate: `16 × 33` = 528.
- `ArmorMaterial.createAttributes(type)`: `ARMOR += defense` (`ADD_VALUE`,
  id `minecraft:armor.<type>`), `ARMOR_TOUGHNESS += toughness`,
  `KNOCKBACK_RESISTANCE += knockbackResistance` **only if > 0** (netherite
  only: 0.1 per piece → 0.4 for a full set).
- `Item.Properties.humanoidArmor(material, type)`: durability as above,
  the attributes, `enchantable(material.enchantmentValue)`,
  `EQUIPPABLE` (slot, equip sound, asset), `repairable`.

### Attributes (`Attributes`)

| Attribute | default | min | max |
|---|---|---|---|
| `ARMOR` | `0.0` | `0.0` | `30.0` |
| `ARMOR_TOUGHNESS` | `0.0` | `0.0` | `20.0` |
| `KNOCKBACK_RESISTANCE` | `0.0` | `-2.0` | `1.0` |

### `CombatRules` constants

`MAX_ARMOR = 20.0F`, `ARMOR_PROTECTION_DIVIDER = 25.0F`,
`BASE_ARMOR_TOUGHNESS = 2.0F`, `MIN_ARMOR_RATIO = 0.2F`.

### Armor damage reduction (`CombatRules.getDamageAfterAbsorb`)

```java
float toughness = 2.0F + armorToughness / 4.0F;
float realArmor = Mth.clamp(totalArmor - damage / toughness, totalArmor * 0.2F, 20.0F);
float armorFraction = realArmor / 25.0F;
// breach hook:
modifiedArmorFraction = clamp(EnchantmentHelper.modifyArmorEffectiveness(level, weaponItem, victim, source, armorFraction), 0.0F, 1.0F);
return damage * (1.0F - modifiedArmorFraction);
```

All `float`. Note the breach hook reads the *attacker's weapon item* and can
only shrink the fraction (clamped to [0,1]).

### Magic/enchantment reduction

- `LivingEntity.getDamageAfterMagicAbsorb`: resistance effect first (see
  effects.md), then `EnchantmentHelper.getDamageProtection` (server only),
  then `CombatRules.getDamageAfterMagicAbsorb(damage, enchantArmor)`:
  `damage * (1 - clamp(enchantArmor, 0, 20)/25)`.
- Full pipeline in `actuallyHurt`: armor absorb → magic absorb → absorption
  hearts (`dmg = max(dmg - absorption, 0)`) → health.

### Durability loss on hit

- `Player.hurtArmor` → `doHurtEquipment(source, damage, FEET, LEGS, CHEST, HEAD)`.
- `doHurtEquipment`: if `damage > 0`: `durabilityDamage =
  (int)Math.max(1.0F, damage / 4.0F)`; per slot, if the piece has
  `EQUIPPABLE` with `damageOnHurt` (default true), `isDamageableItem`, and
  `canBeHurtBy(source)`: `hurtAndBreak(durabilityDamage, …)`.
- `ItemStack.hurtAndBreak` → `processDurabilityChange` (unbreaking roll) →
  `applyDamage`.

### Knockback resistance

`LivingEntity.knockback`: `power *= 1.0 - getAttributeValue(KNOCKBACK_RESISTANCE)`
first; if `power <= 0.0` the knockback is skipped entirely (netherite set =
0.4 → 60% of knockback; note the attribute allows >1, which would invert it).

## Logic

Order inside `hurtServer` (server): blocking (shield.md) → freezing/helmet →
cooldown gate → `actuallyHurt`:
1. `getDamageAfterArmorAbsorb`: unless `BYPASSES_ARMOR`: `hurtArmor`
   (durability), then `CombatRules.getDamageAfterAbsorb` with
   `getArmorValue()` (sum of `ARMOR` attribute) and `ARMOR_TOUGHNESS`.
2. `getDamageAfterMagicAbsorb`: unless `BYPASSES_EFFECTS`: resistance effect,
   then protection enchantments.
3. Absorption hearts, then `setHealth(health - dmg)`.

Difficulty scaling for players happens in `Player.hurtServer` *before* all of
this (only if `source.scalesWithDifficulty()`: peaceful → 0, easy →
`min(d/2+1, d)`, hard → `d*3/2`).

## Tick placement

Everything is inside the server tick's attack-packet handling, in
`hurtServer` → `actuallyHurt`. Armor attributes are synced to the client
(`setSyncable(true)`) but the client never reduces damage itself. Durability
changes sync via the normal inventory/equipment packets.

## Randomness

- None in the reduction math. Unbreaking's binomial draw uses the server
  level random inside `processDurabilityChange` (per durability point for
  small amounts; gaussian approximation above the `n>128 / np≥20` thresholds
  — see enchantments.md).

## Oracle scenarios

1. `50_armor_values`: B in full diamond vs full iron vs none; A diamond sword
   full-strength. Expect per-hit damage 6→(armor 20, tough 8: 6·(1−0.76)) etc.
   — compute exact expectations from the formula in the scenario file.
2. `51_armor_durability`: 10 diamond-sword hits on full diamond; trace each
   piece's durability (−max(1, 6/4)=−1 per hit… note damage/4 uses the
   pre-armor damage).
3. `52_netherite_kb`: full netherite on B; sprint hit. Expect knockback ×0.6.
4. `53_breach`: (needs breach enchant — data exists in 26.3: `density`/`breach`
   are mace enchantments; scenario is a stretch goal) — placeholder.
5. `54_protection_stacking`: B with protection IV on all pieces (EPF 16):
   verify the `getDamageAfterMagicAbsorb` step multiplies after armor.

New trace fields: `armorValue`, `armorToughness`, per-piece durability,
`knockbackResistance`.

## Open questions

- `getArmorValue()` body not read (assumed sum of `ARMOR` attribute; the
  call in `getDamageAfterArmorAbsorb` passes it straight to `CombatRules`).
- `canBeHurtBy(DamageSource)` on `ItemStack` — which damage types skip armor
  durability; not yet read.
- Thorns: `THORNS` key exists; its effect components and damage formula are
  phase-2-adjacent but not yet extracted.
