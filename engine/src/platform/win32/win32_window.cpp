#include "neon/platform/window.hpp"

#include <windows.h>
#include <windowsx.h>
#include <imm.h>

#include "neon/core/log.hpp"

namespace neon::platform {
namespace {

constexpr const wchar_t* kWindowClassName = L"NeonEngineWindow";

// The window class is Unicode so WM_CHAR delivers real UTF-16 code units
// (including IME composition results) instead of ANSI/GBK bytes. config.title
// is UTF-8, so convert it when creating the window.
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                        static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(len > 0 ? len : 0), L'\0');
    if (len > 0)
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                            wide.data(), len);
    return wide;
}

// WGL extension constants (defined in wglext.h; kept local to avoid the header).
constexpr int kWglDrawToWindow = 0x2001;
constexpr int kWglSupportOpenGL = 0x2010;
constexpr int kWglDoubleBuffer = 0x2011;
constexpr int kWglPixelType = 0x2013;
constexpr int kWglTypeRgba = 0x202B;
constexpr int kWglColorBits = 0x2014;
constexpr int kWglDepthBits = 0x2022;
constexpr int kWglAcceleration = 0x2003;
constexpr int kWglFullAcceleration = 0x2027;
constexpr int kWglContextMajorVersion = 0x2091;
constexpr int kWglContextMinorVersion = 0x2092;
constexpr int kWglContextProfileMask = 0x9126;
constexpr int kWglContextCoreProfileBit = 0x00000001;

typedef BOOL(APIENTRY* PFN_wglChoosePixelFormatARB)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
typedef HGLRC(APIENTRY* PFN_wglCreateContextAttribsARB)(HDC, HGLRC, const int*);
typedef BOOL(APIENTRY* PFN_wglSwapIntervalEXT)(int);

// Local WGL extension loader (wglGetProcAddress with an opengl32.dll fallback).
// Inlined here so the platform layer never depends on gfx/gl (the GL function
// loader lives in the rendering layer); the window only needs these WGL entry
// points to create its GL context.
void* LoadWglProc(const char* name) {
    void* p = reinterpret_cast<void*>(wglGetProcAddress(name));
    if (!p || p == reinterpret_cast<void*>(1) || p == reinterpret_cast<void*>(2) ||
        p == reinterpret_cast<void*>(3) || p == reinterpret_cast<void*>(-1)) {
        static HMODULE module = GetModuleHandleA("opengl32.dll");
        p = module ? reinterpret_cast<void*>(GetProcAddress(module, name)) : nullptr;
    }
    return p;
}

