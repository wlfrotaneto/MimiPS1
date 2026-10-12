#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#include "mimips1_runtime.h"

extern bool macos_audio_init(PS1Context* ctx);
extern void macos_audio_shutdown(void);

@interface MimiPS1ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithContext:(PS1Context*)ctx device:(id<MTLDevice>)device;
@end

@interface MimiPS1WindowController : NSWindowController <NSWindowDelegate>
@property (nonatomic, assign) PS1Context* context;
@property (nonatomic, strong) MTKView* mtkView;
@property (nonatomic, strong) MimiPS1ViewDelegate* viewDelegate;
@end

@implementation MimiPS1WindowController

- (void)setupControllerEvents {
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification
                                                      object:nil
                                                       queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification *note) {
        GCController *controller = note.object;
        NSLog(@"[Gamepad] Connected controller: %@", controller.vendorName);
        if (controller.extendedGamepad) {
            controller.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
                if (!self.context) return;
                PS1Input* input = &self.context->input;
                ps1_input_set_button(input, 0, PS1_BTN_CROSS, gamepad.buttonA.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_CIRCLE, gamepad.buttonB.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_SQUARE, gamepad.buttonX.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_TRIANGLE, gamepad.buttonY.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_UP, gamepad.dpad.up.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_DOWN, gamepad.dpad.down.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_LEFT, gamepad.dpad.left.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_RIGHT, gamepad.dpad.right.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_L1, gamepad.leftShoulder.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_R1, gamepad.rightShoulder.isPressed);
                ps1_input_set_button(input, 0, PS1_BTN_START, gamepad.buttonMenu.isPressed);
            };
        }
    }];
}

- (void)windowWillClose:(NSNotification *)notification {
    [NSApp terminate:nil];
}

- (void)keyDown:(NSEvent *)event {
    if (!_context) return;
    if (event.modifierFlags & NSEventModifierFlagCommand) {
        if (event.keyCode == 3) { // Cmd+F: Toggle Full Screen
            [self.window toggleFullScreen:nil];
            return;
        } else if (event.keyCode == 1) { // Cmd+S: Toggle CRT Scanlines
            _context->crt_scanlines = !_context->crt_scanlines;
            return;
        }
    }
    if (event.keyCode == 53) { // Escape
        if ([self.window styleMask] & NSWindowStyleMaskFullScreen) {
            [self.window toggleFullScreen:nil];
            return;
        }
    }

    PS1Input* input = &_context->input;
    switch (event.keyCode) {
        case 126: ps1_input_set_button(input, 0, PS1_BTN_UP, true); break;    // Up arrow
        case 125: ps1_input_set_button(input, 0, PS1_BTN_DOWN, true); break;  // Down arrow
        case 123: ps1_input_set_button(input, 0, PS1_BTN_LEFT, true); break;  // Left arrow
        case 124: ps1_input_set_button(input, 0, PS1_BTN_RIGHT, true); break; // Right arrow
        case 6:   ps1_input_set_button(input, 0, PS1_BTN_CROSS, true); break; // Z
        case 7:   ps1_input_set_button(input, 0, PS1_BTN_CIRCLE, true); break;// X
        case 0:   ps1_input_set_button(input, 0, PS1_BTN_SQUARE, true); break;// A
        case 1:   ps1_input_set_button(input, 0, PS1_BTN_TRIANGLE, true); break;// S
        case 36:  ps1_input_set_button(input, 0, PS1_BTN_START, true); break; // Enter
        case 49:  ps1_input_set_button(input, 0, PS1_BTN_SELECT, true); break;// Space
        case 12:  ps1_input_set_button(input, 0, PS1_BTN_L1, true); break;    // Q
        case 13:  ps1_input_set_button(input, 0, PS1_BTN_R1, true); break;    // W
    }
}

