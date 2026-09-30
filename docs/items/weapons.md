# Swords and axes (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

26.3 has no `SwordItem`/`AxeItem`/`Tiers` classes anymore. Swords and axes are
plain `Item`s whose stats come from data components set at registration:
`ItemAttributeModifiers` (`ATTACK_DAMAGE`/`ATTACK_SPEED`), a `Weapon`
component (`itemDamagePerAttack`, `disableBlockingForSeconds`), and a `Tool`
component. Melee damage, sweep attacks and the attack-strength gate live in
`Player.attack` (server side); enchantment damage is applied by the
`ServerPlayer` override of `getEnchantedDamage`.

## Data

### Tool materials (`ToolMaterial` record: `durability, speed, attackDamageBonus, enchantmentValue`)

| Material | durability | speed | attackDamageBonus | enchantmentValue |
|---|---|---|---|---|
| WOOD | `59` | `2.0F` | `0.0F` | `15` |
| STONE | `131` | `4.0F` | `1.0F` | `5` |
| COPPER | `190` | `5.0F` | `1.0F` | `13` |
| IRON | `250` | `6.0F` | `2.0F` | `14` |
| GOLD | `32` | `12.0F` | `0.0F` | `22` |
| DIAMOND | `1561` | `8.0F` | `3.0F` | `10` |
| NETHERITE | `2031` | `9.0F` | `4.0F` | `15` |

### Swords: `.sword(material, 3.0F, -2.4F)` for every material

- `ATTACK_DAMAGE` modifier: `3.0F + attackDamageBonus`, `ADD_VALUE`,
  id `Item.BASE_ATTACK_DAMAGE_ID`, slot `MAINHAND`.
- `ATTACK_SPEED` modifier: `-2.4F`, `ADD_VALUE`, id `Item.BASE_ATTACK_SPEED_ID`.
- `Weapon` component: `new Weapon(1)` → `itemDamagePerAttack=1`,
  `disableBlockingForSeconds=0.0F`.
- Resulting damage / speed (base 1.0 damage, 4.0 speed from the player):
  wood 3.0, stone 4.0, copper 4.0, iron 5.0, gold 3.0, diamond 6.0,
  netherite 7.0; attack speed 1.6 for all → cooldown
  `getCurrentItemAttackStrengthDelay() = 1/1.6*20` = **12.5 ticks**.

### Axes: `.axe(material, dmgBaseline, speedBaseline)` → `Weapon(2, 5.0F)`

| Material | baseline | speed mod | damage (= baseline+bonus) | attack speed | cooldown (ticks) |
|---|---|---|---|---|---|
| wood | `6.0F` | `-3.2F` | 6.0 | 0.8 | 25 |
| stone | `7.0F` | `-3.2F` | 8.0 | 0.8 | 25 |
| copper | `7.0F` | `-3.2F` | 8.0 | 0.8 | 25 |
| iron | `6.0F` | `-3.1F` | 8.0 | 0.9 | 22.2 |
| gold | `6.0F` | `-3.0F` | 6.0 | 1.0 | 20 |
| diamond | `5.0F` | `-3.0F` | 8.0 | 1.0 | 20 |
| netherite | `5.0F` | `-3.0F` | 9.0 | 1.0 | 20 |

- `itemDamagePerAttack = 2` (axes lose 2 durability per hit entity).
- `disableBlockingForSeconds = 5.0F` = `Weapon.AXE_DISABLES_BLOCKING_FOR_SECONDS`
  (see shield.md for the disable flow).
- `Tool` component rules differ per tool class (mining only; not PvP-relevant).

### New components (all exist; none set by vanilla weapons)

`DataComponents` (verified by grep on the class):
- `ATTACK_RANGE` (`AttackRange`): `(minReach 0.0F, maxReach 3.0F,
  minCreativeReach 0.0F, maxCreativeReach 5.0F, hitboxMargin 0.3F,
  mobFactor 1.0F)` defaults. `getAttackRangeWith(item)` falls back to
  `AttackRange.defaultFor(entity)` = `(0.0F, ENTITY_INTERACTION_RANGE, 0.0F,
  ENTITY_INTERACTION_RANGE, 0.0F, 1.0F)` when unset. No vanilla item sets it
  (verified: zero matches in `Items.java`).
