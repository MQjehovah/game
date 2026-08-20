#include "neon/platform/window.hpp"

#import <Cocoa/Cocoa.h>
#import <OpenGL/OpenGL.h>

#include "neon/core/log.hpp"

// Cocoa / NSOpenGL backend. Compiled on macOS; validated in CI.

@interface NeonOpenGLView : NSOpenGLView
@end

@implementation NeonOpenGLView
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect; // frames are rendered by the engine's own loop
}
@end

@interface NeonWindowDelegate : NSObject <NSWindowDelegate> {
@public
    bool* closeFlag;
}
@end

@implementation NeonWindowDelegate
- (BOOL)windowShouldClose:(id)sender {
    (void)sender;
    if (closeFlag) *closeFlag = true;
    return NO;
}
@end

namespace neon::platform {
namespace {

Key CocoaKeyToEngine(unsigned short keyCode) {
    switch (keyCode) {
        case 0: return Key::A;
        case 1: return Key::S;
        case 2: return Key::D;
        case 3: return Key::F;
        case 4: return Key::H;
        case 5: return Key::G;
        case 6: return Key::Z;
        case 7: return Key::X;
        case 8: return Key::C;
        case 9: return Key::V;
        case 11: return Key::B;
        case 12: return Key::Q;
        case 13: return Key::W;
        case 14: return Key::E;
        case 15: return Key::R;
        case 16: return Key::Y;
        case 17: return Key::T;
        case 18: return Key::D1;
        case 19: return Key::D2;
        case 20: return Key::D3;
        case 21: return Key::D4;
        case 22: return Key::D6;
        case 23: return Key::D5;
        case 24: return Key::D7;
        case 25: return Key::D8;
        case 26: return Key::D9;
        case 27: return Key::D0;
        case 28: return Key::Enter;
        case 29: return Key::Control;
        case 30: return Key::Unknown; // ]
        case 31: return Key::Unknown; // [
        case 32: return Key::Unknown; // ~
        case 33: return Key::Unknown; // ;
        case 34: return Key::Unknown; // '
        case 35: return Key::Unknown; // `
        case 36: return Key::Unknown; // ,
        case 37: return Key::Unknown; // .
        case 38: return Key::Unknown; // /
        case 39: return Key::Unknown; // =
        case 40: return Key::Enter;
        case 41: return Key::Escape;
        case 42: return Key::Backspace;
        case 43: return Key::Tab;
        case 44: return Key::Space;
        case 45: return Key::Unknown; // -
        case 46: return Key::Unknown; // =
        case 47: return Key::Unknown; // [
        case 48: return Key::Unknown; // ]
        case 49: return Key::Unknown; // \
        case 50: return Key::Unknown; // non-US #
        case 51: return Key::Unknown; // ;
        case 52: return Key::Unknown; // '
        case 53: return Key::Unknown; // `
        case 54: return Key::Unknown; // ,
        case 55: return Key::Unknown; // .
        case 56: return Key::Unknown; // /
        case 57: return Key::Unknown; // Caps
        case 58: return Key::Alt;
        case 59: return Key::Control;
        case 60: return Key::Shift;
        case 61: return Key::Alt;
        case 62: return Key::Control;
        case 63: return Key::Shift;
        case 65: return Key::Unknown; // .
        case 67: return Key::Unknown; // *
        case 75: return Key::Unknown; // /
        case 78: return Key::Unknown; // -
        case 79: return Key::Unknown; // +
        case 81: return Key::Unknown; // =
        case 82: return Key::D0;
        case 83: return Key::D1;
        case 84: return Key::D2;
        case 85: return Key::D3;
        case 86: return Key::D4;
        case 87: return Key::D5;
        case 88: return Key::D6;
        case 89: return Key::D7;
        case 91: return Key::D8;
        case 92: return Key::D9;
        case 96: return Key::F5;
        case 97: return Key::F6;
        case 98: return Key::F7;
        case 99: return Key::F3;
        case 100: return Key::F8;
        case 101: return Key::F9;
        case 103: return Key::F11;
        case 105: return Key::F13;
        case 106: return Key::F16;
        case 107: return Key::F14;
        case 109: return Key::F10;
        case 111: return Key::F12;
        case 113: return Key::F15;
        case 114: return Key::Unknown; // Help
        case 115: return Key::ArrowUp;
        case 116: return Key::ArrowRight;
        case 117: return Key::ArrowDown;
        case 118: return Key::ArrowLeft;
        case 120: return Key::F2;
        case 122: return Key::F1;
        case 123: return Key::ArrowLeft;
        case 124: return Key::ArrowRight;
        case 125: return Key::ArrowDown;
        case 126: return Key::ArrowUp;
        default: return Key::Unknown;
    }
}

class CocoaWindow : public IWindow {
public:
    ~CocoaWindow() override { Destroy(); }

