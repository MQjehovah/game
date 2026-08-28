#include "neon/ui/document.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "neon/core/log.hpp"
#include "neon/ui/layout_solver.hpp"

namespace neon::ui {

const char* UiNodeTypeName(UiNodeType type) {
    switch (type) {
        case UiNodeType::Panel: return "panel";
        case UiNodeType::Row: return "row";
        case UiNodeType::Column: return "column";
        case UiNodeType::Label: return "label";
        case UiNodeType::Button: return "button";
        case UiNodeType::Bar: return "bar";
        case UiNodeType::Image: return "image";
    }
    return "panel";
}

UiNodeType UiNodeTypeFromName(const std::string& name) {
    if (name == "row" || name == "hbox") return UiNodeType::Row;
    if (name == "column" || name == "vbox") return UiNodeType::Column;
    if (name == "label") return UiNodeType::Label;
    if (name == "button") return UiNodeType::Button;
    if (name == "bar") return UiNodeType::Bar;
    if (name == "image") return UiNodeType::Image;
    return UiNodeType::Panel;
}

namespace {

core::Json MakeNumber(double v) {
    core::Json j;
    j.type_ = core::Json::Type::Number;
    j.number_ = v;
    return j;
}

core::Json MakeString(const std::string& s) {
    core::Json j;
    j.type_ = core::Json::Type::String;
    j.string_ = s;
    return j;
}

core::Json MakeBool(bool b) {
    core::Json j;
    j.type_ = core::Json::Type::Bool;
    j.bool_ = b;
    return j;
}

core::Json MakeArray() {
    core::Json j;
    j.type_ = core::Json::Type::Array;
    return j;
}

core::Json MakeObject() {
    core::Json j;
    j.type_ = core::Json::Type::Object;
    return j;
}

void Put(core::Json& obj, const std::string& key, core::Json value) {
    obj.object_[key] = std::move(value);
}

} // namespace
namespace {

const char* JustifyName(UiJustify j) {
    switch (j) {
        case UiJustify::Center: return "center";
        case UiJustify::End: return "end";
        case UiJustify::SpaceBetween: return "space-between";
        case UiJustify::Start: return "start";
    }
    return "start";
}

UiJustify JustifyFromName(const std::string& s, UiJustify fallback) {
    if (s == "center") return UiJustify::Center;
    if (s == "end") return UiJustify::End;
    if (s == "space-between" || s == "between") return UiJustify::SpaceBetween;
    if (s == "start") return UiJustify::Start;
    return fallback;
}

const char* AlignName(UiAlign a) {
    switch (a) {
        case UiAlign::Center: return "center";
        case UiAlign::End: return "end";
        case UiAlign::Stretch: return "stretch";
        case UiAlign::Start: return "start";
    }
    return "start";
}

UiAlign AlignFromName(const std::string& s, UiAlign fallback) {
    if (s == "center") return UiAlign::Center;
    if (s == "end") return UiAlign::End;
    if (s == "stretch") return UiAlign::Stretch;
    if (s == "start") return UiAlign::Start;
    return fallback;
}

// Length <-> JSON: number = px; "50%" = percent; "auto"/"center" keywords.
core::Json LengthToJson(const UiLength& l) {
    switch (l.unit) {
        case UiLength::Unit::Px: return MakeNumber(l.value);
        case UiLength::Unit::Percent: return MakeString(std::to_string(l.value * 100.0f) + "%");
        case UiLength::Unit::Auto: return MakeString("auto");
        case UiLength::Unit::Center: return MakeString("center");
        case UiLength::Unit::Unset: break;
    }
    return MakeNumber(0);
}

UiLength LengthFromJson(const core::Json* j) {
    UiLength l;
    if (!j) return l;
    if (j->type() == core::Json::Type::Number) {
        l.unit = UiLength::Unit::Px;
        l.value = static_cast<float>(j->GetNumber());
    } else if (j->type() == core::Json::Type::String) {
        const std::string s = j->GetString();
        if (s == "auto") l.unit = UiLength::Unit::Auto;
        else if (s == "center") l.unit = UiLength::Unit::Center;
        else if (!s.empty() && s.back() == '%') {
            l.unit = UiLength::Unit::Percent;
            l.value = std::atof(s.c_str()) / 100.0f;
        }
    }
    return l;
}

} // namespace

UiNode* UiNode::AddChild(UiNodeType type, const std::string& childName) {
    auto node = std::make_unique<UiNode>();
    node->type = type;
    node->name = childName;
    // Sensible default LOCAL size inside the parent.
    node->rect = {20.0f, 20.0f, std::max(120.0f, rect.w * 0.4f),
                  std::max(36.0f, rect.h * 0.12f)};
    node->parent = this;
    if (type == UiNodeType::Label || type == UiNodeType::Button) node->text = childName;
    if (type == UiNodeType::Bar) {
        node->fill = 0.7f;
        node->color = {0.25f, 0.75f, 1.0f, 1.0f};
    }
    children.push_back(std::move(node));
    return children.back().get();
}

UiNode* UiNode::Find(const std::string& target) {
    if (name == target) return this;
    for (auto& c : children) {
        if (UiNode* hit = c->Find(target)) return hit;
    }
    return nullptr;
}

bool UiDocument::Load(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        NEON_LOG_ERROR("UI: cannot open '%s'", path.c_str());
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    return LoadJson(ss.str());
}

bool UiDocument::Save(const std::string& path) const {
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open()) {
        NEON_LOG_ERROR("UI: cannot write '%s'", path.c_str());
        return false;
    }
    out << ToJson();
    return static_cast<bool>(out);
}