- `MINIMUM_ATTACK_CHARGE` (`Float`, 0.0–1.0): read in
  `Player.cannotAttackWithItem(itemStack, tolerance)`:
  `requiredStrength = getOrDefault(..., 0.0F)`,
  `optimisticStrength = (attackStrengthTicker + tolerance) /
  getCurrentItemAttackStrengthDelay()`; cannot attack if
  `requiredStrength > 0 && optimisticStrength < requiredStrength`.
  No vanilla item sets it.
- `PIERCING_WEAPON` (`PiercingWeapon`): spear jab attack (phase 5).
  `attack(...)`: damage = `ATTACK_DAMAGE` attribute; per-entity
  `stabAttack`; `swingAndResetAttackStrength`. No sword/axe sets it.
- `ATTACK_ANIMATION` (`SwingAnimation`): `DEFAULT = (WHACK, 6)`.
  `WEAPON` (`Weapon`), `KINETIC_WEAPON` (mace, phase 5) also exist.

### Attack strength

- `getAttackStrengthScale(a) = Mth.clamp((attackStrengthTicker + a) /
  getCurrentItemAttackStrengthDelay(), 0, 1)`, delay = `1/ATTACK_SPEED*20`.
- `baseDamageScaleFactor() = 0.2F + scale² * 0.8F` with `scale =
  getAttackStrengthScale(0.5F)`.

## Logic

### `Player.attack(entity)` (server; the client copy runs it too but `hurtClient` is false)

Verified body, in order:
1. `cannotAttack` (not attackable / skips interaction) → return.
2. `baseDamage = ATTACK_DAMAGE` attribute (auto-spin-attack override aside).
3. `attackingItemStack = getWeaponItem()` (main hand; spin-attack override aside);
   `damageSource = attackingItemStack.getDamageSource(this)`.
4. `attackStrengthScale = getAttackStrengthScale(0.5F)`.
5. `magicBoost = attackStrengthScale *
   (getEnchantedDamage(entity, baseDamage, damageSource) - baseDamage)`.
   On `Player` this returns `dmg` (boost 0); **`ServerPlayer` overrides** it to
   `EnchantmentHelper.modifyDamage(level, getWeaponItem(), entity, damageSource, dmg)`
   — sharpness/smite/BoA apply here, on the *unscaled* base, then multiplied by
   the strength scale. (Verified in `ServerPlayer`.)
6. `baseDamage *= baseDamageScaleFactor()` (the 0.2+0.8·scale² curve).
7. Sprint full-strength hit → knockback sound, `knockbackAttack = true`.
8. `baseDamage += attackingItemStack.getItem().getAttackDamageBonus(…)` (0 for vanilla).
9. Crit: `fullStrengthAttack && canCriticalAttack` (`fallDistance > 0`,
   `!onGround`, `!onClimbable`, `!inWater`, `!mobilityRestricted`,
   `!passenger`, target is `LivingEntity`, `!sprinting`) → `baseDamage *= 1.5F`.
   Note: the crit multiplies the scaled base only, **not** `magicBoost`.
10. `totalDamage = baseDamage + magicBoost`.
11. `sweepAttack = isSweepAttack(fullStrength, critical, knockbackAttack)`:
    all true of: full strength, not crit, not sprint-knockback, `onGround`,
    `horizontalSpeedSq < (getSpeed()*2.5)²`, main-hand item in `ItemTags.SWORDS`.
12. `entity.hurtOrSimulate(damageSource, totalDamage)` → server: `hurtServer`.
13. If hurt: `causeExtraKnockback(entity, getKnockback(entity, damageSource) +
    (knockbackAttack ? 0.5F : 0), …)`; sweep → `doSweepAttack`;
    `setLastHurtMob`; `itemAttackInteraction`; `damageStatsAndHearts`;
    `causeFoodExhaustion(0.1F)` (`FoodConstants.EXHAUSTION_ATTACK`).
    Else: no-damage sound.

