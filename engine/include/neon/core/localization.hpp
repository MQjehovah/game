#pragma once

#include <map>
#include <string>

#include "neon/core/json.hpp"

namespace neon::core {

// Godot-style localization: string tables loaded from JSON. A table file is a
// language map: {"zh": {"hello": "你好"}, "en": {"hello": "Hello"}}. The
// active language is a simple key; missing keys fall back to the default
// language, then to the key itself.
class Localization {
public:
    // Adds/replaces a whole language table (idempotent per language).
    bool LoadTable(const std::string& jsonText, std::string* error = nullptr);
    void SetLanguage(const std::string& lang) { language_ = lang; }
    const std::string& Language() const { return language_; }
    void SetDefaultLanguage(const std::string& lang) { defaultLanguage_ = lang; }
    const std::string& DefaultLanguage() const { return defaultLanguage_; }

    // Lookup with fallback chain: active -> default -> key.
    std::string Get(const std::string& key) const;
    bool HasLanguage(const std::string& lang) const;
    std::vector<std::string> Languages() const;

    // Editor-facing mutation + serialization.
    void Set(const std::string& lang, const std::string& key, const std::string& value);
    std::string GetIn(const std::string& lang, const std::string& key) const;
    std::vector<std::string> Keys() const; // union across languages
    Json ToJson() const;

private:
    std::map<std::string, std::map<std::string, std::string>> tables_;
    std::string language_;
    std::string defaultLanguage_;
};

} // namespace neon::core
