# Research brief: combat items and mechanics (vanilla 26.3)

A hand-off for a research agent. The output is reference material the
simulator port will be built from later; no simulator or oracle code is
written under this brief.

## Why this exists

MCPPORL reimplements Minecraft PvP bit for bit ("parity in known domains",
see `docs/ARCHITECTURE.md`). Melee with bare hands is done. Next come the
items of kit PvP. Porting each one needs, before any code: the exact vanilla
logic, the exact constants (with their float/double types), and the order in
which things happen within a tick and across the client/server split.

## Ground rules

1. **Source of truth is the decompiled 26.3 jar, nothing else.** Not the
   wiki, not memory, not older versions. Wiki values are fine as a pointer to
   what to look up, never as the answer.
2. **How to read source:** this repo's `vanilla-source` GitHub workflow
   decompiles requested members of the real 26.3 client jar and prints them in
   the job log. Edit `oracle/source-requests.txt` (format in its header:
   `<fully.qualified.Class> <member> ...`, plus `:signatures`, `:fields`,
   `:all`, `:grep=REGEX`), push, read the log. Work on your **own branch**
   (e.g. `research/items`), never on the simulator branch. Stage files
   explicitly (no `git commit -a`).
3. **Do not commit decompiled source.** The jar's code is Mojang's (EULA).
   Quote short excerpts (a few lines) where the exact expression matters;
   otherwise paraphrase with the class and method name. Never commit the jar.
4. **Exact constants.** Write literals as the source has them: `0.4F` is not
   `0.4` (the float widens to 0.4000000059604645 in double math), `20` ticks
   is not "1 second". Note float vs double for every quantity that feeds
   physics or damage.
5. **Order of operations is part of the answer.** For every mechanic, say
   where it runs (client or server), in which method, at which point of the
   tick, and what it reads and writes. Packet flow matters: what the client
   sends, what the server does with it, what it echoes back.
6. **Mark confidence.** Each fact is either *verified in source* (cite
   `Class.method`) or *unverified* (say why). Open questions go in a list at
   the end of each file rather than being guessed.

## Scope, in priority order

Phase 2 (first):

- **Shield**: raising (use duration before it blocks), blocking arc
  (`BlocksAttacks` component, `resolveBlockedDamage`, angle test), what a
  block does to knockback, damage and the attacker; axe disabling
  (`disableBlockingForSeconds`, item cooldown length), sounds/animations only
  where they carry state.
- **Swords and axes**: attack damage, attack speed, attack range components
  per material (wood to netherite), sweep attacks (conditions, targets,
  damage), axe vs shield. Also the new `ATTACK_RANGE`, `MINIMUM_ATTACK_CHARGE`,
  `PIERCING_WEAPON`, `ATTACK_ANIMATION` components if any vanilla weapon sets them.
- **Armor**: `CombatRules.getDamageAfterAbsorb` and magic absorb, armor and
  toughness per piece and material, durability loss on hit (`hurtArmor`),
  knockback resistance (netherite).
- **Food, golden apples, enchanted golden apples**: use duration, effects and
  their durations/amplifiers, absorption, `FoodData` (exhaustion, saturation,
  regeneration timing), sprinting's food requirement, eating while moving
  (speed multiplier while using an item).
- **Totem of Undying**: `checkTotemDeathProtection` path, effects granted,
  which hand, ordering against death in the killing tick.
- **Status effects used in kits**: strength, weakness, resistance,
  regeneration, absorption, speed, slowness, jump boost, fire resistance,
  poison, instant health/damage: the exact attribute modifiers or tick
  logic, and how amplifiers scale.
- **Enchantments used in kits**: sharpness, protection family, knockback,
  fire aspect, unbreaking/mending (durability only), and the new data-driven
  enchantment effects that feed `EnchantmentHelper.modifyKnockback`,
  `getDamageProtection`, `modifyDamage`.

Phase 3:

- **Bows and crossbows**: draw time to power curve, arrow initial velocity
  and inaccuracy (and which RNG it uses), arrow physics per tick (gravity,
  drag, water), hit detection (`ProjectileUtil`, the entity margin), damage
  from velocity, crits, punch/power/multishot/piercing/quick charge,
  knockback from arrows (`calculateHorizontalHurtKnockbackDirection`).
- **Fishing rod**: bobber physics, hooking a player, the pull, knockback on hit.
- **Ender pearl**: throw velocity, flight, teleport on impact, fall damage on
  landing, item cooldown.
- **Splash potions** (and lingering if kits use them): throw, flight, area,
  distance falloff of effect strength.
- **Cobwebs**: `stuckSpeedMultiplier` values and how they interact with
  movement (the simulator already calls `Entity.move`; say exactly what
  changes).
- **Water and lava buckets**: placement rules, fluid flow timing, swimming
  and lava physics, fire.

Phase 5 (later, lower priority): elytra and fireworks, mace (smash attack,
fall-distance scaling, density/breach/wind burst), spear (piercing and lunge
mechanics), TNT and explosions (exposure raycasts, knockback, damage).

## Deliverables

For each item family, one Markdown file in `docs/items/` (e.g.
`docs/items/shield.md`) with:

1. **Summary**: what it does in PvP terms, two or three lines.
2. **Data**: every constant, with type and source (`Class.FIELD` or
   `Class.method`), in a table.
3. **Logic**: step-by-step, in source order, split by client and server, with
   the packets exchanged. Short source excerpts only where the exact
   expression matters.
4. **Tick placement**: where each step sits relative to what the simulator
   already models (see "One client tick" and "What the server copy actually
   does" in `docs/ARCHITECTURE.md`, and `sim/include/mcp/duel.hpp`).
5. **Randomness**: every RNG draw, which random source it uses, and whether
   it can be reproduced (per-entity `RandomSource`s generally cannot).
6. **Oracle scenarios**: 3 to 6 proposed two-player scenarios in the format
   of `oracle/combat/README.md` that would pin the mechanic down (what to do,
   what to look at in the trace). Note any new trace fields they need.
7. **Open questions**: anything unverified.

Plus one machine-readable table, `docs/items/items.json`: per item id, its
components and attribute modifiers with exact values (numbers as strings in
the source's literal form, e.g. `"0.4F"`), and the source location.

## Useful context in this repo

- `docs/ARCHITECTURE.md`: the client/server split, what has been verified,
  the fairness policy (inputs must be human-plausible).
- `oracle/combat/README.md`: scenario format and existing golden results.
- `sim/include/mcp/duel.hpp`: the ported melee, as a model of the level of
  detail needed (every step there traces back to a vanilla method).
- `tools/extract_methods.py`: what the source workflow can print.

## Done means

Every Phase 2 family has its file, every constant in it cites a 26.3 source
location, and the open-questions lists are honest. Phase 3 and 5 files may
be partial; say what is missing.
