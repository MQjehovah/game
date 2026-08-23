#include "neon/core/localization.hpp"

#include <algorithm>

#include "neon/core/json.hpp"

namespace neon::core {

bool Localization::LoadTable(const std::string& jsonText, std::string* error) {
    std::string perr;
    Json root = Json::Parse(jsonText, &perr);
    if (root.IsNull() && !perr.empty()) {
        if (error) *error = "localization: JSON parse error: " + perr;
        return false;
    }
    if (!root.IsObject()) {
        if (error) *error = "localization: root must be an object of language tables";
        return false;
    }
    for (const auto& [lang, table] : root.Members()) {
        if (!table.IsObject()) {
            if (error) *error = "localization: language '" + lang + "' must be an object";
            return false;
        }
        for (const auto& [key, value] : table.Members()) {
            if (value.IsString()) tables_[lang][key] = value.GetString();
        }
    }
    if (language_.empty() && !tables_.empty()) language_ = tables_.begin()->first;
    if (defaultLanguage_.empty()) defaultLanguage_ = language_;
    return true;
}

std::string Localization::Get(const std::string& key) const {
    auto lang = tables_.find(language_);
    if (lang != tables_.end()) {
        auto it = lang->second.find(key);
        if (it != lang->second.end()) return it->second;
    }
    if (defaultLanguage_ != language_) {
        auto def = tables_.find(defaultLanguage_);
        if (def != tables_.end()) {
            auto it = def->second.find(key);
            if (it != def->second.end()) return it->second;
        }
    }
    return key;
}

bool Localization::HasLanguage(const std::string& lang) const {
    return tables_.find(lang) != tables_.end();
}

std::vector<std::string> Localization::Languages() const {
    std::vector<std::string> out;
    for (const auto& [lang, table] : tables_) out.push_back(lang);
    return out;
}

void Localization::Set(const std::string& lang, const std::string& key,
                       const std::string& value) {
    tables_[lang][key] = value;
    if (language_.empty()) language_ = lang;
    if (defaultLanguage_.empty()) defaultLanguage_ = lang;
}

std::string Localization::GetIn(const std::string& lang, const std::string& key) const {
    auto it = tables_.find(lang);
    if (it == tables_.end()) return {};
    auto kv = it->second.find(key);
    return kv == it->second.end() ? std::string() : kv->second;
}

std::vector<std::string> Localization::Keys() const {
    std::vector<std::string> out;
    for (const auto& langEntry : tables_) {
        for (const auto& kv : langEntry.second) {
            if (std::find(out.begin(), out.end(), kv.first) == out.end())
                out.push_back(kv.first);
        }
    }
    return out;
}

Json Localization::ToJson() const {
    Json root;
    root.type_ = Json::Type::Object;
    for (const auto& langEntry : tables_) {
        Json obj;
        obj.type_ = Json::Type::Object;
        for (const auto& kv : langEntry.second) {
            Json v;
            v.type_ = Json::Type::String;
            v.string_ = kv.second;
            obj.object_[kv.first] = std::move(v);
        }
        root.object_[langEntry.first] = std::move(obj);
    }
    return root;
}

} // namespace neon::core
