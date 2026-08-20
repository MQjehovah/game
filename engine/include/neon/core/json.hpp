#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace neon::core {

// Minimal JSON DOM parser (glTF needs object/array/string/number/bool/null).
class Json {
public:
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object };

    Json() = default;

    static Json Parse(const std::string& text, std::string* error = nullptr);

    Type type() const { return type_; }
    bool IsNull() const { return type_ == Type::Null; }
    bool IsBool() const { return type_ == Type::Bool; }
    bool IsNumber() const { return type_ == Type::Number; }
    bool IsString() const { return type_ == Type::String; }
    bool IsArray() const { return type_ == Type::Array; }
    bool IsObject() const { return type_ == Type::Object; }

    bool GetBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double GetNumber(double def = 0.0) const { return type_ == Type::Number ? number_ : def; }
    int GetInt(int def = 0) const { return type_ == Type::Number ? static_cast<int>(number_) : def; }
    const std::string& GetString(const std::string& def = "") const {
        return type_ == Type::String ? string_ : def;
    }

    const Json* Get(const std::string& key) const {
        if (type_ != Type::Object) return nullptr;
        auto it = object_.find(key);
        return it != object_.end() ? &it->second : nullptr;
    }

    const Json* At(size_t index) const {
        if (type_ != Type::Array || index >= array_.size()) return nullptr;
        return &array_[index];
    }

    size_t Size() const { return type_ == Type::Array ? array_.size() : 0; }
    const std::map<std::string, Json>& Members() const { return object_; }
    const std::vector<Json>& Items() const { return array_; }

public: // fields are internal to the DOM; use accessors from game code
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Json> array_;
    std::map<std::string, Json> object_;
};

class JsonWriter {
public:
    static std::string Write(const Json& value);
    static std::string Escape(const std::string& s);
};

} // namespace neon::core
