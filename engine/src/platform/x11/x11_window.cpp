#include "neon/platform/window.hpp"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <GL/glx.h>

#include <cstring>

#include "neon/core/log.hpp"

// X11 / GLX window backend. Compiled on Linux; validated in CI.
namespace neon::platform {
namespace {

typedef GLXContext (*PFN_glXCreateContextAttribsARB)(Display*, GLXFBConfig, GLXContext, Bool, const int*);
typedef Bool (*PFN_glXMakeContextCurrent)(Display*, GLXDrawable, GLXDrawable, GLXContext);

Key X11KeyToEngine(KeySym sym) {
    if (sym >= XK_a && sym <= XK_z) return static_cast<Key>(static_cast<int>(Key::A) + (sym - XK_a));
    if (sym >= XK_0 && sym <= XK_9) return static_cast<Key>(static_cast<int>(Key::D0) + (sym - XK_0));
    switch (sym) {
        case XK_space: return Key::Space;
        case XK_Return: return Key::Enter;
        case XK_Escape: return Key::Escape;
        case XK_Tab: return Key::Tab;
        case XK_BackSpace: return Key::Backspace;
        case XK_Delete: return Key::Delete;
        case XK_Shift_L:
        case XK_Shift_R: return Key::Shift;
        case XK_Control_L:
        case XK_Control_R: return Key::Control;
        case XK_Alt_L:
        case XK_Alt_R: return Key::Alt;
        case XK_Up: return Key::ArrowUp;
        case XK_Down: return Key::ArrowDown;
        case XK_Left: return Key::ArrowLeft;
        case XK_Right: return Key::ArrowRight;
        case XK_F1: return Key::F1;
        case XK_F2: return Key::F2;
        case XK_F3: return Key::F3;
        case XK_F4: return Key::F4;
        case XK_F5: return Key::F5;
        case XK_F6: return Key::F6;
        case XK_F7: return Key::F7;
        case XK_F8: return Key::F8;
        case XK_F9: return Key::F9;
        case XK_F10: return Key::F10;
        case XK_F11: return Key::F11;
        case XK_F12: return Key::F12;
        default: return Key::Unknown;
    }
}

class X11Window : public IWindow {
public:
    ~X11Window() override { Destroy(); }

    bool Create(const WindowConfig& config) override {
        display_ = XOpenDisplay(nullptr);
        if (!display_) {
            NEON_LOG_ERROR("X11: cannot open display");
            return false;
        }
        screen_ = DefaultScreen(display_);
        root_ = RootWindow(display_, screen_);

        int fbAttribs[] = {
            GLX_X_RENDERABLE, True,
            GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
            GLX_RENDER_TYPE, GLX_RGBA_BIT,
            GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
            GLX_RED_SIZE, 8,
            GLX_GREEN_SIZE, 8,
            GLX_BLUE_SIZE, 8,
            GLX_ALPHA_SIZE, 8,
            GLX_DEPTH_SIZE, 24,
            GLX_DOUBLEBUFFER, True,
            None};
        int fbCount = 0;
        GLXFBConfig* fbConfigs = glXChooseFBConfig(display_, screen_, fbAttribs, &fbCount);
        if (!fbConfigs || fbCount == 0) {
            NEON_LOG_ERROR("X11: no suitable GLX framebuffer config");
            return false;
        }
        fbConfig_ = fbConfigs[0];
        XFree(fbConfigs);

        XVisualInfo* visual = glXGetVisualFromFBConfig(display_, fbConfig_);
        if (!visual) {
            NEON_LOG_ERROR("X11: glXGetVisualFromFBConfig failed");
            return false;
        }

        XSetWindowAttributes attrs{};
        attrs.colormap = XCreateColormap(display_, root_, visual->visual, AllocNone);
        attrs.border_pixel = 0;
        attrs.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                           ButtonReleaseMask | PointerMotionMask | StructureNotifyMask;
        window_ = XCreateWindow(display_, root_, 0, 0, config.width, config.height, 0,
                                visual->depth, InputOutput, visual->visual,
                                CWColormap | CWBorderPixel | CWEventMask, &attrs);
        XFree(visual);
        if (!window_) {
            NEON_LOG_ERROR("X11: XCreateWindow failed");
            return false;
        }

        XStoreName(display_, window_, config.title.c_str());
        wmDeleteMessage_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display_, window_, &wmDeleteMessage_, 1);
        XMapWindow(display_, window_);
        XFlush(display_);

        // GL context (3.3 core if available, otherwise compatibility).
        PFN_glXCreateContextAttribsARB createAttribs =
            reinterpret_cast<PFN_glXCreateContextAttribsARB>(
                glXGetProcAddressARB(reinterpret_cast<const GLubyte*>("glXCreateContextAttribsARB")));
        if (createAttribs) {
            int contextAttribs[] = {
                GLX_CONTEXT_MAJOR_VERSION_ARB, config.glMajor,
                GLX_CONTEXT_MINOR_VERSION_ARB, config.glMinor,
                GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
                None};
            context_ = createAttribs(display_, fbConfig_, nullptr, True, contextAttribs);
        }
        if (!context_) context_ = glXCreateNewContext(display_, fbConfig_, GLX_RGBA_TYPE, nullptr, True);
        if (!context_) {
            NEON_LOG_ERROR("X11: failed to create GLX context");
            return false;
        }

