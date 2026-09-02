#include "neon/mcp/mcp_server.hpp"

#include <string>

#include "neon/scene/component_schema.hpp"

namespace neon::mcp {
namespace {\
core::Json Null() { return {}; }

core::Json Num(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}
core::Json Str(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}
core::Json Bool(bool b) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = b;
    return j;
}
const core::Json* Member(const core::Json& j, const char* k) { return j.Get(k); }
const core::Json* Path(const core::Json& j, const char* child) {
    return (j.IsObject() && j.Get(child)) ? j.Get(child) : nullptr;
}

core::Json Ok(core::Json id, core::Json result) {
    core::Json r;
    r.type_ = core::Json::Type::Object;
    r.object_["jsonrpc"] = Str("2.0");
    r.object_["id"] = std::move(id);
    r.object_["result"] = std::move(result);
    return r;
}

core::Json Err(core::Json id, const std::string& msg, int code = -32600) {
    core::Json r;
    r.type_ = core::Json::Type::Object;
    r.object_["jsonrpc"] = Str("2.0");
    r.object_["id"] = std::move(id);
    core::Json err;
    err.type_ = core::Json::Type::Object;
    err.object_["code"] = Num(static_cast<double>(code));
    err.object_["message"] = Str(msg);
    r.object_["error"] = std::move(err);
    return r;
}

int FindEntity(const scene::SceneFile& scene, const core::Json& args) {
    const core::Json* id = Member(args, "entity");
    if (!id || !id->IsNumber()) return -1;
    const int want = id->GetInt();
    for (size_t i = 0; i < scene.entities.size(); ++i)
        if (scene.entities[i].id == want) return static_cast<int>(i);
    return -1;
}

// Best-effort reflection validation of a component value. A component without a
// registered schema accepts any JSON (data components); known scalar/enum/etc.
// fields are type-checked so the AI assistant gets a clear error early.
std::string Validate(const core::Json& value, const std::string& compName) {
    if (!value.IsObject()) return "component value must be a JSON object";
    const scene::ComponentSchema* schema = scene::FindComponentSchema(compName);
    if (!schema || schema->fields.empty()) return ""; // data components: accept any JSON
    for (const auto& [key, v] : value.object_) {
        const scene::FieldSchema* fs = nullptr;
        for (const auto& f : schema->fields)
            if (f.key == key) { fs = &f; break; }
        if (!fs) continue; // unknown field kept for forward-compatibility
        const bool ok =
            (fs->type == scene::FieldType::Number || fs->type == scene::FieldType::Int ||
             fs->type == scene::FieldType::Vec3 || fs->type == scene::FieldType::Vec4 ||
             fs->type == scene::FieldType::Color)
                ? (v.IsNumber() || v.IsArray())
                : fs->type == scene::FieldType::Bool
                      ? v.IsBool()
                      : fs->type == scene::FieldType::String ||
                                fs->type == scene::FieldType::Resource ||
                                fs->type == scene::FieldType::Enum
                            ? v.IsString()
                            : true; // Json/Array/Struct: accept anything
        if (!ok) return "field '" + key + "' has the wrong type for component '" + compName + "'";
    }
    return "";
}

// Runs a tool for tools/call. Returns the raw result Json (an Err object if the
// tool itself failed), or the direct result value on success.
core::Json RunTool(scene::SceneFile& scene, const core::Json& args, const std::string& name,
                   bool* changed) {
    if (name == "list_entities") {
        core::Json entities;
        entities.type_ = core::Json::Type::Array;
        for (const auto& e : scene.entities) {
            core::Json ent;
            ent.type_ = core::Json::Type::Object;
            ent.object_["id"] = Num(static_cast<double>(e.id));
            ent.object_["name"] = Str(e.name.empty() ? std::to_string(e.id) : e.name);
            core::Json comps;
            comps.type_ = core::Json::Type::Array;
            for (const auto& c : e.components) comps.array_.push_back(Str(c.name));
            ent.object_["components"] = std::move(comps);
            entities.array_.push_back(std::move(ent));
        }
        return entities;
    }
    if (name == "get_component") {
        const int idx = FindEntity(scene, args);
        if (idx < 0) return Err(Null(), "entity not found", -32602);
        const core::Json* cname = Member(args, "name");
        if (!cname || !cname->IsString()) return Err(Null(), "missing 'name'", -32602);
        const std::string comp = cname->GetString();
        for (const auto& c : scene.entities[static_cast<size_t>(idx)].components)
            if (c.name == comp) {
                core::Json out;
                out.type_ = core::Json::Type::Object;
                out.object_["component"] = c.data;
                out.object_["present"] = Bool(true);
                return out;
            }
        core::Json out;
        out.type_ = core::Json::Type::Object;
        out.object_["present"] = Bool(false);
        return out;
    }
    if (name == "set_component") {
        const int idx = FindEntity(scene, args);
        if (idx < 0) return Err(Null(), "entity not found", -32602);
        const core::Json* cname = Member(args, "name");
        if (!cname || !cname->IsString()) return Err(Null(), "missing 'name'", -32602);
        const std::string comp = cname->GetString();
        const core::Json* value = Member(args, "value");
        if (!value) return Err(Null(), "missing 'value'", -32602);
        const std::string err = Validate(*value, comp);
        if (!err.empty()) return Err(Null(), err, -32602);
        auto& ents = scene.entities[static_cast<size_t>(idx)].components;
        for (auto& c : ents)
            if (c.name == comp) {
                c.data = *value;
                *changed = true;
                return Bool(true);
            }
        ents.push_back({comp, *value});
        *changed = true;
        return Bool(true);
    }
    return Err(Null(), "unknown tool: " + name, -32601);
}

} // namespace