bool UiDocument::LoadJson(const std::string& text) {
    std::string error;
    core::Json doc = core::Json::Parse(text, &error);
    if (doc.IsNull() || !error.empty()) {
        NEON_LOG_ERROR("UI: JSON parse failed: %s", error.c_str());
        return false;
    }
    const core::Json* rootJson = doc.Get("root");
    if (!rootJson) {
        NEON_LOG_ERROR("UI: missing 'root' node");
        return false;
    }
    root = UiNode{};
    root.type = UiNodeType::Panel;
    root.name = "root";
    root.rect = {0, 0, 1280, 720};
    root.parent = nullptr;
    if (const core::Json* s = doc.Get("solver")) {
        if (s->type() == core::Json::Type::String) solver = s->GetString();
    }
    std::string parseError;
    if (!ParseNode(*rootJson, root, parseError)) {
        NEON_LOG_ERROR("UI: node error: %s", parseError.c_str());
        return false;
    }
    MarkLayoutDirty(); // B12: a fresh document needs a fresh layout
    return true;
}

std::string UiDocument::ToJson() const {
    core::Json doc = MakeObject();
    Put(doc, "format", MakeString("neon-ui"));
    Put(doc, "version", MakeNumber(1));
    Put(doc, "solver", MakeString(solver));
    core::Json rootJson = MakeObject();
    SerializeNode(root, rootJson);
    Put(doc, "root", std::move(rootJson));
    return core::JsonWriter::WritePretty(doc);
}

