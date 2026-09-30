# Shield (vanilla 26.3)

All facts verified in the decompiled 26.3 client jar unless marked otherwise.

## Summary

Raising a shield starts the "using item" state (right-click, server-authoritative).
After a 5-tick raise delay the shield blocks: melee and projectile damage from
within a 90-degree half-angle (180 degrees total) frontal arc is reduced to
zero, the shield takes durability damage instead of the player, and the
attacker is told about the block. Axes
(and anything with `disable_blocking_for_seconds` on its `Weapon` component)
put the shield on a 100-tick cooldown and force it down.

## Data

| Constant | Value (source literal) | Source |
|---|---|---|
| Shield durability | `336` | `Items.SHIELD` registration (`Item.Properties.durability(336)`) |
| `block_delay_seconds` | `0.25F` | `Items.SHIELD` → `BlocksAttacks(0.25F, …)` |
| Raise delay in ticks | `Math.round(0.25F * 20.0F)` = **5** | `BlocksAttacks.blockDelayTicks()` |
| `disable_cooldown_scale` | `1.0F` | `Items.SHIELD` |
| Blocking arc | `90.0F` degrees half-angle | `BlocksAttacks.DamageReduction(90.0F, Optional.empty(), 0.0F, 1.0F)` (default list) |
| Damage reduction formula | `Mth.clamp(0.0F + 1.0F * dealtDamage, 0.0F, dealtDamage)` | `BlocksAttacks.DamageReduction.resolve` |
| Shield item damage on block | `dealtDamage < 3.0F ? 0 : Mth.floor(1.0F + 1.0F * dealtDamage)` | `BlocksAttacks.ItemDamageFunction(3.0F, 1.0F, 1.0F)` (`DEFAULT` is `(1.0F, 0.0F, 1.0F)`; the shield overrides it) |
| Axe `disable_blocking_for_seconds` | `5.0F` | `Weapon.AXE_DISABLES_BLOCKING_FOR_SECONDS`; set by `Item.Properties.axe` → `ToolMaterial.applyToolProperties(…, 5.0F)` |
| Disable cooldown ticks | `Math.round(5.0F * 1.0F * 20.0F)` = **100** | `BlocksAttacks.disableBlockingForTicks` |
| Shield use duration | `72000` ticks | `Item.getUseDuration` (any stack with `BLOCKS_ATTACKS`) |
| Blocked-by tag (bypasses shield) | `DamageTypeTags.BYPASSES_SHIELD` | `Items.SHIELD` `bypassedBy` |
| Block sound | `SHIELD_BLOCK`, vol `1.0F`, pitch `0.8F + rand*0.4F` | `BlocksAttacks.onBlocked` |
| Disable sound | `SHIELD_BREAK`, vol `0.8F`, pitch `0.8F + rand*0.4F` | `BlocksAttacks.disable` |
| `UseEffects` default (movement while raised) | `canSprint=false`, `speedMultiplier=0.2F` | `UseEffects.DEFAULT`; the shield sets no `USE_EFFECTS` component |
| `isBlocking()` | `getItemBlockingWith() != null` | `LivingEntity.isBlocking` |

The generic `BlocksAttacks` defaults (for any other item that gains the
component): `block_delay_seconds` `0.0F`, `disable_cooldown_scale` `1.0F`,
one `DamageReduction(90.0F, empty, 0.0F, 1.0F)`, `ItemDamageFunction.DEFAULT`
`(1.0F, 0.0F, 1.0F)`.

## Logic

### Raising (client + server)

1. Client right-click: `Item.use` sees `BLOCKS_ATTACKS` → `player.startUsingItem(hand)`,
   returns `CONSUME`. (`Item.use`, verified.)
2. `LivingEntity.startUsingItem` (both sides): `useItem = stack`,
   `useItemRemaining = getUseDuration()` (72000). Server side only: equipment
   sync to tracking players, entity-data flags, `ITEM_INTERACT_START` vibration.
