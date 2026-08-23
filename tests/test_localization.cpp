#include <string>

#include "neon/core/localization.hpp"
#include "neon/neon.hpp"
#include "helpers.hpp"

using namespace neon;

// Language fallback chain: active -> default -> key.
TEST(LocalizationFallbackChain) {
    core::Localization loc;
    std::string err;
    CHECK(loc.LoadTable(
        R"({"zh": {"hello": "你好", "only_zh": "中文"}, "en": {"hello": "Hello"}})",
        &err));
    CHECK_EQ(err, std::string());
    loc.SetDefaultLanguage("zh");
    loc.SetLanguage("en");
    CHECK_EQ(loc.Get("hello"), std::string("Hello"));
    // Missing in active, present in default (zh).
    CHECK_EQ(loc.Get("only_zh"), std::string("中文"));
    // Missing everywhere -> the key itself.
    CHECK_EQ(loc.Get("missing"), std::string("missing"));
    // Explicit default.
    loc.SetDefaultLanguage("zh");
    loc.SetLanguage("en");
    CHECK_EQ(loc.Get("only_zh"), std::string("中文"));
}

// Editing API + serialization round trip.
TEST(LocalizationEditAndSerialize) {
    core::Localization loc;
    loc.SetLanguage("zh");
    loc.SetDefaultLanguage("zh");
    loc.Set("zh", "greet", "你好");
    loc.Set("en", "greet", "Hi");
    CHECK_EQ(loc.Get("greet"), std::string("你好"));
    CHECK_EQ(loc.GetIn("en", "greet"), std::string("Hi"));
    const std::vector<std::string> langs = loc.Languages();
    CHECK(langs.size() == 2u);
    const std::vector<std::string> keys = loc.Keys();
    CHECK(keys.size() == 1u && keys[0] == "greet");

    const std::string json = core::JsonWriter::Write(loc.ToJson());
    core::Localization back;
    CHECK(back.LoadTable(json));
    back.SetLanguage("en");
    CHECK_EQ(back.Get("greet"), std::string("Hi"));
}

// Multiple locale files merge; invalid JSON reports an error and keeps prior
// tables intact.
TEST(LocalizationMergeAndErrors) {
    core::Localization loc;
    std::string err;
    CHECK(loc.LoadTable(R"({"zh": {"a": "1"}})", &err));
    CHECK(loc.LoadTable(R"({"en": {"a": "A", "b": "B"}})", &err));
    CHECK_EQ(loc.Keys().size(), 2u);
    CHECK(!loc.LoadTable("not json{", &err));
    CHECK(!err.empty());
    CHECK_EQ(loc.Get("a"), std::string("1")); // prior tables survive
}
