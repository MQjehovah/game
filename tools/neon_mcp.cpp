// NeonEngine MCP stdio server (D): reads newline-delimited JSON-RPC 2.0 from
// stdin, operates on a scene JSON file (load --scene, write back on change), and
// writes newline-delimited responses to stdout. Keeps the MCP handler (mcp_server)
// host-agnostic; this file is only the stdio <-> scene file bridge.
//
// Usage:
//   neon_mcp --scene assets/scenes/foo.json        # load once, serve stdio, save on change

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

#include "neon/core/json.hpp"
#include "neon/mcp/mcp_server.hpp"
#include "neon/scene/scene_file.hpp"

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool WriteFile(const std::string& path, const std::string& s) {
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
    return out.good();
}

} // namespace

int main(int argc, char** argv) {
    std::string scenePath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scene" && i + 1 < argc) scenePath = argv[++i];
    }

    neon::scene::SceneFile scene;
    bool loaded = false;
    if (!scenePath.empty()) {
        if (auto r = neon::scene::SceneFile::Parse(ReadFile(scenePath)); r.Ok()) {
            scene = std::move(r.Value());
            loaded = true;
        } else {
            std::cerr << "neon_mcp: failed to parse scene: " << r.Error() << "\n";
            return 1;
        }
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        // Strip a trailing '\r'.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        neon::core::Json req = neon::core::Json::Parse(line);
        bool changed = false;
        neon::core::Json res = neon::mcp::Handle(scene, req, &changed);
        std::cout << neon::core::JsonWriter::Write(res) << "\n";
        std::cout.flush();
        if (changed && !scenePath.empty()) {
            // Persist the mutated scene (pretty-printed for git-friendly diffs).
            WriteFile(scenePath, neon::core::JsonWriter::WritePretty(scene.ToJson()));
        }
    }
    return 0;
}
