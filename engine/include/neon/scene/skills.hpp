#pragma once

// Data-driven skill/ability table for the combat core.
//
// Skills are plain JSON, loaded into a SkillTable and consumed by
// GameRuntime::CastSkill + the CastSkill/SkillCooldown bindings. A skill
// defines its kind (projectile / melee arc / attack box), damage, cooldown,
// optional mana cost (subtracted from the GameVar "mana" when present), and
// status effects applied to every hit target.

#include <map>
#include <string>
#include <vector>

#include "neon/core/json.hpp"

namespace neon::scene {

// A status applied to a hit target by a skill (name resolved via the built-in
// status table: burning / poison / regen).
struct SkillStatus {
    std::string name;
    float duration = 0.0f;
    float magnitude = 0.0f;
};

// One data-driven skill definition.
struct SkillDef {
    std::string name;
    std::string kind;      // "projectile" | "melee" | "box"
    float damage = 0.0f;
    float cooldown = 0.0f; // seconds between casts (0 = none)
    float manaCost = 0.0f; // subtracted from GameVar "mana" when > 0
    // projectile fields
    float speed = 12.0f;
    float life = 2.0f;
    float range = 0.0f;    // max travel (0 = life-bounded only)
    // melee arc fields
    float meleeRange = 2.0f;
    float arcDeg = 90.0f;
    // attack-box (OBB) fields
    float boxHalfX = 1.0f;
    float boxHalfY = 1.0f;
    float boxHalfZ = 1.0f;
    // status effects applied to every hit target
    std::vector<SkillStatus> statuses;
};

// Parses a skill table JSON:
//   { "skills": {
//       "fireball": { "kind": "projectile", "damage": 18, "cooldown": 0.5,
//                     "speed": 14, "range": 30, "life": 2, "manaCost": 8,
//                     "statuses": [{"name":"burning","duration":3,"magnitude":2}] },
//       "cleave":   { "kind": "melee", "damage": 12, "meleeRange": 2.5,
//                     "arcDeg": 100, "cooldown": 0.8 }
//   } }
class SkillTable {
public:
    // Parses `json` text. Returns false (with a message in *err) on malformed
    // JSON, a missing "skills" object, or a skill with an unknown kind or an
    // unknown status name.
    bool Load(const std::string& json, std::string* err);
    bool Load(const core::Json& root, std::string* err);

    const SkillDef* Find(const std::string& name) const;
    size_t Size() const { return defs_.size(); }

private:
    std::vector<SkillDef> defs_;
    std::map<std::string, size_t> index_;
};

} // namespace neon::scene