3. Client prediction: `LocalPlayer.isUsingItem()` reads the local
   `startedUsingItem` flag. While using, input is scaled by
   `itemUseSpeedMultiplier()` = `0.2F` (`LocalPlayer`, around the
   `input.scale(0.98F)` step), and `isSlowDueToUsingItem()` zeroes
   `sprintTriggerTime` and makes `canStartSprinting()` return false.
4. The client sends `ServerboundUseItemPacket`; the server's
   `ServerGamePacketListenerImpl.handleUseItem` calls
   `gameMode.useItem(...)` which runs the same `Item.use` → `startUsingItem`
   on the server copy.
5. The shield only *blocks* once `getItemBlockingWith()` is non-null:
   `isUsingItem()` and `elapsedTicks = getUseDuration() - useItemRemaining
   >= blockDelayTicks()` (5 ticks). Before that the item is "being raised".

### Blocking a hit (server, inside `LivingEntity.hurtServer`)

`hurtServer` order (verified, `LivingEntity.hurtServer`): invulnerability →
dead → fire-resistance → `damage = max(damage, 0)` → **`applyItemBlocking`**
→ freezing ×5 → helmet damage ×0.75 → NaN/Inf guard → cooldown gate →
`actuallyHurt` → `resolveBlockedDamage` is *not* in `CombatRules` (the brief's
name); it lives on the component: `BlocksAttacks.resolveBlockedDamage`.

`applyItemBlocking(level, source, damage)`:
1. `damage <= 0.0F` → 0. No blocking item → 0.
2. `bypassedBy` tag contains the damage type → 0. Piercing arrow
   (`AbstractArrow` with `getPierceLevel() > 0`) → 0.
3. Angle: `sourcePosition = source.getSourcePosition()`; if null, `angle = PI`;
   else the xz-normalised vector from defender to source position, dotted with
   `calculateViewVector(0.0F, getYHeadRot())`, `angle = Math.acos(dot)`.
   Note: the test is against the defender's **head yaw**, in double precision.
4. `damageBlocked = resolveBlockedDamage(source, damage, angle)`:
   per `DamageReduction`: `angle > (PI/180) * 90.0F` → 0; else
   `clamp(0.0F + 1.0F * damage, 0, damage)`. Summed and clamped to
   `[0, dealtDamage]`.
5. `hurtBlockingItem(...)`: players only, server only: `ITEM_USED` stat, then
   `itemDamage = damageBlocked < 3.0F ? 0 : floor(1.0F + damageBlocked)`,
   `hurtAndBreak(itemDamage, …)` on the shield (unbreaking applies inside).
6. If `damageBlocked > 0`, not a projectile, and the direct entity is a
   `LivingEntity`: `blockUsingItem(level, attacker, source, damage,
   damageBlocked >= damage)` → `attacker.blockedByItem(this, …)`:
   if **not** fully blocked, the *defender* is knocked back:
   `defender.knockback(0.5, defender.x - attacker.x, defender.z - attacker.z,
   source, damage)` — i.e. the defender is shoved 0.5 away from the attacker
   even on a partial block.
7. Back in `hurtServer`: `damage -= damageBlocked`; `blocked = damageBlocked > 0`.
   `fullyBlocked = blocked && damage <= 0.0F`. Knockback:
   `if (!source.is(NO_KNOCKBACK) && !fullyBlocked) dealDefaultKnockback(…)`
   — a fully blocked hit deals **no** knockback; a partially blocked hit still
   gets the normal 0.4F knockback plus the 0.5 shove from step 6.
   If `tookFullDamage`: blocked → `blocksAttacks.onBlocked` (sound, server,
   `level.getRandom()`), else `broadcastDamageEvent`. `markHurt()` unless
   `NO_IMPACT` or (blocked and no remaining damage). `success = !blocked ||
   damage > 0`.

### Axe disabling (server)

`Player.blockUsingItem` override, after `super` (the knockback above):
`secondsToDisableBlocking = attacker.getSecondsToDisableBlocking()` =
`weapon.disableBlockingForSeconds()` if the attacker's weapon item has a
`Weapon` component **and** `weaponItem == getActiveItem()` (reference equality;
for a plain held axe `getActiveItem()` returns the main-hand item, so it
applies). If `> 0.0F`: `blocksAttacks.disable(level, defender, 5.0F, shield)`:
`cooldownTicks = Math.round(5.0F * 1.0F * 20.0F)` = 100;
`player.getCooldowns().addCooldown(shieldStack, 100)`; `stopUsingItem()`;
disable sound. The cooldown blocks re-raising (see open questions for the
exact gate).