bool UiDocument::SerializeNode(const UiNode& node, core::Json& out) {
    out = MakeObject();
    Put(out, "type", MakeString(UiNodeTypeName(node.type)));
    Put(out, "name", MakeString(node.name));
    core::Json rect = MakeArray();
    rect.array_ = {MakeNumber(node.rect.x), MakeNumber(node.rect.y), MakeNumber(node.rect.w),
                   MakeNumber(node.rect.h)};
    Put(out, "rect", std::move(rect));
    core::Json color = MakeArray();
    color.array_ = {MakeNumber(node.color.r), MakeNumber(node.color.g), MakeNumber(node.color.b),
                     MakeNumber(node.color.a)};
    Put(out, "color", std::move(color));
    core::Json border = MakeArray();
    border.array_ = {MakeNumber(node.borderColor.r), MakeNumber(node.borderColor.g),
                      MakeNumber(node.borderColor.b), MakeNumber(node.borderColor.a)};
    Put(out, "border", std::move(border));
    Put(out, "text", MakeString(node.text));
    Put(out, "sprite", MakeString(node.sprite));
    Put(out, "slice", MakeNumber(node.slice));
    Put(out, "fill", MakeNumber(node.fill));
    Put(out, "fontSize", MakeNumber(node.fontSize));
    Put(out, "visible", MakeBool(node.visible));
    Put(out, "clip", MakeBool(node.clipChildren));
    // Box-layout fields (written only when set; legacy nodes stay compact).
    if (node.left.IsSet()) Put(out, "left", LengthToJson(node.left));
    if (node.top.IsSet()) Put(out, "top", LengthToJson(node.top));
    if (node.right.IsSet()) Put(out, "right", LengthToJson(node.right));
    if (node.bottom.IsSet()) Put(out, "bottom", LengthToJson(node.bottom));
    if (node.width.IsSet()) Put(out, "width", LengthToJson(node.width));
    if (node.height.IsSet()) Put(out, "height", LengthToJson(node.height));
    if (node.gap != 0.0f) Put(out, "gap", MakeNumber(node.gap));
    if (node.padding != 0.0f) Put(out, "padding", MakeNumber(node.padding));
    if (node.justify != UiJustify::Start) Put(out, "justify", MakeString(JustifyName(node.justify)));
    if (node.align != UiAlign::Start) Put(out, "align", MakeString(AlignName(node.align)));
    // Unrecognized fields ride along verbatim (extension seam).
    if (node.extra.type_ == core::Json::Type::Object) {
        for (const auto& [k, v] : node.extra.object_) Put(out, k, v);
    }
    if (!node.children.empty()) {
        core::Json children = MakeArray();
        for (const auto& c : node.children) {
            core::Json child = MakeObject();
            SerializeNode(*c, child);
            children.array_.push_back(std::move(child));
        }
        Put(out, "children", std::move(children));
    }
    return true;
}

namespace {

bool ReadVec4(const core::Json* j, float out[4]) {
    if (!j || j->type() != core::Json::Type::Array || j->Size() < 3) return false;
    for (int i = 0; i < 3; ++i) out[i] = static_cast<float>(j->At(i)->GetNumber());
    out[3] = j->Size() >= 4 ? static_cast<float>(j->At(3)->GetNumber()) : 1.0f;
    return true;
}

} // namespace

bool UiDocument::ParseNode(const core::Json& in, UiNode& out, std::string& error) {
    if (in.type() != core::Json::Type::Object) {
        error = "node must be an object";
        return false;
    }
    if (const core::Json* t = in.Get("type")) out.type = UiNodeTypeFromName(t->GetString());
    if (const core::Json* n = in.Get("name")) out.name = n->GetString();
    if (const core::Json* r = in.Get("rect")) {
        if (r->type() == core::Json::Type::Array && r->Size() >= 4) {
            out.rect.x = static_cast<float>(r->At(0)->GetNumber());
            out.rect.y = static_cast<float>(r->At(1)->GetNumber());
            out.rect.w = static_cast<float>(r->At(2)->GetNumber());
            out.rect.h = static_cast<float>(r->At(3)->GetNumber());
        }
    }
    float c[4];
    if (ReadVec4(in.Get("color"), c)) out.color = {c[0], c[1], c[2], c[3]};
    if (ReadVec4(in.Get("border"), c)) out.borderColor = {c[0], c[1], c[2], c[3]};
    if (const core::Json* t = in.Get("text")) out.text = t->GetString();
    if (const core::Json* s = in.Get("sprite")) out.sprite = s->GetString();
    if (const core::Json* s = in.Get("slice")) out.slice = static_cast<float>(s->GetNumber());
    if (const core::Json* f = in.Get("fill")) out.fill = static_cast<float>(f->GetNumber());
    if (const core::Json* f = in.Get("fontSize")) out.fontSize = static_cast<float>(f->GetNumber());
    if (const core::Json* v = in.Get("visible")) out.visible = v->GetBool(true);
    if (const core::Json* v = in.Get("clip")) out.clipChildren = v->GetBool(true);
    // Box-layout fields.
    out.left = LengthFromJson(in.Get("left"));
    out.top = LengthFromJson(in.Get("top"));
    out.right = LengthFromJson(in.Get("right"));
    out.bottom = LengthFromJson(in.Get("bottom"));
    out.width = LengthFromJson(in.Get("width"));
    out.height = LengthFromJson(in.Get("height"));
    if (const core::Json* g = in.Get("gap")) out.gap = static_cast<float>(g->GetNumber());
    if (const core::Json* p = in.Get("padding")) out.padding = static_cast<float>(p->GetNumber());
    if (const core::Json* j = in.Get("justify"))
        out.justify = JustifyFromName(j->GetString(), out.justify);
    if (const core::Json* a = in.Get("align"))
        out.align = AlignFromName(a->GetString(), out.align);
    // Extension seam: keep UNRECOGNIZED fields verbatim so future layout
    // strategies (and other tools) round-trip their own attributes.
    {
        static const char* kKnown[] = {"type",      "name",  "rect",  "color", "border",
                                       "text",      "sprite", "slice", "fill",  "fontSize",
                                       "visible",   "clip",  "left",  "top",   "right",
                                       "bottom",    "width", "height", "gap",   "padding",
                                       "justify",   "align", "children"};
        out.extra = MakeObject();
        for (const auto& [k, v] : in.object_) {
            bool known = false;
            for (const char* kn : kKnown)
                if (k == kn) { known = true; break; }
            if (!known) Put(out.extra, k, v);
        }
    }
    if (const core::Json* ch = in.Get("children")) {
        if (ch->type() == core::Json::Type::Array) {
            for (size_t i = 0; i < ch->Size(); ++i) {
                auto node = std::make_unique<UiNode>();
                std::string childError;
                if (!ParseNode(*ch->At(i), *node, childError)) {
                    error = childError;
                    return false;
                }
                node->parent = &out;
                out.children.push_back(std::move(node));
            }
        }
    }
    return true;
}