`getKnockback` (`LivingEntity`): `ATTACK_KNOCKBACK` attribute (default `0.0`);
server: `EnchantmentHelper.modifyKnockback(level, weaponItem, target, source,
knockback) / 2.0F`. The knockback enchantment's added value is **halved**
here (its JSON adds `1.0 + 1.0·(level-1)`; see enchantments.md).

### Sweep attack (`Player.doSweepAttack`, server)

- Sweep damage: `1.0F + SWEEPING_DAMAGE_RATIO_attr * baseDamage`
  (attribute default `0.0`, range 0–1, syncable; sweeping edge adds a fraction
  of it — see enchantments.md).
- Targets: `LivingEntity`s in `entity.getBoundingBox().inflate(1.0, 0.25, 1.0)`,
  excluding self, the direct target, allies, marker armor stands, and anything
  with `distanceToSqr ≥ 9.0`.
- Per target: `enchantedDamage = getEnchantedDamage(nearby, sweepDamage,
  source) * attackStrengthScale` → `hurtServer` → on hit
  `knockback(0.4F, sin(yaw), -cos(yaw), …)` and
  `EnchantmentHelper.doPostAttackEffects` (fire aspect can ignite sweep victims).
- Sweep sound `PLAYER_ATTACK_SWEEP`; sweep particles at the attacker's position.

### Item durability on attack

`Weapon.itemDamagePerAttack` (1 sword, 2 axe) — applied where? The call is in
`itemAttackInteraction` (seen in `Player.attack`); exact body not yet read —
see open questions. Unbreaking modifies it via
`EnchantmentHelper.processDurabilityChange` inside `ItemStack.hurtAndBreak`.

## Tick placement

- Client tick: click → `handleKeybinds` → `startAttack` sends
  `ServerboundAttackPacket` before `LocalPlayer.tick`/movement (per
  `docs/ARCHITECTURE.md`); the server evaluates with the previous tick's
  rotation.
- Server: attack packet handled → `Player.attack` → `hurtServer` on the victim
  (damage pipeline in shield.md/armor.md) → `causeExtraKnockback` writes the
  victim's velocity; sweep hits are further `hurtServer` calls in the same
  packet handling.
- `attackStrengthTicker` increments each player tick; reset on attack
  (`resetAttackStrengthTicker` also zeroes `itemSwapTicker`).

## Randomness

- No RNG in the damage/strength/sweep math itself. RNG enters via enchantment
  value effects (unbreaking's binomial uses the item's holder random —
  `EnchantmentHelper.processDurabilityChange(serverLevel, …)` takes the
  server level random) and fire aspect (no roll; always ignites).

## Oracle scenarios

1. `40_sword_damage_ladder`: A with each sword material, full cooldown, hits
   on B at 20 health. Expect 3/4/4/5/3/6/7 damage.
2. `41_axe_disable_timing`: covered in shield.md scenario 33; add a
   same-tick case: B raises shield the same tick A's axe packet arrives —
   documents the 5-tick raise vs packet order.
3. `42_sweep`: A with diamond sword surrounded by B and C (C within 3 blocks,
   not targeted). Full-strength hit on B → C takes
   `(1.0 + ratio·6.0) * enchant…` damage with knockback away from A's facing.
   New trace fields: sweep targets, per-target damage.
4. `43_weak_hit`: attack at ~50% strength → damage ×(0.2+0.8·0.25)=×0.4,
   no sweep, no sprint knockback.
5. `44_attack_speed`: wooden axe (0.8 → 25-tick cooldown) vs diamond sword
   (12.5): second click timing and resulting `attackStrengthScale` in trace.

## Open questions

- Exact body of `itemAttackInteraction` (where `itemDamagePerAttack` is
  consumed, and what happens on miss vs hit) — not yet requested.
- `getAttackDamageBonus` overrides: none in vanilla (`Item` returns 0);
  modded hook only.
- Whether `ServerboundAttackPacket` handling checks `cannotAttackWithItem`
  (the `MINIMUM_ATTACK_CHARGE` gate) before `Player.attack` — the method
  exists on `Player`; its call site is in the packet listener, not yet read.