Swords have `Weapon(1, 0.0F)` — no disable.

## Tick placement

- Client tick: right-click is processed in `handleKeybinds` (same phase as
  attack clicks per `docs/ARCHITECTURE.md`); `LocalPlayer.startUsingItem`
  sets the predicted state immediately, so the 0.2× slowdown applies from that
  tick. `ServerboundUseItemPacket` goes out with the tick's packets.
- Server: the use packet is handled before the server tick; `startUsingItem`
  on the server copy starts `useItemRemaining` at 72000. Each server tick
  `updatingUsingItem` decrements it; the shield counts as blocking from the
  6th tick of holding (5 elapsed).
- An attack arriving the same tick the shield finishes raising: the server
  processes packets in send order, then ticks. Whether the 5th tick has
  elapsed is evaluated in `getItemBlockingWith` at damage time.
- `hurtServer` runs inside the server tick when the attack packet is handled.
  All of `applyItemBlocking`, durability damage, sounds, disable and the
  knockback decisions happen there, before `actuallyHurt`.

## Randomness

- Block sound pitch: `0.8F + level.getRandom().nextFloat() * 0.4F` — the
  **server level's** `RandomSource` (not reproducible per-entity; count as a
  non-parity draw if the sim ever models audio, otherwise ignorable).
- Disable sound pitch: same source.
- No RNG in the angle test, damage math, or cooldown.

## Oracle scenarios

1. `40_shield_raise_timing`: B holds shield from tick 0; A attacks with a
   diamond sword on ticks 3, 5 and 6. Expect: full 7.0 damage on ticks 3–5
   (shield still raising), zero damage from tick 6 on (5 ticks elapsed).
   Trace: `blocked`, `damageBlocked`, shield durability, B velocity.
2. `41_shield_arc`: B raises shield facing A (yaw 180), A attacks; then B
   yaw 90 (side-on), A attacks. Expect full block then full damage. Trace the
   computed angle.
3. `42_shield_partial_knockback`: not testable with a vanilla shield — its
   `DamageReduction` is all-or-nothing (`base 0.0F, factor 1.0F`), so a
   partial block never occurs. The defender-shove path
   (`blockedByItem` → 0.5 knockback when not fully blocked) only matters for
   custom `damage_reductions`.
4. `43_axe_disable`: B shield raised; A hits with diamond axe. Expect: 0
   damage, shield on 100-tick cooldown, B stops using item; A attacks again
   20 ticks later → full damage (cooldown still active).
5. `44_shield_vs_sword`: diamond sword vs raised shield: 0 damage, no
   cooldown, shield durability −8 (`floor(1.0F + 7.0F)` for the blocked 7.0 damage).
6. `45_shield_projectile_arc`: arrow from the front vs raised shield
   (phase 3, placeholder): blocked, no `blockUsingItem` knockback (projectiles
   skip it), `dealDefaultKnockback` uses the arrow's
   `calculateHorizontalHurtKnockbackDirection` unless fully blocked.

New trace fields: `blocked` (bool), `damageBlocked` (float), shield
`useItemRemaining` / `isBlocking`, per-item cooldown map.

## Open questions

- Where exactly the item-use cooldown gates re-raising on the server
  (`ItemCooldowns.isOnCooldown` check site for `startUsingItem`) — unverified;
  needed for the tick the shield can be raised again after disable.
- Client damage prediction for blocked hits (does the victim's client show
  the hurt tilt before the server reply?) — the tilt comes from
  `dealDefaultKnockback`→`indicateDamage` server-side; client handling of
  `broadcastDamageEvent` vs `onBlocked` sound is cosmetic.
- Whether `getSourcePosition()` for a melee attack is the attacker's eye or
  feet — affects the arc test only through the xz projection, so likely
  immaterial; verify when porting (`DamageSource.getSourcePosition`
  not yet read).
