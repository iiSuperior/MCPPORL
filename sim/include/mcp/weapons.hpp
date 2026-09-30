// Melee weapons, from vanilla 26.3 (docs/items/weapons.md).
//
// 26.3 has no SwordItem/AxeItem classes: a sword or axe is a plain item whose
// ItemAttributeModifiers add to the player's ATTACK_DAMAGE (base 1.0) and
// ATTACK_SPEED (base 4.0). Items.java registers them through
// Item.Properties.sword(material, 3.0F, -2.4F) and .axe(material, baseline,
// speed); the damage modifier is (baseline + material.attackDamageBonus) in
// float, stored as a double amount. Sweeps have no other target in a duel.
// An axe's Weapon component disables a raised shield for 5 seconds.
//
// The shield (Items.SHIELD) sits in the same table: no attribute modifiers,
// no Weapon component, a BlocksAttacks component (see ShieldStats).
#pragma once

#include <cstdint>
#include <cstring>

#include "mcp/jmath.hpp"

namespace mcp {

enum class Weapon : uint8_t {
    Hand = 0,
    WoodenSword, StoneSword, CopperSword, IronSword, GoldenSword, DiamondSword, NetheriteSword,
    WoodenAxe, StoneAxe, CopperAxe, IronAxe, GoldenAxe, DiamondAxe, NetheriteAxe,
    Shield,
    Count
};

struct WeaponStats {
    const char* name;       // the Items field, lower case (oracle scenarios' item=)
    float damageModifier;   // ATTACK_DAMAGE, ADD_VALUE
    float speedModifier;    // ATTACK_SPEED, ADD_VALUE
    int32_t durability;     // ToolMaterial.durability
    int32_t damagePerHit;   // Weapon.itemDamagePerAttack (0: no Weapon component)
    float disableSeconds;   // Weapon.disableBlockingForSeconds
    bool blocksAttacks;     // has a BlocksAttacks component (usable to block)
};

// Items.SHIELD's BlocksAttacks: 0.25 s raise delay, all-or-nothing reduction
// within 90 degrees, item damage floor(1 + blocked) from 3 blocked, cooldown
// scale 1. The default UseEffects applies while raised: input x0.2, no sprint.
struct ShieldStats {
    static constexpr float kBlockDelaySeconds = 0.25F;
    static constexpr float kDisableCooldownScale = 1.0F;
    static constexpr float kHorizontalBlockingAngle = 90.0F;
    static constexpr float kItemDamageThreshold = 3.0F, kItemDamageBase = 1.0F, kItemDamageFactor = 1.0F;
    static constexpr float kUseSpeedMultiplier = 0.2F;
    static constexpr int32_t kUseDuration = 72000;  // Item.getUseDuration for BLOCKS_ATTACKS
};

namespace weapons {

// ToolMaterial: attackDamageBonus and durability.
struct Material {
    float bonus;
    int32_t durability;
};
constexpr Material kWood{0.0F, 59}, kStone{1.0F, 131}, kCopper{1.0F, 190}, kIron{2.0F, 250}, kGold{0.0F, 32},
    kDiamond{3.0F, 1561}, kNetherite{4.0F, 2031};

constexpr WeaponStats sword(const char* n, Material m) { return {n, 3.0F + m.bonus, -2.4F, m.durability, 1, 0.0F, false}; }
constexpr WeaponStats axe(const char* n, Material m, float baseline, float speed) {
    return {n, baseline + m.bonus, speed, m.durability, 2, 5.0F, false};  // Weapon.AXE_DISABLES_BLOCKING_FOR_SECONDS
}

constexpr WeaponStats kTable[] = {
    {"", 0.0F, 0.0F, 0, 0, 0.0F, false},
    sword("wooden_sword", kWood), sword("stone_sword", kStone), sword("copper_sword", kCopper),
    sword("iron_sword", kIron), sword("golden_sword", kGold), sword("diamond_sword", kDiamond),
    sword("netherite_sword", kNetherite),
    axe("wooden_axe", kWood, 6.0F, -3.2F), axe("stone_axe", kStone, 7.0F, -3.2F), axe("copper_axe", kCopper, 7.0F, -3.2F),
    axe("iron_axe", kIron, 6.0F, -3.1F), axe("golden_axe", kGold, 6.0F, -3.0F), axe("diamond_axe", kDiamond, 5.0F, -3.0F),
    axe("netherite_axe", kNetherite, 5.0F, -3.0F),
    {"shield", 0.0F, 0.0F, 336, 0, 0.0F, true},
};
static_assert(sizeof(kTable) / sizeof(kTable[0]) == static_cast<size_t>(Weapon::Count), "one row per weapon");

}  // namespace weapons

MCP_HD inline const WeaponStats& stats(Weapon w) { return weapons::kTable[static_cast<int32_t>(w)]; }

// AttributeInstance.getValue: base + the ADD_VALUE modifier (a double amount).
MCP_HD inline double attackDamageAttribute(Weapon w) { return 1.0 + static_cast<double>(stats(w).damageModifier); }
MCP_HD inline double attackSpeedAttribute(Weapon w) { return 4.0 + static_cast<double>(stats(w).speedModifier); }

// Player.getCurrentItemAttackStrengthDelay
MCP_HD inline float attackStrengthDelay(Weapon w) { return static_cast<float>(1.0 / attackSpeedAttribute(w) * 20.0); }

// Host-side lookup by the oracle's item name ("" for a bare hand).
inline bool weaponByName(const char* name, Weapon& out) {
    for (int32_t i = 0; i < static_cast<int32_t>(Weapon::Count); ++i) {
        if (std::strcmp(weapons::kTable[i].name, name) == 0) {
            out = static_cast<Weapon>(i);
            return true;
        }
    }
    return false;
}

}  // namespace mcp