void UiDocument::Draw(gfx::Renderer& renderer, const gfx::Font& font,
                      const UiTextureLoader& loadTexture,
                      const math::Vec2& viewportSize) const {
    Layout(viewportSize, &font);
    struct Frame {
        const UiNode* node;
        math::Rect2 clip;
    };
    // G3-5: draws a node's background/image. When `sprite` + `loadTexture` are
    // available it becomes a textured quad; with a `slice` > 0 the texture is
    // split into 9 quads so corners keep their size and edges stretch.
    auto drawBackground = [&](const UiNode& n, const math::Rect2& clip) {
        if (loadTexture && !n.sprite.empty()) {
            gfx::Texture tex = loadTexture(n.sprite);
            if (tex.Valid()) {
                if (n.slice > 0.0f) {
                    NineSliceQuad quads[9];
                    if (ComputeNineSlice(clip, n.slice, static_cast<float>(tex.Width()),
                                         static_cast<float>(tex.Height()), quads)) {
                        for (const NineSliceQuad& q : quads)
                            renderer.DrawQuad({q.dest.x, q.dest.y}, {q.dest.w, q.dest.h},
                                              n.color, tex.Handle(), q.uv0, q.uv1);
                        return;
                    }
                }
                renderer.DrawQuad({clip.x, clip.y}, {clip.w, clip.h}, n.color, tex.Handle());
                return;
            }
        }
        renderer.DrawRect({clip.x, clip.y}, {clip.w, clip.h}, n.color);
    };

    std::vector<Frame> stack;
    stack.push_back({&root, root.ResolvedRect()});
    while (!stack.empty()) {
        const Frame frame = stack.back();
        stack.pop_back();
        const UiNode& n = *frame.node;
        if (!n.visible) continue;

        const math::Rect2 abs = n.ResolvedRect();
        const math::Rect2 clip = {
            std::max(frame.clip.x, abs.x),
            std::max(frame.clip.y, abs.y),
            std::min(frame.clip.x + frame.clip.w, abs.x + abs.w) -
                std::max(frame.clip.x, abs.x),
            std::min(frame.clip.y + frame.clip.h, abs.y + abs.h) -
                std::max(frame.clip.y, abs.y)};
        if (clip.w <= 0.0f || clip.h <= 0.0f) continue;

        switch (n.type) {
            case UiNodeType::Panel:
            case UiNodeType::Row:
            case UiNodeType::Column: {
                drawBackground(n, clip);
                renderer.DrawRectOutline(clip, 1.0f, n.borderColor);
                break;
            }
            case UiNodeType::Label: {
                // Visual vertical centering: DrawText's centerY centers the
                // font line box, but CJK ink occupies the upper part of it, so
                // text ends up above the rect center. Use a baseline placed so
                // the glyph ink sits centered instead.
                const float scale = n.fontSize / static_cast<float>(font.BakedSize());
                const float baseline =
                    clip.y + clip.h * 0.5f + (font.Ascent() + font.Descent()) * 0.5f * scale;
                // G3-5: rich text (["color:#..]..[/color]") renders per-segment
                // colors; plain text draws exactly as before.
                DrawRichLabel(renderer, font, n.text, {clip.x + clip.w * 0.5f, baseline},
                              n.fontSize, n.color, /*centerX=*/true);
                break;
            }
            case UiNodeType::Button: {
                drawBackground(n, clip);
                renderer.DrawRectOutline(clip, 2.0f, n.borderColor);
                const float scale = n.fontSize / static_cast<float>(font.BakedSize());
                const float baseline =
                    clip.y + clip.h * 0.5f + (font.Ascent() + font.Descent()) * 0.5f * scale;
                renderer.DrawText(font, n.text, {clip.x + clip.w * 0.5f, baseline},
                                  n.fontSize, {1, 1, 1, 1}, true, false);
                break;
            }
            case UiNodeType::Bar: {
                renderer.DrawRect({clip.x, clip.y}, {clip.w, clip.h}, {0.08f, 0.09f, 0.13f, 1.0f});
                const float fw = clip.w * std::clamp(n.fill, 0.0f, 1.0f);
                if (fw > 0.0f) renderer.DrawRect({clip.x, clip.y}, {fw, clip.h}, n.color);
                renderer.DrawRectOutline(clip, 1.0f, n.borderColor);
                break;
            }
            case UiNodeType::Image: {
                drawBackground(n, clip);
                break;
            }
        }

        // Children (topmost last, so push in reverse).
        for (size_t i = n.children.size(); i-- > 0;) {
            stack.push_back({n.children[i].get(), n.clipChildren ? clip : abs});
        }
    }
}