const std::vector<McpTool>& Tools() {
    static const std::vector<McpTool> kTools = {
        {"list_entities", "List scene entities with their component names. args: scene"},
        {"get_component", "Read one entity's component value. args: scene, entity, name"},
        {"set_component", "Write one entity's component (validated by reflection). args: scene, entity, name, value"},
    };
    return kTools;
}

core::Json Handle(scene::SceneFile& scene, const core::Json& request, bool* changed) {
    if (changed) *changed = false;
    const core::Json* id = request.Get("id");
    core::Json idv = id ? *id : Null();
    if (!request.IsObject()) return Err(idv, "request must be a JSON object");

    const core::Json* method = request.Get("method");
    if (!method || !method->IsString()) return Err(idv, "missing 'method'");
    const std::string m = method->GetString();

    if (m == "initialize") {
        core::Json info;
        info.type_ = core::Json::Type::Object;
        info.object_["protocolVersion"] = Str("2025-06-18");
        info.object_["name"] = Str("neon-engine");
        return Ok(std::move(idv), std::move(info));
    }
    if (m == "tools/list") {
        core::Json list;
        list.type_ = core::Json::Type::Array;
        for (const auto& t : Tools()) {
            core::Json tjs;
            tjs.type_ = core::Json::Type::Object;
            tjs.object_["name"] = Str(t.name);
            tjs.object_["description"] = Str(t.description);
            list.array_.push_back(std::move(tjs));
        }
        core::Json res;
        res.type_ = core::Json::Type::Object;
        res.object_["tools"] = std::move(list);
        return Ok(std::move(idv), std::move(res));
    }
    if (m == "tools/call") {
        const core::Json* params = request.Get("params");
        const core::Json* tn = params ? params->Get("name") : nullptr;
        const std::string tool = (tn && tn->IsString()) ? tn->GetString() : "";
        const core::Json* argsVal = params ? params->Get("arguments") : nullptr;
        core::Json emptyArgs;
        emptyArgs.type_ = core::Json::Type::Object;
        const core::Json& args = argsVal ? *argsVal : emptyArgs;
        const core::Json raw = RunTool(scene, args, tool, changed);
        if (raw.Get("error")) return raw; // tool-level error already a JSON-RPC error
        core::Json result;
        result.type_ = core::Json::Type::Object;
        core::Json content;
        content.type_ = core::Json::Type::Array;
        core::Json item;
        item.type_ = core::Json::Type::Object;
        item.object_["type"] = Str("text");
        item.object_["text"] = Str(core::JsonWriter::Write(raw));
        content.array_.push_back(std::move(item));
        result.object_["content"] = std::move(content);
        return Ok(std::move(idv), std::move(result));
    }
    return Err(std::move(idv), "method not supported: " + m, -32601);
}

} // namespace neon::mcp
