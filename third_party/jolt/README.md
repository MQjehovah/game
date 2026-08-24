# JoltPhysics (vendored)

Vendored from [jrouwe/JoltPhysics](https://github.com/jrouwe/JoltPhysics)
tag **v5.0.0** (MIT licensed, see LICENSE). Only the `Jolt/` library sources
are included - no samples/tests/build scripts.

Why vendored instead of FetchContent: the project supports offline / CI builds
and the old MinGW 8.1 toolchain cannot compile modern Jolt, so the engine now
requires GCC >= 9 (or MSVC/Clang). See `docs/godot-gap-analysis.md` P0-1.

The engine integrates it as a static library `neon_jolt` (see CMakeLists.txt)
behind `NEON_ENABLE_JOLT`, keeping the deterministic custom `physics::World`
as the server fallback.
