#pragma once
#include <map>
#include <string>

namespace neon::core {

// Minimal INI-ish key=value config, used for save data and settings.
class Config {
public:
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;

    int GetInt(const std::string& key, int def = 0) const;
    float GetFloat(const std::string& key, float def = 0.0f) const;
    bool GetBool(const std::string& key, bool def = false) const;
    std::string GetString(const std::string& key, const std::string& def = "") const;

    void SetInt(const std::string& key, int v);
    void SetFloat(const std::string& key, float v);
    void SetBool(const std::string& key, bool v);
    void SetString(const std::string& key, const std::string& v);

    bool Has(const std::string& key) const { return kv_.count(key) > 0; }
    void Clear() { kv_.clear(); }

private:
    std::map<std::string, std::string> kv_;
};

// Applies log-related command-line options found in argv[1..argc):
//   --log-level <debug|info|warn|error>   global minimum level
//   --log-cat <name>:<level>              per-category override; repeatable,
//                                         and values may be comma-separated
//                                         (e.g. "gfx:debug,audio:warn")
// Unknown level names and malformed items print a warning to stderr and are
// ignored; unknown category names map to Core (see CategoryFromName).
// Matches the argv-scan style used by --smoke-test/--screenshot in main().
void ApplyLogCli(int argc, char** argv);

} // namespace neon::core
