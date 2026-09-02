#pragma once

// NeonEngine MCP server (D / Infernux "mcp plugin" style). A minimal Model
// Context Protocol handler: JSON-RPC 2.0 over newline-delimited messages. It
// operates on a parsed SceneFile so an AI assistant (Claude / opencode / ...) can
// list entities and read/write a single component field through the engine's
// reflection schema (TypeRegistry / FindComponentSchema). The handler is
// host-agnostic (no GUI, no renderer): the caller owns loading/saving the scene
// and routing stdin/stdout, which keeps it trivially unit-testable headlessly.
//
// Supported MCP methods:
//   initialize            -> protocol info
//   tools/list            -> the exposed tools
//   tools/call            -> run a tool
// Tool: list_entities     {scene}            -> [{id,name,components:[...]}]
// Tool: get_component     {scene,entity,name}-> {component: <json>, present: bool}
// Tool: set_component     {scene,entity,name,value} -> {ok,error?}
//
// `scene` is a scene file path (used to locate/validate; "" = the in-memory
// SceneFile). set_component validates the value against the component's
// reflection schema when one is registered (a data component without a schema
// accepts any JSON).

#include <vector>

#include "neon/core/json.hpp"
#include "neon/scene/scene_file.hpp"

namespace neon::mcp {

// Descriptor for tools/list.
struct McpTool {
    const char* name;
    const char* description;
};

const std::vector<McpTool>& Tools();

// Handle one JSON-RPC request against `scene`. `changed` is set when the call
// mutated `scene` (the caller then writes the scene file). Returns the JSON-RPC
// response ({"id","result"} or {"id","error"}), or an error object for a
// non-JSON-RPC / malformed request.
core::Json Handle(scene::SceneFile& scene, const core::Json& request, bool* changed);

} // namespace neon::mcp
