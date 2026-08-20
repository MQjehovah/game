#include <cstdint>
#include <string>

#include "neon/neon.hpp"
#include "neon/core/json.hpp"
#include "helpers.hpp"

using namespace neon;

// ---------------------------------------------------------------------------
// core::Json + JsonWriter
// ---------------------------------------------------------------------------

TEST(JsonScalarTypes) {
    core::Json n = core::Json::Parse("null");
    CHECK(n.IsNull());

    core::Json t = core::Json::Parse("true");
    CHECK(t.IsBool());
    CHECK(t.GetBool(false));

    core::Json f = core::Json::Parse("false");
    CHECK(f.IsBool());
    CHECK(!f.GetBool(true));

    core::Json num = core::Json::Parse("  42.5  ");
    CHECK(num.IsNumber());
    CHECK_NEAR(num.GetNumber(), 42.5, 1e-9);

    core::Json neg = core::Json::Parse("-2.5e-3");
    CHECK_NEAR(neg.GetNumber(), -0.0025, 1e-12);

    core::Json exp = core::Json::Parse("1.5e3");
    CHECK_NEAR(exp.GetNumber(), 1500.0, 1e-9);

    core::Json str = core::Json::Parse("\"hello\"");
    CHECK(str.IsString());
    CHECK_EQ(str.GetString(), std::string("hello"));
}

TEST(JsonArrayObjectAccess) {
    core::Json root = core::Json::Parse(
        "{\"name\":\"neon\",\"count\":3,\"flags\":[true,false,null],\"nested\":{\"x\":1.5}}");
    CHECK(root.IsObject());
    CHECK_EQ(root.Get("name")->GetString(), std::string("neon"));
    CHECK_EQ(root.Get("count")->GetInt(-1), 3);

    const core::Json* flags = root.Get("flags");
    CHECK(flags->IsArray());
    CHECK_EQ(flags->Size(), 3u);
    CHECK(flags->At(0)->GetBool(false));
    CHECK(!flags->At(1)->GetBool(true));
    CHECK(flags->At(2)->IsNull());
    CHECK(flags->At(3) == nullptr);

    const core::Json* nested = root.Get("nested");
    CHECK(nested->IsObject());
    CHECK_NEAR(nested->Get("x")->GetNumber(), 1.5, 1e-9);

    // Missing keys and defaults.
    CHECK_EQ(root.Get("missing"), nullptr);
    CHECK_EQ(root.Get("count")->GetInt(-99), 3);
}

// UTF-8 escapes: single BMP escapes, and surrogate pairs decoding to 4-byte
// sequences (U+1F389 PARTY POPPER -> F0 9F 8E 89).
TEST(JsonUtf8EscapesAndSurrogates) {
    std::string error;
    core::Json j = core::Json::Parse(
        "[\"\\u00e9\",\"\\u4e2d\",\"\\ud83c\\udf89\"]", &error);
    CHECK(!j.IsNull());
    CHECK_EQ(error, std::string());
    CHECK(j.IsArray());
    CHECK_EQ(j.Size(), 3u);

    CHECK_EQ(j.At(0)->GetString(), std::string("\xC3\xA9"));           // e-acute
    CHECK_EQ(j.At(1)->GetString(), std::string("\xE4\xB8\xAD"));       // CJK 中
    CHECK_EQ(j.At(2)->GetString(), std::string("\xF0\x9F\x8E\x89"));   // U+1F389

    // Raw UTF-8 bytes pass through unchanged.
    core::Json raw = core::Json::Parse("\"\xE4\xB8\xAD\xE6\x96\x87\"");
    CHECK_EQ(raw.GetString(), std::string("\xE4\xB8\xAD\xE6\x96\x87"));
}

