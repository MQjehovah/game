#include "neon/scene/environment.hpp"

#include "neon/gfx/color.hpp"
#include "neon/math/vec3.hpp"

namespace neon::scene {
namespace {

core::Json MakeNumber(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

core::Json MakeString(const std::string& v) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = v;
    return j;
}

core::Json MakeBool(bool v) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = v;
    return j;
}

core::Json ColorToJson(const gfx::Color& c) {
    core::Json a;
    a.type_ = core::Json::Type::Array;
    a.array_ = {MakeNumber(c.r), MakeNumber(c.g), MakeNumber(c.b), MakeNumber(c.a)};
    return a;
}

// Lenient colour read: missing key keeps the current value; non-number entries
// read as 0 (matches the historical inline parsers so existing scenes load).
void ReadColor(const core::Json& j, const char* key, gfx::Color& out) {
    const core::Json* c = j.Get(key);
    if (!c || !c->IsArray()) return;
    float v[4] = {out.r, out.g, out.b, out.a};
    size_t n = 0;
    for (const core::Json& x : c->Items()) {
        if (n >= 4) break;
        v[n++] = static_cast<float>(x.GetNumber());
    }
    out = {v[0], v[1], v[2], v[3]};
}

void ReadFloat(const core::Json& j, const char* key, float& out) {
    if (const core::Json* n = j.Get(key))
        if (n->IsNumber()) out = static_cast<float>(n->GetNumber());
}

void ReadBool(const core::Json& j, const char* key, bool& out) {
    if (const core::Json* b = j.Get(key))
        if (b->IsBool()) out = b->GetBool();
}

void ReadString(const core::Json& j, const char* key, std::string& out) {
    if (const core::Json* s = j.Get(key))
        if (s->IsString()) out = s->GetString();
}

} // namespace

bool EnvironmentFromJson(const core::Json& json, SceneEnvironment& out, std::string* err) {
    if (!json.IsObject()) {
        if (err) *err = "environment must be a JSON object";
        return false;
    }
    ReadColor(json, "ambientColor", out.ambientColor);
    ReadFloat(json, "ambientStrength", out.ambientStrength);
    ReadString(json, "skyTexture", out.skyTexture);
    ReadBool(json, "useAtmosphere", out.useAtmosphere);
    ReadBool(json, "skybox", out.skybox);
    ReadColor(json, "skyTop", out.skyTop);
    ReadColor(json, "skyHorizon", out.skyHorizon);
    ReadColor(json, "fogColor", out.fogColor);
    ReadFloat(json, "fogNear", out.fogNear);
    ReadFloat(json, "fogFar", out.fogFar);
    ReadFloat(json, "exposure", out.exposure);
    // A WorldEnvironment references a reusable `environments/*.env.json`; the
    // runtime resolves it and folds the resource's values into this component.
    ReadString(json, "resource", out.resource);
    return true;
}

void EnvironmentToJson(const SceneEnvironment& env, core::Json& obj) {
    obj.type_ = core::Json::Type::Object;
    if (!env.resource.empty()) obj.object_["resource"] = MakeString(env.resource);
    obj.object_["ambientColor"] = ColorToJson(env.ambientColor);
    obj.object_["ambientStrength"] = MakeNumber(env.ambientStrength);
    if (!env.skyTexture.empty()) obj.object_["skyTexture"] = MakeString(env.skyTexture);
    if (env.useAtmosphere) obj.object_["useAtmosphere"] = MakeBool(true);
    if (env.skybox) obj.object_["skybox"] = MakeBool(true);
    obj.object_["skyTop"] = ColorToJson(env.skyTop);
    obj.object_["skyHorizon"] = ColorToJson(env.skyHorizon);
    obj.object_["fogColor"] = ColorToJson(env.fogColor);
    obj.object_["fogNear"] = MakeNumber(env.fogNear);
    obj.object_["fogFar"] = MakeNumber(env.fogFar);
    if (env.exposure >= 0.0f) obj.object_["exposure"] = MakeNumber(env.exposure);
}

} // namespace neon::scene