- (void)keyUp:(NSEvent *)event {
    if (!_context) return;
    PS1Input* input = &_context->input;
    switch (event.keyCode) {
        case 126: ps1_input_set_button(input, 0, PS1_BTN_UP, false); break;
        case 125: ps1_input_set_button(input, 0, PS1_BTN_DOWN, false); break;
        case 123: ps1_input_set_button(input, 0, PS1_BTN_LEFT, false); break;
        case 124: ps1_input_set_button(input, 0, PS1_BTN_RIGHT, false); break;
        case 6:   ps1_input_set_button(input, 0, PS1_BTN_CROSS, false); break;
        case 7:   ps1_input_set_button(input, 0, PS1_BTN_CIRCLE, false); break;
        case 0:   ps1_input_set_button(input, 0, PS1_BTN_SQUARE, false); break;
        case 1:   ps1_input_set_button(input, 0, PS1_BTN_TRIANGLE, false); break;
        case 36:  ps1_input_set_button(input, 0, PS1_BTN_START, false); break;
        case 49:  ps1_input_set_button(input, 0, PS1_BTN_SELECT, false); break;
        case 12:  ps1_input_set_button(input, 0, PS1_BTN_L1, false); break;
        case 13:  ps1_input_set_button(input, 0, PS1_BTN_R1, false); break;
    }
}

@end

// Master entry point for native app
int run_native_macos_app(PS1Context* ctx, const char* window_title) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

        // Create Main Menu
        NSMenu* menubar = [[NSMenu alloc] init];
        NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
        [menubar addItem:appMenuItem];
        [NSApp setMainMenu:menubar];

        NSMenu* appMenu = [[NSMenu alloc] init];
        NSString* appName = window_title ? [NSString stringWithUTF8String:window_title] : @"MimiPS1";
        [appMenu addItemWithTitle:[NSString stringWithFormat:@"Quit %@", appName]
                           action:@selector(terminate:)
                    keyEquivalent:@"q"];
        [appMenuItem setSubmenu:appMenu];

        // Create Window
        NSRect frame = NSMakeRect(0, 0, 1024, 768);
        NSWindow* window = [[NSWindow alloc] initWithContentRect:frame
                                                       styleMask:(NSWindowStyleMaskTitled |
                                                                  NSWindowStyleMaskClosable |
                                                                  NSWindowStyleMaskMiniaturizable |
                                                                  NSWindowStyleMaskResizable)
                                                         backing:NSBackingStoreBuffered
                                                           defer:NO];
        [window setTitle:[NSString stringWithFormat:@"%@ [Apple Silicon]", appName]];
        [window setCollectionBehavior:NSWindowCollectionBehaviorFullScreenPrimary];
        [window setMinSize:NSMakeSize(640, 480)];
        [window center];

        // Create Metal View
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        MTKView* mtkView = [[MTKView alloc] initWithFrame:frame device:device];
        mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        mtkView.clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        mtkView.preferredFramesPerSecond = 60;

        MimiPS1ViewDelegate* delegate = [[MimiPS1ViewDelegate alloc] initWithContext:ctx device:device];
        mtkView.delegate = delegate;

        MimiPS1WindowController* winController = [[MimiPS1WindowController alloc] initWithWindow:window];
        winController.context = ctx;
        winController.mtkView = mtkView;
        winController.viewDelegate = delegate;
        window.delegate = winController;
        [window setContentViewController:[[NSViewController alloc] init]];
        window.contentViewController.view = mtkView;
        [winController setupControllerEvents];

        // Init CoreAudio
        macos_audio_init(ctx);

        // Start CPU Game Execution on dedicated background thread
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
            printf("[MimiPS1] Starting native game execution thread (PC: 0x%08X)...\n", ctx->cpu.pc);
            if (ctx->cpu.pc != 0) {
                ps1_call_indirect(ctx, ctx->cpu.pc);
            }
            printf("[MimiPS1] Native game execution finished.\n");
        });

        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];

        macos_audio_shutdown();
    }
    return 0;
}
