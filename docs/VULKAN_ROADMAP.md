# Vulkan 后端实现路线

`IRenderBackend` 接口已冻结，Vulkan 后端只需填充该接口。当前 `engine/src/gfx/vulkan/vk_backend.cpp` 为占位实现（`Init` 返回 false）。

## 步骤

1. **实例与设备**
   - `vkCreateInstance`（开启 `VK_KHR_surface`/`VK_KHR_win32_surface` 或对应平台扩展）。
   - 枚举物理设备，选离散 GPU；创建队列（graphics + present）。
   - 使用 Vulkan-Headers（Khronos 官方头，vendored）与 `volk` 或自写加载器。

2. **表面与交换链**
   - 平台表面创建（Win32：`vkCreateWin32SurfaceKHR`；X11：`VkXlibSurfaceCreateInfoKHR`；macOS：`VK_EXT_metal_surface`）。
   - 交换链：选择格式（SRGB）、呈现模式（FIFO=vsync）、图像数量 2~3。

3. **渲染管线**
   - RenderPass：颜色 + 深度附件；Subpass 1 个（3D），后续加 UI subpass。
   - Pipeline：把现有 GLSL 330 shader 移植为 SPIR-V（`glslangValidator` 或 `shaderc`），顶点输入与现有 `Vertex3D` 布局一致。

4. **资源**
   - 缓冲：`VkBuffer` + `VkDeviceMemory`，动态 uniform/staging。
   - 纹理：`vkCreateImage` + 传输队列上传（staging buffer），采样器抽象。
   - Descriptor：每帧 1 组（MVP/材质/贴图），材质变化触发重绑。

5. **帧同步**
   - 每帧：acquire → submit（command buffer）→ present；semaphore/fence 环形。
   - 渲染目标：现有“直接画到默认帧缓冲”改为交换链图像，UI 后置到独立 RenderPass。

6. **验证**
   - Debug messenger + validation layers（Debug 构建）。
   - 用 `--smoke-test --screenshot` 与现有 OpenGL 后端做像素级对比。

## 兼容性策略

- 先支持 Windows，再按平台条件编译 surface 扩展。
- `IRenderBackend` 中与 OpenGL 相关的假设（如立即模式绘制）已在接口层统一为显式提交，Vulkan 实现可在内部做命令缓冲累积。
