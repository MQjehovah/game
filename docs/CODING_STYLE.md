# 代码规范

## 语言与构建

- C++17；禁止 C++20 特性（工具链兼容）。
- CMake ≥ 3.15；新文件必须加入 `CMakeLists.txt`。
- 第三方依赖必须 vendored（`third_party/`）并附许可证。

## 命名

- 命名空间：`neon::math`、`neon::core`、`neon::ecs`、`neon::platform`、`neon::gfx`、`neon::audio`、`neon::physics`、`neon::assets`、`neon::scene`、`neon::ui`。
- 类型/类：`PascalCase`（`GameObject`、`RenderBackend`）。
- 函数/方法：`PascalCase`（`CreateTexture`、`ViewAll`）。
- 变量/参数：`camelCase`（`frameIndex`、`camDist`）。
- 成员变量：`camelCase` + 尾下划线（`screenW_`）。
- 常量：`k` 前缀（`kMaxPointLights`）。
- 宏：`NEON_` 前缀（`NEON_LOG_INFO`）。
- 头文件：`.hpp`；实现：`.cpp`；Objective-C++：`.mm`。

## 头文件

- 每个头文件自包含（包含它依赖的头），`#pragma once`。
- 接口头（`neon/platform/*.hpp` 等）禁止包含平台/GL 头文件。
- 优先传 `const T&`；接口类用纯虚 + 工厂函数返回 `std::unique_ptr`。

## 错误处理

- 返回值/日志优先；`core::Result<T>` 用于可恢复的显式结果。
- 日志分级：Debug/Info/Warn/Error，禁止静默吞错。
- 资源创建失败必须返回无效句柄并 `NEON_LOG_ERROR`。

## 数学约定

- `Mat4` 一律**行主序**；提交给 GL 时 `transpose=GL_TRUE`。
- 角度一律弧度；世界坐标 Y 轴向上；相机看向 -Z。
- 涉及矩阵的修改必须补充单元测试（防约定回归）。

## 分层纪律

- 游戏层不得包含 `#include <windows.h>`、GL/X11/Cocoa 头。
- 平台后端只在对应 OS 编译（CMake 按 `WIN32/APPLE/UNIX` 分支）。
- 新增跨平台能力先加接口，再逐个平台实现。

## 提交前检查

- `cmake --build build -j` 无警告（`-Wall -Wextra`）。
- `neon_tests` 全绿。
- Windows 冒烟：`neon_rush --smoke-test 240` 退出码 0。
