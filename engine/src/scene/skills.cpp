#include "neon/scene/skills.hpp"

#include "neon/scene/status.hpp"

namespace neon::scene {

namespace {

bool IsKnownKind(const std::string& kind) {
    return kind == "projectile" || kind == "melee" || kind == "box";
}

} // namespace

bool SkillTable::Load(const std::string& json, std::string* err) {
    std::string parseErr;
    core::Json root = core::Json::Parse(json, &parseErr);
    if (root.IsNull() && !parseErr.empty()) {
        if (err) *err = "skill table: " + parseErr;
        return false;
    }
    return Load(root, err);
}

bool SkillTable::Load(const core::Json& root, std::string* err) {
    defs_.clear();
    index_.clear();

    const core::Json* skills = root.Get("skills");
    if (!skills || !skills->IsObject()) {
        if (err) *err = "skill table: missing \"skills\" object";
        return false;
    }

    for (const auto& kv : skills->Members()) {
        const std::string& name = kv.first;
        const core::Json& s = kv.second;
        if (!s.IsObject()) {
            if (err) *err = "skill table: skill '" + name + "' is not an object";
            return false;
        }

        SkillDef def;
        def.name = name;
        def.kind = s.Get("kind") ? s.Get("kind")->GetString("projectile") : "projectile";
        if (!IsKnownKind(def.kind)) {
            if (err) *err = "skill table: skill '" + name + "' has unknown kind '" + def.kind + "'";
            return false;
        }
        def.damage = static_cast<float>(s.Get("damage") ? s.Get("damage")->GetNumber(0.0) : 0.0);
        def.cooldown =
            static_cast<float>(s.Get("cooldown") ? s.Get("cooldown")->GetNumber(0.0) : 0.0);
        def.manaCost =
            static_cast<float>(s.Get("manaCost") ? s.Get("manaCost")->GetNumber(0.0) : 0.0);
        def.speed = static_cast<float>(s.Get("speed") ? s.Get("speed")->GetNumber(12.0) : 12.0);
        def.life = static_cast<float>(s.Get("life") ? s.Get("life")->GetNumber(2.0) : 2.0);
        def.range = static_cast<float>(s.Get("range") ? s.Get("range")->GetNumber(0.0) : 0.0);
        def.meleeRange =
            static_cast<float>(s.Get("meleeRange") ? s.Get("meleeRange")->GetNumber(2.0) : 2.0);
        def.arcDeg =
            static_cast<float>(s.Get("arcDeg") ? s.Get("arcDeg")->GetNumber(90.0) : 90.0);
        def.boxHalfX =
            static_cast<float>(s.Get("boxHalfX") ? s.Get("boxHalfX")->GetNumber(1.0) : 1.0);
        def.boxHalfY =
            static_cast<float>(s.Get("boxHalfY") ? s.Get("boxHalfY")->GetNumber(1.0) : 1.0);
        def.boxHalfZ =
            static_cast<float>(s.Get("boxHalfZ") ? s.Get("boxHalfZ")->GetNumber(1.0) : 1.0);

        if (const core::Json* sts = s.Get("statuses")) {
            if (!sts->IsArray()) {
                if (err) *err = "skill table: skill '" + name + "' statuses is not an array";
                return false;
            }
            for (size_t i = 0; i < sts->Size(); ++i) {
                const core::Json* st = sts->At(i);
                if (!st || !st->IsObject()) {
                    if (err)
                        *err = "skill table: skill '" + name + "' status #" +
                               std::to_string(i) + " is not an object";
                    return false;
                }
                SkillStatus ss;
                ss.name = st->Get("name") ? st->Get("name")->GetString() : "";
                if (StatusIdByName(ss.name) == 0) {
                    if (err)
                        *err = "skill table: skill '" + name + "' references unknown status '" +
                               ss.name + "'";
                    return false;
                }
                ss.duration =
                    static_cast<float>(st->Get("duration") ? st->Get("duration")->GetNumber(0.0)
                                                          : 0.0);
                ss.magnitude =
                    static_cast<float>(st->Get("magnitude") ? st->Get("magnitude")->GetNumber(0.0)
                                                            : 0.0);
                def.statuses.push_back(std::move(ss));
            }
        }

        index_[def.name] = defs_.size();
        defs_.push_back(std::move(def));
    }
    return true;
}

const SkillDef* SkillTable::Find(const std::string& name) const {
    const auto it = index_.find(name);
    return it == index_.end() ? nullptr : &defs_[it->second];
}

} // namespace neon::scene