    bool Create(const WindowConfig& config) override {
        __block bool ok = false;
        @autoreleasepool {
            [NSApplication sharedApplication];
            NSRect frame = NSMakeRect(0, 0, config.width, config.height);
            NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                               NSWindowStyleMaskMiniaturizable;
            if (config.resizable) style |= NSWindowStyleMaskResizable;
            window_ = [[NSWindow alloc] initWithContentRect:frame
                                                  styleMask:style
                                                    backing:NSBackingStoreBuffered
                                                      defer:NO];
            [window_ setTitle:[NSString stringWithUTF8String:config.title.c_str()]];

            NSOpenGLPixelFormatAttribute attrs[] = {
                NSOpenGLPFAOpenGLProfile, NSOpenGLProfileVersion3_2Core,
                NSOpenGLPFAColorSize, 24,
                NSOpenGLPFADepthSize, 24,
                NSOpenGLPFADoubleBuffer,
                NSOpenGLPFAAccelerated,
                0};
            NSOpenGLPixelFormat* pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:attrs];
            if (!pixelFormat) {
                NSOpenGLPixelFormatAttribute fallbackAttrs[] = {
                    NSOpenGLPFAColorSize, 24,
                    NSOpenGLPFADepthSize, 24,
                    NSOpenGLPFADoubleBuffer,
                    0};
                pixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:fallbackAttrs];
            }
            if (!pixelFormat) {
                NEON_LOG_ERROR("Cocoa: no OpenGL pixel format");
                return false;
            }
            view_ = [[NeonOpenGLView alloc] initWithFrame:frame pixelFormat:pixelFormat];
            [window_ setContentView:view_];
            [window_ makeKeyAndOrderFront:nil];
            [window_ setAcceptsMouseMovedEvents:YES];

            delegate_ = [[NeonWindowDelegate alloc] init];
            delegate_->closeFlag = &shouldClose_;
            [window_ setDelegate:delegate_];
            [window_ center];
            ok = true;
        }
        width_ = config.width;
        height_ = config.height;
        NEON_LOG_INFO("Cocoa: window created (%dx%d)", width_, height_);
        return ok;
    }

    void Destroy() override {
        @autoreleasepool {
            if (window_) {
                [window_ close];
                window_ = nil;
            }
            view_ = nil;
            delegate_ = nil;
        }
    }

    void PumpEvents() override {
        @autoreleasepool {
            NSEvent* event = nil;
            while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:[NSDate distantPast]
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES])) {
                [NSApp sendEvent:event];
                NSEventType type = [event type];
                if (type == NSEventTypeKeyDown || type == NSEventTypeKeyUp) {
                    Key key = CocoaKeyToEngine([event keyCode]);
                    if (key != Key::Unknown && onEvent) {
                        InputEvent e;
                        e.type = type == NSEventTypeKeyDown ? InputEvent::Type::KeyDown
                                                            : InputEvent::Type::KeyUp;
                        e.key = key;
                        onEvent(e);
                    }
                } else if (type == NSEventTypeMouseMoved || type == NSEventTypeLeftMouseDragged ||
                           type == NSEventTypeRightMouseDragged || type == NSEventTypeOtherMouseDragged) {
                    NSPoint p = [event locationInWindow];
                    if (onEvent) {
                        InputEvent e;
                        e.type = InputEvent::Type::MouseMove;
                        e.x = static_cast<int>(p.x);
                        e.y = static_cast<int>(height_ - p.y);
                        e.dx = static_cast<int>([event deltaX]);
                        e.dy = static_cast<int>([event deltaY]);
                        onEvent(e);
                    }
                } else if (type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown ||
                           type == NSEventTypeOtherMouseDown ||
                           type == NSEventTypeLeftMouseUp || type == NSEventTypeRightMouseUp ||
                           type == NSEventTypeOtherMouseUp) {
                    bool down = type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown ||
                                type == NSEventTypeOtherMouseDown;
                    MouseButton button = MouseButton::Middle;
                    if (type == NSEventTypeLeftMouseDown || type == NSEventTypeLeftMouseUp) {
                        button = MouseButton::Left;
                    } else if (type == NSEventTypeRightMouseDown || type == NSEventTypeRightMouseUp) {
                        button = MouseButton::Right;
                    }
                    if (onEvent) {
                        InputEvent e;
                        e.type = down ? InputEvent::Type::MouseDown : InputEvent::Type::MouseUp;
                        e.button = button;
                        onEvent(e);
                    }
                } else if (type == NSEventTypeScrollWheel) {
                    if (onEvent) {
                        InputEvent e;
                        e.type = InputEvent::Type::MouseWheel;
                        e.wheel = [event scrollingDeltaY] > 0.0 ? 1 : ([event scrollingDeltaY] < 0.0 ? -1 : 0);
                        onEvent(e);
                    }
                }
            }
        }
    }

    void SwapBuffers() override {
        @autoreleasepool {
            [[view_ openGLContext] flushBuffer];
        }
    }

    bool MakeGLContextCurrent() override {
        @autoreleasepool {
            [[view_ openGLContext] makeCurrentContext];
            return true;
        }
    }

    bool ShouldClose() const override { return shouldClose_; }
    void RequestClose() override { shouldClose_ = true; }
    int Width() const override { return width_; }
    int Height() const override { return height_; }

    void SetCaptureMouse(bool capture) override {
        if (capture) {
            [NSCursor hide];
        } else {
            [NSCursor unhide];
        }
    }

private:
    NSWindow* window_ = nil;
    NSOpenGLView* view_ = nil;
    NeonWindowDelegate* delegate_ = nil;
    int width_ = 1280;
    int height_ = 720;
    bool shouldClose_ = false;
};

} // namespace

std::unique_ptr<IWindow> CreatePlatformWindow() {
    return std::make_unique<CocoaWindow>();
}

} // namespace neon::platform
