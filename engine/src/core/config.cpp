#include "neon/core/config.hpp"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "neon/core/log.hpp"

namespace neon::core {

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool Config::Load(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return false;
    std::string line;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        if (!key.empty()) kv_[key] = value;
    }
    return true;
}

bool Config::Save(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) return false;
    for (const auto& [key, value] : kv_) {
        out << key << "=" << value << "\n";
    }
    return out.good();
}

int Config::GetInt(const std::string& key, int def) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return def;
    return std::atoi(it->second.c_str());
}

float Config::GetFloat(const std::string& key, float def) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return def;
    return static_cast<float>(std::atof(it->second.c_str()));
}

bool Config::GetBool(const std::string& key, bool def) const {
    auto it = kv_.find(key);
    if (it == kv_.end()) return def;
    const std::string& v = it->second;
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::string Config::GetString(const std::string& key, const std::string& def) const {
    auto it = kv_.find(key);
    return it == kv_.end() ? def : it->second;
}

void Config::SetInt(const std::string& key, int v) { kv_[key] = std::to_string(v); }
void Config::SetFloat(const std::string& key, float v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%g", v);
    kv_[key] = buf;
}
void Config::SetBool(const std::string& key, bool v) { kv_[key] = v ? "true" : "false"; }
void Config::SetString(const std::string& key, const std::string& v) { kv_[key] = v; }

namespace {

void ApplyLogCatItem(const std::string& item) {
    const size_t colon = item.find(':');
    if (colon == std::string::npos) {
        std::fprintf(stderr, "neon: malformed --log-cat '%s' (expected name:level)\n",
                     item.c_str());
        return;
    }
    const std::string catName = item.substr(0, colon);
    const std::string levelName = item.substr(colon + 1);
    LogLevel level;
    if (!LogLevelFromName(levelName, level)) {
        std::fprintf(stderr,
                     "neon: unknown log level '%s' in --log-cat "
                     "(debug|info|warn|error)\n",
                     levelName.c_str());
        return;
    }
    SetCategoryLogLevel(CategoryFromName(catName), level);
}

} // namespace

void ApplyLogCli(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            LogLevel level;
            if (!LogLevelFromName(argv[i + 1], level)) {
                std::fprintf(stderr,
                             "neon: unknown --log-level '%s' (debug|info|warn|error)\n",
                             argv[i + 1]);
            } else {
                SetLogLevel(level);
            }
            ++i;
        } else if (std::strcmp(argv[i], "--log-cat") == 0 && i + 1 < argc) {
            const std::string value = argv[i + 1];
            for (size_t start = 0; start <= value.size();) {
                const size_t comma = value.find(',', start);
                const size_t end =
                    comma == std::string::npos ? value.size() : comma;
                ApplyLogCatItem(value.substr(start, end - start));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            ++i;
        }
    }
}

} // namespace neon::core
