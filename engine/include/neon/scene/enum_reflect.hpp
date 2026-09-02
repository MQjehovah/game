#pragma once

// NeonEngine enum reflection (G2-1). A tiny, deterministic, dependency-free
// alternative to magic_enum, which was evaluated (v0.9.3) but its compiler
// compatibility check rejects the project's MinGW GCC 8.1 toolchain
// ("magic_enum unsupported compiler"). This macro works on GCC 8.1 / MSVC and
// is exactly what the editor combo + JSON codec need: an ordered name<->value
// table. The enum must have contiguous underlying values starting at 0 (the
// common declared-in-order case); a non-contiguous enum must not be reflected.
//
// Usage:
//   enum class ArmorKind { None, Cone, Bucket };
//   NEO_ENUM(ArmorKind, None, Cone, Bucket);      // value index == declaration order
//
// EnumSpecs<T> is the trait a reflected enum specializes; the reflector
// (component_reflect.hpp) detects it via the `Enabled` tag and fails to compile
// for enums that were never reflected.

#include <cstring>

namespace neon::scene {

// Primary template: no reflection until NEO_ENUM adds an EnumSpecs specialization.
template <typename E>
struct EnumSpecs {
    static constexpr bool Enabled = false;

    using EnumType = E;
    static const char** Names() { return nullptr; }
    static int Count() { return 0; }
    static const char* ToString(E) { return nullptr; }
    static bool FromString(const char* s, E& out) {
        (void)s;
        (void)out;
        return false;
    }
};

namespace reflect_detail {

// Shared per-enum backing store (declared for the reflector; kept here so
// NEO_ENUM users can query options without including component_reflect.hpp).
template <typename E>
struct EnumStore {
    static const EnumStore& Get() {
        static EnumStore inst;
        return inst;
    }
    EnumStore() : names(EnumSpecs<E>::Names()), count(EnumSpecs<E>::Count()) {}
    const char* const* names;
    int count;
};

} // namespace reflect_detail

} // namespace neon::scene

// ---- Variadic FOR_EACH (tokenizing stringify), up to 16 enumerators --------
#define NEO_ENUM_STR(x) #x
// Two-level token paste: a `##` operand is not macro-expanded, so the count
// must be resolved by an intermediate CAT that expands its arguments first.
#define NEO_ENUM_CAT_I(a, b) a##b
#define NEO_ENUM_CAT(a, b) NEO_ENUM_CAT_I(a, b)
#define NEO_ENUM_FE_1(f, x) f(x)
#define NEO_ENUM_FE_16(f, a, b, c, d, e, f2, g, h, i, j, k, l, m, n, o, p) \
    f(a), NEO_ENUM_FE_15(f, b, c, d, e, f2, g, h, i, j, k, l, m, n, o, p)
#define NEO_ENUM_FE_15(f, a, b, c, d, e, f2, g, h, i, j, k, l, m, n, o) \
    f(a), NEO_ENUM_FE_14(f, b, c, d, e, f2, g, h, i, j, k, l, m, n, o)
#define NEO_ENUM_FE_14(f, a, b, c, d, e, f2, g, h, i, j, k, l, m, n) \
    f(a), NEO_ENUM_FE_13(f, b, c, d, e, f2, g, h, i, j, k, l, m, n)
#define NEO_ENUM_FE_13(f, a, b, c, d, e, f2, g, h, i, j, k, l, m) \
    f(a), NEO_ENUM_FE_12(f, b, c, d, e, f2, g, h, i, j, k, l, m)
#define NEO_ENUM_FE_12(f, a, b, c, d, e, f2, g, h, i, j, k, l) \
    f(a), NEO_ENUM_FE_11(f, b, c, d, e, f2, g, h, i, j, k, l)
#define NEO_ENUM_FE_11(f, a, b, c, d, e, f2, g, h, i, j, k) \
    f(a), NEO_ENUM_FE_10(f, b, c, d, e, f2, g, h, i, j, k)
#define NEO_ENUM_FE_10(f, a, b, c, d, e, f2, g, h, i, j) \
    f(a), NEO_ENUM_FE_9(f, b, c, d, e, f2, g, h, i, j)
#define NEO_ENUM_FE_9(f, a, b, c, d, e, f2, g, h, i) \
    f(a), NEO_ENUM_FE_8(f, b, c, d, e, f2, g, h, i)
#define NEO_ENUM_FE_8(f, a, b, c, d, e, f2, g, h) \
    f(a), NEO_ENUM_FE_7(f, b, c, d, e, f2, g, h)
#define NEO_ENUM_FE_7(f, a, b, c, d, e, f2, g) \
    f(a), NEO_ENUM_FE_6(f, b, c, d, e, f2, g)
#define NEO_ENUM_FE_6(f, a, b, c, d, e, f2) \
    f(a), NEO_ENUM_FE_5(f, b, c, d, e, f2)
#define NEO_ENUM_FE_5(f, a, b, c, d, e) f(a), NEO_ENUM_FE_4(f, b, c, d, e)
#define NEO_ENUM_FE_4(f, a, b, c, d) f(a), NEO_ENUM_FE_3(f, b, c, d)
#define NEO_ENUM_FE_3(f, a, b, c) f(a), NEO_ENUM_FE_2(f, b, c)
#define NEO_ENUM_FE_2(f, a, b) f(a), NEO_ENUM_FE_1(f, b)
#define NEO_ENUM_NARG(...) NEO_ENUM_NARG_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define NEO_ENUM_NARG_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, _14, _15, _16, N, ...) N
#define NEO_ENUM_PICK_FE(f, ...) NEO_ENUM_CAT(NEO_ENUM_FE_, NEO_ENUM_NARG(__VA_ARGS__))(f, __VA_ARGS__)

// Defines the EnumSpecs<E> specialization. `TYPE` is the enum; the rest are the
// enumerators in declaration order (index == static_cast<int>(value), contiguous
// from 0). Names are token-stringized, so the enum may be `enum class`. Call it
// at a scope where `neon::scene` is reachable (any namespace; it is fully
// qualified here, so it is safe even from inside neon::scene itself).
#define NEO_ENUM(TYPE, ...)                                                          \
    namespace neon::scene {                                                         \
        template <>                                                                  \
        struct EnumSpecs<TYPE> {                                                     \
            static constexpr bool Enabled = true;                                    \
            using EnumType = TYPE;                                                   \
            static const char** Names() {                                            \
                static const char* s[] = {NEO_ENUM_PICK_FE(NEO_ENUM_STR, __VA_ARGS__)}; \
                return s;                                                            \
            }                                                                        \
            static int Count() {                                                     \
                static const char* s[] = {NEO_ENUM_PICK_FE(NEO_ENUM_STR, __VA_ARGS__)}; \
                return static_cast<int>(sizeof(s) / sizeof(s[0]));                   \
            }                                                                        \
            static const char* ToString(TYPE v) {                                    \
                const char** names = Names();                                        \
                const int idx = static_cast<int>(v);                                 \
                return (idx >= 0 && idx < Count()) ? names[idx] : names[0];          \
            }                                                                        \
            static bool FromString(const char* s, TYPE& out) {                       \
                const char** names = Names();                                        \
                for (int i = 0; i < Count(); ++i)                                    \
                    if (std::strcmp(names[i], s) == 0) {                             \
                        out = static_cast<TYPE>(i);                                  \
                        return true;                                                 \
                    }                                                                \
                out = static_cast<TYPE>(0);                                          \
                return false;                                                        \
            }                                                                        \
        };                                                                           \
    }