        width_ = config.width;
        height_ = config.height;
        NEON_LOG_INFO("X11: window created (%dx%d)", width_, height_);
        return true;
    }

    void Destroy() override {
        if (display_) {
            if (context_) {
                glXMakeCurrent(display_, None, nullptr);
                glXDestroyContext(display_, context_);
                context_ = nullptr;
            }
            if (window_) {
                XDestroyWindow(display_, window_);
                window_ = 0;
            }
            XCloseDisplay(display_);
            display_ = nullptr;
        }
    }

    void PumpEvents() override {
        while (XPending(display_) > 0) {
            XEvent event;
            XNextEvent(display_, &event);
            switch (event.type) {
                case ClientMessage:
                    if (static_cast<Atom>(event.xclient.data.l[0]) == wmDeleteMessage_) shouldClose_ = true;
                    break;
                case ConfigureNotify:
                    if (event.xconfigure.width > 0 && event.xconfigure.height > 0) {
                        width_ = event.xconfigure.width;
                        height_ = event.xconfigure.height;
                        if (onEvent) {
                            InputEvent e;
                            e.type = InputEvent::Type::Resize;
                            e.width = width_;
                            e.height = height_;
                            onEvent(e);
                        }
                    }
                    break;
                case KeyPress:
                case KeyRelease: {
                    KeySym sym = XLookupKeysym(&event.xkey, 0);
                    Key key = X11KeyToEngine(sym);
                    if (key != Key::Unknown && onEvent) {
                        InputEvent e;
                        e.type = event.type == KeyPress ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
                        e.key = key;
                        onEvent(e);
                    }
                    if (event.type == KeyPress && onEvent) {
                        char buf[8] = {};
                        KeySym ignored = 0;
                        int len = XLookupString(&event.xkey, buf, sizeof(buf), &ignored, nullptr);
                        if (len > 0) {
                            InputEvent e;
                            e.type = InputEvent::Type::TextInput;
                            e.text.assign(buf, static_cast<size_t>(len));
                            onEvent(e);
                        }
                    }
                    break;
                }
                case MotionNotify: {
                    int x = event.xmotion.x;
                    int y = event.xmotion.y;
                    int dx = 0, dy = 0;
                    if (capturing_) {
                        dx = x - centerX_;
                        dy = y - centerY_;
                        XWarpPointer(display_, None, window_, 0, 0, 0, 0, centerX_, centerY_);
                        XFlush(display_);
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
                    break;
                }
                case ButtonPress:
                case ButtonRelease: {
                    MouseButton button = MouseButton::Middle;
                    if (event.xbutton.button == 1) button = MouseButton::Left;
                    else if (event.xbutton.button == 2) button = MouseButton::Middle;
                    else if (event.xbutton.button == 3) button = MouseButton::Right;
                    if (onEvent) {
                        InputEvent e;
                        if (event.xbutton.button == 4 || event.xbutton.button == 5) {
                            e.type = InputEvent::Type::MouseWheel;
                            e.wheel = event.xbutton.button == 4 ? 1 : -1;
                        } else {
                            e.type = event.type == ButtonPress ? InputEvent::Type::MouseDown
                                                               : InputEvent::Type::MouseUp;
                            e.button = button;
                        }
                        onEvent(e);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }

    void SwapBuffers() override { glXSwapBuffers(display_, window_); }
    bool MakeGLContextCurrent() override {
        return glXMakeCurrent(display_, window_, context_) == True;
    }
    bool ShouldClose() const override { return shouldClose_; }
    void RequestClose() override { shouldClose_ = true; }
    int Width() const override { return width_; }
    int Height() const override { return height_; }

    void SetCaptureMouse(bool capture) override {
        capturing_ = capture;
        if (capture) {
            centerX_ = width_ / 2;
            centerY_ = height_ / 2;
            XWarpPointer(display_, None, window_, 0, 0, 0, 0, centerX_, centerY_);
            XFlush(display_);
        }
    }

private:
    Display* display_ = nullptr;
    int screen_ = 0;
    Window root_ = 0;
    Window window_ = 0;
    GLXFBConfig fbConfig_ = nullptr;
    GLXContext context_ = nullptr;
    Atom wmDeleteMessage_ = 0;
    int width_ = 1280;
    int height_ = 720;
    bool shouldClose_ = false;
    bool capturing_ = false;
    int centerX_ = 0;
    int centerY_ = 0;
};

} // namespace

std::unique_ptr<IWindow> CreatePlatformWindow() {
    return std::make_unique<X11Window>();
}

} // namespace neon::platform