UiNode* UiDocument::HitTest(const math::Vec2& p, const math::Vec2& viewportSize) {
    // Box-layout nodes only have a resolved rect - make sure one is fresh.
    Layout(viewportSize, nullptr);
    struct Item {
        UiNode* node;
        int depth;
    };
    std::vector<Item> stack;
    stack.push_back({&root, 0});
    UiNode* best = nullptr;
    int bestDepth = -1;
    while (!stack.empty()) {
        Item item = stack.back();
        stack.pop_back();
        if (!item.node->visible) continue;
        if (item.node->Contains(p)) {
            // Deepest node wins; among equal depth, the later sibling (drawn
            // on top) wins.
            if (item.depth > bestDepth) {
                best = item.node;
                bestDepth = item.depth;
            }
        }
        for (auto& c : item.node->children) stack.push_back({c.get(), item.depth + 1});
    }
    return best;
}

void UiDocument::Layout(const math::Vec2& viewportSize, const gfx::Font* font) const {
    // B12: memoized -- unchanged documents and same-viewport double solves
    // (Draw + HitTest in one frame) skip the tree walk entirely.
    if (!layoutDirty_ && layoutViewport_.x == viewportSize.x &&
        layoutViewport_.y == viewportSize.y) {
        return;
    }
    // Pluggable strategy (layout_solver.hpp): "absolute" keeps the legacy
    // rect chain; anything else resolves through the registered solver.
    ILayoutSolver* s = FindLayoutSolver(solver);
    if (!s) s = AbsoluteLayoutSolver();
    s->Solve(const_cast<UiNode&>(root), viewportSize, FontTextMeasure(font));
    layoutDirty_ = false;
    layoutViewport_ = viewportSize;
}

} // namespace neon::ui