Key VkToKey(WPARAM vk) {
    if (vk >= 'A' && vk <= 'Z') return static_cast<Key>(static_cast<int>(Key::A) + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return static_cast<Key>(static_cast<int>(Key::D0) + (vk - '0'));
    switch (vk) {
        case VK_SPACE: return Key::Space;
        case VK_RETURN: return Key::Enter;
        case VK_ESCAPE: return Key::Escape;
        case VK_TAB: return Key::Tab;
        case VK_BACK: return Key::Backspace;
        case VK_DELETE: return Key::Delete;
        case VK_SHIFT: return Key::Shift;
        case VK_CONTROL: return Key::Control;
        case VK_MENU: return Key::Alt;
        case VK_UP: return Key::ArrowUp;
        case VK_DOWN: return Key::ArrowDown;
        case VK_LEFT: return Key::ArrowLeft;
        case VK_RIGHT: return Key::ArrowRight;
        case VK_F1: return Key::F1;
        case VK_F2: return Key::F2;
        case VK_F3: return Key::F3;
        case VK_F4: return Key::F4;
        case VK_F5: return Key::F5;
        case VK_F6: return Key::F6;
        case VK_F7: return Key::F7;
        case VK_F8: return Key::F8;
        case VK_F9: return Key::F9;
        case VK_F10: return Key::F10;
        case VK_F11: return Key::F11;
        case VK_F12: return Key::F12;
        default: return Key::Unknown;
    }
}

class Win32Window : public IWindow {
public:
    ~Win32Window() override { Destroy(); }

    bool Create(const WindowConfig& config) override {
        // Enable per-monitor DPI awareness (loaded dynamically for older SDKs).
        typedef BOOL(WINAPI* SetProcessDPIAwareFn)(void);
        static SetProcessDPIAwareFn setDpiAware = reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware"));
        if (setDpiAware) setDpiAware();

        HINSTANCE instance = GetModuleHandle(nullptr);
        WNDCLASSW wc{};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = &Win32Window::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClassName;
        RegisterClassW(&wc);

        RECT rect{0, 0, config.width, config.height};
        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!config.resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        AdjustWindowRect(&rect, style, FALSE);

        const std::wstring titleW = Utf8ToWide(config.title);
        hwnd_ = CreateWindowExW(0, kWindowClassName, titleW.c_str(), style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance, nullptr);
        if (!hwnd_) {
            NEON_LOG_ERROR("Win32: CreateWindowEx failed, error=%lu", GetLastError());
            return false;
        }
        SetWindowLongPtr(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        hdc_ = GetDC(hwnd_);
        if (!CreateGLContext(config)) {
            NEON_LOG_ERROR("Win32: failed to create OpenGL context");
            Destroy();
            return false;
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        GetClientRect(hwnd_, &rect);
        width_ = rect.right - rect.left;
        height_ = rect.bottom - rect.top;
        NEON_LOG_INFO("Win32: window created, client=%dx%d", width_, height_);
        return true;
    }

    void Destroy() override {
        if (glrc_) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(glrc_);
            glrc_ = nullptr;
        }
        if (helperGlrc_) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(helperGlrc_);
            helperGlrc_ = nullptr;
        }
        if (hdc_) {
            ReleaseDC(hwnd_, hdc_);
            hdc_ = nullptr;
        }
        if (helperHdc_) {
            ReleaseDC(helperHwnd_, helperHdc_);
            helperHdc_ = nullptr;
        }
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (helperHwnd_) {
            DestroyWindow(helperHwnd_);
            helperHwnd_ = nullptr;
        }
    }

    void PumpEvents() override {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    void SwapBuffers() override { ::SwapBuffers(hdc_); }
    bool MakeGLContextCurrent() override { return wglMakeCurrent(hdc_, glrc_) == TRUE; }
    bool ShouldClose() const override { return shouldClose_; }
    void RequestClose() override { shouldClose_ = true; }
    int Width() const override { return width_; }
    int Height() const override { return height_; }
    void* NativeHandle() override { return hwnd_; }

    void SetCaptureMouse(bool capture) override {
        capturing_ = capture;
        if (capture) {
            ShowCursor(FALSE);
            RECT rc;
            GetClientRect(hwnd_, &rc);
            POINT center{rc.right / 2, rc.bottom / 2};
            ClientToScreen(hwnd_, &center);
            SetCursorPos(center.x, center.y);
            lastCursor_ = {rc.right / 2, rc.bottom / 2};
            skipNextDelta_ = true;
        } else {
            ShowCursor(TRUE);
            hasLastCursor_ = false; // re-seed on the next move to avoid a jump
        }
    }

    void SetImeEnabled(bool enabled) override {
        // Detaching the IME from the window stops the input method from
        // capturing game keys (WASD/digits/space) during composition, which a
        // Chinese/Japanese/Korean IME in native mode otherwise swallows. The
        // editor re-attaches the saved context when the playtest ends so ImGui
        // text fields keep their IME. Guarded for the IME-free case.
        if (!enabled) {
            if (imeContext_ == nullptr) imeContext_ = ImmGetContext(hwnd_);
            ImmAssociateContext(hwnd_, nullptr);
        } else {
            if (imeContext_ != nullptr) {
                ImmAssociateContext(hwnd_, imeContext_);
                ImmReleaseContext(hwnd_, imeContext_);
                imeContext_ = nullptr;
            }
        }
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        Win32Window* self = reinterpret_cast<Win32Window*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (!self) return DefWindowProc(hwnd, msg, wp, lp);
        return self->HandleMessage(msg, wp, lp);
    }

    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
            case WM_CLOSE:
            case WM_DESTROY:
                shouldClose_ = true;
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_SIZE: {
                int w = LOWORD(lp), h = HIWORD(lp);
                if (w > 0 && h > 0) {
                    width_ = w;
                    height_ = h;
                    if (onEvent) {
                        InputEvent e;
                        e.type = InputEvent::Type::Resize;
                        e.width = w;
                        e.height = h;
                        onEvent(e);
                    }
                }
                return 0;
            }
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                Key key = VkToKey(wp);
                if (key != Key::Unknown && onEvent) {
                    InputEvent e;
                    e.type = InputEvent::Type::KeyDown;
                    e.key = key;
                    onEvent(e);
                }
                return 0;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                Key key = VkToKey(wp);
                if (key != Key::Unknown && onEvent) {
                    InputEvent e;
                    e.type = InputEvent::Type::KeyUp;
                    e.key = key;
                    onEvent(e);
                }
                return 0;
            }
            case WM_CHAR: {
                // With the Unicode window class, wParam is a UTF-16 code unit.
                // BMP characters arrive directly; astral-plane characters
                // arrive as a surrogate pair split across two WM_CHAR messages
                // (keep the high half and combine with the next low half).
                const uint16_t unit = static_cast<uint16_t>(wp);
                if (unit >= 0xD800 && unit <= 0xDBFF) {
                    pendingHighSurrogate_ = unit;
                    return 0;
                }
                unsigned int cp = 0;
                if (unit >= 0xDC00 && unit <= 0xDFFF) {
                    if (pendingHighSurrogate_ == 0) return 0;
                    cp = 0x10000u + (static_cast<unsigned int>(pendingHighSurrogate_ - 0xD800u) << 10) +
                         static_cast<unsigned int>(unit - 0xDC00u);
                    pendingHighSurrogate_ = 0;
                } else {
                    pendingHighSurrogate_ = 0;
                    cp = unit;
                }
                char utf8[8] = {};
                if (cp < 0x80) {
                    utf8[0] = static_cast<char>(cp);
                } else if (cp < 0x800) {
                    utf8[0] = static_cast<char>(0xC0 | (cp >> 6));
                    utf8[1] = static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    utf8[0] = static_cast<char>(0xE0 | (cp >> 12));
                    utf8[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8[2] = static_cast<char>(0x80 | (cp & 0x3F));
                } else if (cp < 0x110000) {
                    utf8[0] = static_cast<char>(0xF0 | (cp >> 18));
                    utf8[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    utf8[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    utf8[3] = static_cast<char>(0x80 | (cp & 0x3F));
                }
                if (onEvent && utf8[0] != '\0') {
                    InputEvent e;
                    e.type = InputEvent::Type::TextInput;
                    e.text = utf8;
                    onEvent(e);
                }
                return 0;
            }
            case WM_MOUSEMOVE: {
                int x = GET_X_LPARAM(lp);
                int y = GET_Y_LPARAM(lp);
                int dx = 0, dy = 0;
                if (capturing_) {
                    if (!skipNextDelta_) {
                        dx = x - lastCursor_.x;
                        dy = y - lastCursor_.y;
                    }
                    skipNextDelta_ = false;
                    RECT rc;
                    GetClientRect(hwnd_, &rc);
                    POINT center{rc.right / 2, rc.bottom / 2};
                    ClientToScreen(hwnd_, &center);
                    SetCursorPos(center.x, center.y);
                    lastCursor_ = {rc.right / 2, rc.bottom / 2};
                } else {
                    // Editor-style relative tracking: report the movement since
                    // the previous position even without capture so viewport
                    // orbit/pan (which read MouseDelta) respond to mouse drags.
                    if (hasLastCursor_) {
                        dx = x - lastCursor_.x;
                        dy = y - lastCursor_.y;
                    }
                    lastCursor_ = {x, y};
                    hasLastCursor_ = true;
                }
                if (onEvent) {
                    InputEvent e;
                    e.type = InputEvent::Type::MouseMove;
                    e.x = x;
                    e.y = y;
                    e.dx = dx;
                    e.dy = dy;
                    onEvent(e);
                }
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN: {
                if (onEvent) {
                    InputEvent e;
                    e.type = InputEvent::Type::MouseDown;
                    e.button = msg == WM_LBUTTONDOWN ? MouseButton::Left
                                : msg == WM_RBUTTONDOWN ? MouseButton::Right
                                                        : MouseButton::Middle;
                    onEvent(e);
                }
                return 0;
            }
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP: {
                if (onEvent) {
                    InputEvent e;
                    e.type = InputEvent::Type::MouseUp;
                    e.button = msg == WM_LBUTTONUP ? MouseButton::Left
                                : msg == WM_RBUTTONUP ? MouseButton::Right
                                                      : MouseButton::Middle;
                    onEvent(e);
                }
                return 0;
            }
            case WM_MOUSEWHEEL: {
                if (onEvent) {
                    InputEvent e;
                    e.type = InputEvent::Type::MouseWheel;
                    e.wheel = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
                    onEvent(e);
                }
                return 0;
            }
            default:
                return DefWindowProc(hwnd_, msg, wp, lp);
        }
    }

    bool CreateGLContext(const WindowConfig& config) {
        // 1. Dummy context on a hidden helper window to load WGL extensions.
        HINSTANCE instance = GetModuleHandle(nullptr);
        helperHwnd_ = CreateWindowExW(0, kWindowClassName, L"", WS_OVERLAPPED,
                                      0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
        if (!helperHwnd_) return false;
        helperHdc_ = GetDC(helperHwnd_);

        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 24;
        pfd.cDepthBits = 24;
        pfd.iLayerType = PFD_MAIN_PLANE;

        int pf = ChoosePixelFormat(helperHdc_, &pfd);
        if (!pf || !SetPixelFormat(helperHdc_, pf, &pfd)) return false;
        helperGlrc_ = wglCreateContext(helperHdc_);
        if (!helperGlrc_) return false;
        wglMakeCurrent(helperHdc_, helperGlrc_);

        auto wglChoosePixelFormatARB =
            reinterpret_cast<PFN_wglChoosePixelFormatARB>(LoadWglProc("wglChoosePixelFormatARB"));
        auto wglCreateContextAttribsARB =
            reinterpret_cast<PFN_wglCreateContextAttribsARB>(LoadWglProc("wglCreateContextAttribsARB"));
        auto wglSwapIntervalEXT =
            reinterpret_cast<PFN_wglSwapIntervalEXT>(LoadWglProc("wglSwapIntervalEXT"));

        if (!wglChoosePixelFormatARB || !wglCreateContextAttribsARB) {
            NEON_LOG_ERROR("Win32: WGL_ARB_pixel_format/context extensions unavailable");
            return false;
        }

        // 2. Choose a modern pixel format for the real window.
        const int pixelAttribs[] = {
            kWglDrawToWindow, 1,
            kWglSupportOpenGL, 1,
            kWglDoubleBuffer, 1,
            kWglPixelType, kWglTypeRgba,
            kWglColorBits, 24,
            kWglDepthBits, 24,
            kWglAcceleration, kWglFullAcceleration,
            0};
        UINT formatCount = 0;
        int format = 0;
        bool arbChosen =
            wglChoosePixelFormatARB(hdc_, pixelAttribs, nullptr, 1, &format, &formatCount) &&
            formatCount > 0;

        PIXELFORMATDESCRIPTOR chosen{};
        if (arbChosen) {
            DescribePixelFormat(hdc_, format, sizeof(chosen), &chosen);
            NEON_LOG_INFO("Win32: ARB pixel format %d (depth=%d)", format, chosen.cDepthBits);
        }
        if (!arbChosen || chosen.cDepthBits < 24) {
            // Fall back to a standard GDI pixel format with a depth buffer.
            PIXELFORMATDESCRIPTOR pfdFallback{};
            pfdFallback.nSize = sizeof(pfdFallback);
            pfdFallback.nVersion = 1;
            pfdFallback.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            pfdFallback.iPixelType = PFD_TYPE_RGBA;
            pfdFallback.cColorBits = 24;
            pfdFallback.cDepthBits = 24;
            pfdFallback.iLayerType = PFD_MAIN_PLANE;
            int legacyFormat = ChoosePixelFormat(hdc_, &pfdFallback);
            if (legacyFormat == 0) {
                NEON_LOG_ERROR("Win32: no pixel format with depth buffer");
                return false;
            }
            format = legacyFormat;
            DescribePixelFormat(hdc_, format, sizeof(chosen), &chosen);
            NEON_LOG_INFO("Win32: legacy pixel format %d (depth=%d)", format, chosen.cDepthBits);
        }
        if (format == 0) {
            NEON_LOG_ERROR("Win32: no suitable pixel format");
            return false;
        }

        if (!SetPixelFormat(hdc_, format, &chosen)) {
            NEON_LOG_ERROR("Win32: SetPixelFormat failed, error=%lu", GetLastError());
            return false;
        }

        // 3. Create the 3.x core context.
        // Try the requested version, then fall back to 3.3, then to a legacy context.
        struct VersionAttempt {
            int major;
            int minor;
        } attempts[] = {
            {config.glMajor, config.glMinor},
            {3, 3},
            {2, 1},
        };

        // Some drivers report depth in the pixel format but expose a depth-less
        // context for attrib-created (core) contexts. Validate GL_DEPTH_BITS
        // per attempt and fall back until we get a real depth buffer.
        typedef void(WINAPI* GetIntegervFn)(unsigned int, int*);
        static GetIntegervFn rawGetIntegerv = reinterpret_cast<GetIntegervFn>(
            GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetIntegerv"));

        for (const VersionAttempt& attempt : attempts) {
            if (attempt.major < 3) {
                glrc_ = wglCreateContext(hdc_);
            } else {
                const int contextAttribs[] = {
                    kWglContextMajorVersion, attempt.major,
                    kWglContextMinorVersion, attempt.minor,
                    kWglContextProfileMask, kWglContextCoreProfileBit,
                    0};
                glrc_ = wglCreateContextAttribsARB(hdc_, nullptr, contextAttribs);
            }
            if (!glrc_) continue;

            wglMakeCurrent(hdc_, glrc_);
            int depthBits = 0;
            if (rawGetIntegerv) rawGetIntegerv(0x0D56, &depthBits);
            NEON_LOG_INFO("Win32: context attempt %d.%d depth-bits=%d",
                          attempt.major, attempt.minor, depthBits);
            if (depthBits >= 16) break;

            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(glrc_);
            glrc_ = nullptr;
        }
        if (!glrc_) {
            NEON_LOG_ERROR("Win32: failed to create GL %d.%d context, error=%lu",
                           config.glMajor, config.glMinor, GetLastError());
            return false;
        }
        wglMakeCurrent(hdc_, glrc_);
        if (wglSwapIntervalEXT) wglSwapIntervalEXT(config.vsync ? 1 : 0);

        // 4. Tear down the helper context.
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(helperGlrc_);
        helperGlrc_ = nullptr;
        ReleaseDC(helperHwnd_, helperHdc_);
        helperHdc_ = nullptr;
        DestroyWindow(helperHwnd_);
        helperHwnd_ = nullptr;
        return true;
    }

    HWND hwnd_ = nullptr;
    HDC hdc_ = nullptr;
    HGLRC glrc_ = nullptr;
    HWND helperHwnd_ = nullptr;
    HDC helperHdc_ = nullptr;
    HGLRC helperGlrc_ = nullptr;
    int width_ = 1280;
    int height_ = 720;
    bool shouldClose_ = false;
    bool capturing_ = false;
    POINT lastCursor_{};
    bool hasLastCursor_ = false; // first WM_MOUSEMOVE seeds lastCursor_ (no delta)
    bool skipNextDelta_ = false;
    HIMC imeContext_ = nullptr; // saved IME context while detached (SetImeEnabled)
    uint16_t pendingHighSurrogate_ = 0; // high half of an in-progress astral char
};

} // namespace

std::unique_ptr<IWindow> CreatePlatformWindow() {
    return std::make_unique<Win32Window>();
}

} // namespace neon::platform