TEST(JsonWriterEscapes) {
    CHECK_EQ(core::JsonWriter::Escape("a\"b\\c\n\tt"), std::string("a\\\"b\\\\c\\n\\tt"));
    CHECK_EQ(core::JsonWriter::Escape("\b\f"), std::string("\\b\\f"));
    CHECK_EQ(core::JsonWriter::Escape(std::string("\x01")), std::string("\\u0001"));
    CHECK_EQ(core::JsonWriter::Escape("plain"), std::string("plain"));
}

// Parse -> write -> parse -> write is stable, and values survive the trip.
TEST(JsonRoundTripIdempotent) {
    const std::string text =
        "{\"z\":9,\"a\":[1,-2.5,3e2,true,false,null],\"s\":\"hi\\n\\\"you\\\"\","
        "\"u\":\"\\ud83c\\udf89\",\"obj\":{\"deep\":{\"k\":0.125}}}";
    core::Json v1 = core::Json::Parse(text);
    CHECK(v1.IsObject());
    CHECK_EQ(v1.Get("a")->Size(), 6u);
    CHECK_EQ(v1.Get("s")->GetString(), std::string("hi\n\"you\""));
    CHECK_EQ(v1.Get("u")->GetString(), std::string("\xF0\x9F\x8E\x89"));
    CHECK_NEAR(v1.Get("obj")->Get("deep")->Get("k")->GetNumber(), 0.125, 1e-12);

    const std::string out1 = core::JsonWriter::Write(v1);
    core::Json v2 = core::Json::Parse(out1);
    CHECK(!v2.IsNull());
    const std::string out2 = core::JsonWriter::Write(v2);
    CHECK_EQ(out1, out2);

    // Values still correct after the round trip.
    core::Json back = core::Json::Parse(out2);
    CHECK_EQ(back.Get("a")->At(0)->GetNumber(), 1.0);
    CHECK_NEAR(back.Get("a")->At(2)->GetNumber(), 300.0, 1e-9);
    CHECK(back.Get("a")->At(3)->GetBool(false));
    CHECK_EQ(back.Get("s")->GetString(), std::string("hi\n\"you\""));
    CHECK_EQ(back.Get("u")->GetString(), std::string("\xF0\x9F\x8E\x89"));
}

// Duplicate object keys collapse to the last value (std::map storage).
TEST(JsonDuplicateKeysLastWins) {
    core::Json j = core::Json::Parse("{\"a\":1,\"a\":2,\"b\":3}");
    CHECK_EQ(j.Get("a")->GetInt(-1), 2);
    CHECK_EQ(j.Members().size(), 2u);
}

TEST(JsonParseErrors) {
    std::string error;
    core::Json j1 = core::Json::Parse("{\"a\":}", &error);
    CHECK(j1.IsNull());
    CHECK(!error.empty());

    std::string error2;
    core::Json j2 = core::Json::Parse("\"unterminated", &error2);
    CHECK(j2.IsNull());
    CHECK(!error2.empty());

    std::string error3;
    core::Json j3 = core::Json::Parse("tru", &error3);
    CHECK(j3.IsNull());
    CHECK(!error3.empty());

    core::Json j4 = core::Json::Parse("");
    CHECK(j4.IsNull());
}

// Deeply nested arrays parse and round-trip without exhausting the stack.
TEST(JsonDeepNesting) {
    const int kDepth = 1000;
    std::string text;
    text.reserve(static_cast<size_t>(kDepth) * 2);
    for (int i = 0; i < kDepth; ++i) text += '[';
    text += "42";
    for (int i = 0; i < kDepth; ++i) text += ']';

    core::Json v = core::Json::Parse(text);
    CHECK(v.IsArray());
    int depth = 0;
    const core::Json* node = &v;
    while (node->IsArray() && node->Size() == 1u) {
        node = node->At(0);
        ++depth;
    }
    CHECK_EQ(depth, kDepth);
    CHECK(node->IsNumber());
    CHECK_EQ(node->GetInt(-1), 42);

    const std::string out = core::JsonWriter::Write(v);
    CHECK_EQ(out, text);
}
