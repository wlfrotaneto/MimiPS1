#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#include "mimips1_runtime.h"

extern bool macos_audio_init(PS1Context* ctx);
extern void macos_audio_shutdown(void);

#import <CoreHaptics/CoreHaptics.h>

@interface MimiPS1ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithContext:(PS1Context*)ctx device:(id<MTLDevice>)device;
@end

@interface MimiPS1WindowController : NSWindowController <NSWindowDelegate> {
    NSMutableArray<GCController*>* _connectedControllers;
    CHHapticEngine* _hapticEngines[2];
    id<CHHapticPatternPlayer> _hapticPlayers[2];
}
@property (nonatomic, assign) PS1Context* context;
@property (nonatomic, strong) MTKView* mtkView;
@property (nonatomic, strong) MimiPS1ViewDelegate* viewDelegate;

- (void)handleRumblePad:(int)padIdx small:(uint8_t)small large:(uint8_t)large;
- (void)attachController:(GCController*)controller;
- (void)detachController:(GCController*)controller;
@end

static void macos_rumble_callback(int pad_idx, uint8_t small_motor, uint8_t large_motor, void* userdata) {
    MimiPS1WindowController* win = (__bridge MimiPS1WindowController*)userdata;
    [win handleRumblePad:pad_idx small:small_motor large:large_motor];
}

@implementation MimiPS1WindowController

- (void)setupControllerEvents {
    _connectedControllers = [[NSMutableArray alloc] init];

    if (self.context) {
        ps1_input_set_rumble_callback(&self.context->input, macos_rumble_callback, (__bridge void*)self);
    }

    // Connect any gamepads already connected before app launch
    for (GCController* c in [GCController controllers]) {
        [self attachController:c];
    }

    // Dynamic connection and disconnection notifications
    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidConnectNotification
                                                      object:nil
                                                       queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification *note) {
        [self attachController:note.object];
    }];

    [[NSNotificationCenter defaultCenter] addObserverForName:GCControllerDidDisconnectNotification
                                                      object:nil
                                                       queue:[NSOperationQueue mainQueue]
                                                  usingBlock:^(NSNotification *note) {
        [self detachController:note.object];
    }];
}

- (void)attachController:(GCController*)controller {
    if (!controller || [_connectedControllers containsObject:controller]) return;

    if (_connectedControllers.count >= 2) {
        NSLog(@"[Gamepad] Max 2 controllers already mapped. Skipping %@", controller.vendorName);
        return;
    }

    [_connectedControllers addObject:controller];
    NSInteger playerIdx = [_connectedControllers indexOfObject:controller];
    controller.playerIndex = (playerIdx == 0) ? GCControllerPlayerIndex1 : GCControllerPlayerIndex2;
    NSLog(@"[Gamepad] Player %ld assigned to: %@", (long)(playerIdx + 1), controller.vendorName);

    // Initialize CoreHaptics if supported
    if (@available(macOS 11.0, *)) {
        if (controller.haptics) {
            NSError* err = nil;
            CHHapticEngine* engine = [controller.haptics createEngineWithLocality:GCHapticsLocalityDefault];
            if (engine) {
                [engine startAndReturnError:&err];
                _hapticEngines[playerIdx] = engine;
                NSLog(@"[Gamepad] Player %ld CoreHaptics engine ready.", (long)(playerIdx + 1));
            }
        }
    }

    __unsafe_unretained typeof(self) weakSelf = self;
    if (controller.extendedGamepad) {
        GCExtendedGamepad* gp = controller.extendedGamepad;
        if (self.context) {
            ps1_input_set_analog(&self.context->input, (int)playerIdx, true);
        }

        gp.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
            MimiPS1WindowController* strongSelf = weakSelf;
            if (!strongSelf || !strongSelf.context) return;
            PS1Input* input = &strongSelf.context->input;
            int p = (int)playerIdx;

            // Face buttons
            ps1_input_set_button(input, p, PS1_BTN_CROSS, gamepad.buttonA.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_CIRCLE, gamepad.buttonB.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_SQUARE, gamepad.buttonX.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_TRIANGLE, gamepad.buttonY.isPressed);

            // D-Pad
            ps1_input_set_button(input, p, PS1_BTN_UP, gamepad.dpad.up.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_DOWN, gamepad.dpad.down.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_LEFT, gamepad.dpad.left.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_RIGHT, gamepad.dpad.right.isPressed);

            // Shoulders & Triggers
            ps1_input_set_button(input, p, PS1_BTN_L1, gamepad.leftShoulder.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_R1, gamepad.rightShoulder.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_L2, (gamepad.leftTrigger.isPressed || gamepad.leftTrigger.value > 0.3f));
            ps1_input_set_button(input, p, PS1_BTN_R2, (gamepad.rightTrigger.isPressed || gamepad.rightTrigger.value > 0.3f));

            // Start & Select
            ps1_input_set_button(input, p, PS1_BTN_START, gamepad.buttonMenu.isPressed);
            bool selectPressed = false;
            if (gamepad.buttonOptions && gamepad.buttonOptions.isPressed) selectPressed = true;
            if (gamepad.buttonHome && gamepad.buttonHome.isPressed) selectPressed = true;
            ps1_input_set_button(input, p, PS1_BTN_SELECT, selectPressed);

            // Thumbstick buttons (L3 / R3)
            if (gamepad.leftThumbstickButton) {
                ps1_input_set_button(input, p, PS1_BTN_L3, gamepad.leftThumbstickButton.isPressed);
            }
            if (gamepad.rightThumbstickButton) {
                ps1_input_set_button(input, p, PS1_BTN_R3, gamepad.rightThumbstickButton.isPressed);
            }

            // Dual Analog Sticks (Left Stick: 0..255, 128 centered, Y inverted)
            float lx = gamepad.leftThumbstick.xAxis.value;
            float ly = gamepad.leftThumbstick.yAxis.value;
            int sx = 128 + (int)(lx * 127.0f);
            int sy = 128 - (int)(ly * 127.0f);
            if (sx < 0) sx = 0; else if (sx > 255) sx = 255;
            if (sy < 0) sy = 0; else if (sy > 255) sy = 255;
            ps1_input_set_stick(input, p, false, (uint8_t)sx, (uint8_t)sy);

            // Right Stick: 0..255, 128 centered
            float rx = gamepad.rightThumbstick.xAxis.value;
            float ry = gamepad.rightThumbstick.yAxis.value;
            int rsx = 128 + (int)(rx * 127.0f);
            int rsy = 128 - (int)(ry * 127.0f);
            if (rsx < 0) rsx = 0; else if (rsx > 255) rsx = 255;
            if (rsy < 0) rsy = 0; else if (rsy > 255) rsy = 255;
            ps1_input_set_stick(input, p, true, (uint8_t)rsx, (uint8_t)rsy);
        };
    } else if (controller.microGamepad) {
        GCMicroGamepad* mgp = controller.microGamepad;
        mgp.valueChangedHandler = ^(GCMicroGamepad *gamepad, GCControllerElement *element) {
            MimiPS1WindowController* strongSelf = weakSelf;
            if (!strongSelf || !strongSelf.context) return;
            PS1Input* input = &strongSelf.context->input;
            int p = (int)playerIdx;
            ps1_input_set_button(input, p, PS1_BTN_CROSS, gamepad.buttonA.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_CIRCLE, gamepad.buttonX.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_UP, gamepad.dpad.up.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_DOWN, gamepad.dpad.down.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_LEFT, gamepad.dpad.left.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_RIGHT, gamepad.dpad.right.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_START, gamepad.buttonMenu.isPressed);
        };
    }
}

- (void)detachController:(GCController*)controller {
    NSInteger idx = [_connectedControllers indexOfObject:controller];
    if (idx != NSNotFound) {
        if (self.context) {
            PS1Input* input = &self.context->input;
            input->pads[idx].buttons = 0xFFFF;
            input->pads[idx].left_stick_x = 128;
            input->pads[idx].left_stick_y = 128;
            input->pads[idx].right_stick_x = 128;
            input->pads[idx].right_stick_y = 128;
        }
        if (@available(macOS 11.0, *)) {
            if (_hapticPlayers[idx]) {
                [_hapticPlayers[idx] stopAtTime:0 error:nil];
                _hapticPlayers[idx] = nil;
            }
            _hapticEngines[idx] = nil;
        }
        [_connectedControllers removeObjectAtIndex:idx];
        NSLog(@"[Gamepad] Player %ld disconnected.", (long)(idx + 1));
    }
}

- (void)handleRumblePad:(int)padIdx small:(uint8_t)small large:(uint8_t)large {
    if (padIdx < 0 || padIdx > 1) return;
    if (@available(macOS 11.0, *)) {
        CHHapticEngine* engine = _hapticEngines[padIdx];
        if (!engine) return;

        if (small == 0 && large == 0) {
            if (_hapticPlayers[padIdx]) {
                [_hapticPlayers[padIdx] stopAtTime:0 error:nil];
                _hapticPlayers[padIdx] = nil;
            }
            return;
        }

        float intensity = fmaxf((float)large / 255.0f, small ? 0.7f : 0.0f);
        float sharpness = (small > 0) ? 0.9f : 0.3f;

        CHHapticEventParameter* paramIntensity = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity value:intensity];
        CHHapticEventParameter* paramSharpness = [[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticSharpness value:sharpness];

        CHHapticEvent* event = [[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                                             parameters:@[paramIntensity, paramSharpness]
                                                           relativeTime:0
                                                               duration:0.25];

        NSError* err = nil;
        CHHapticPattern* pattern = [[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&err];
        if (pattern) {
            if (_hapticPlayers[padIdx]) {
                [_hapticPlayers[padIdx] stopAtTime:0 error:nil];
            }
            id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&err];
            [player startAtTime:0 error:&err];
            _hapticPlayers[padIdx] = player;
        }
    }
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
        // D-Pad
        case 126: ps1_input_set_button(input, 0, PS1_BTN_UP, true); break;    // Up arrow
        case 125: ps1_input_set_button(input, 0, PS1_BTN_DOWN, true); break;  // Down arrow
        case 123: ps1_input_set_button(input, 0, PS1_BTN_LEFT, true); break;  // Left arrow
        case 124: ps1_input_set_button(input, 0, PS1_BTN_RIGHT, true); break; // Right arrow

        // Action Buttons
        case 6:   ps1_input_set_button(input, 0, PS1_BTN_CROSS, true); break; // Z
        case 7:   ps1_input_set_button(input, 0, PS1_BTN_CIRCLE, true); break;// X
        case 0:   ps1_input_set_button(input, 0, PS1_BTN_SQUARE, true); break;// A
        case 1:   ps1_input_set_button(input, 0, PS1_BTN_TRIANGLE, true); break;// S

        // Start / Select
        case 36:  ps1_input_set_button(input, 0, PS1_BTN_START, true); break; // Enter
        case 49:  ps1_input_set_button(input, 0, PS1_BTN_SELECT, true); break;// Space
        case 51:  ps1_input_set_button(input, 0, PS1_BTN_SELECT, true); break;// Backspace

        // Shoulders & Triggers
        case 12:  ps1_input_set_button(input, 0, PS1_BTN_L1, true); break;    // Q
        case 13:  ps1_input_set_button(input, 0, PS1_BTN_R1, true); break;    // W
        case 14:  ps1_input_set_button(input, 0, PS1_BTN_L2, true); break;    // E
        case 15:  ps1_input_set_button(input, 0, PS1_BTN_R2, true); break;    // R

        // Thumbstick Buttons (L3 / R3)
        case 8:   ps1_input_set_button(input, 0, PS1_BTN_L3, true); break;    // C
        case 9:   ps1_input_set_button(input, 0, PS1_BTN_R3, true); break;    // V

        // Toggle DualShock Analog Mode
        case 48: // Tab
        case 17: // T
            input->pads[0].is_analog = !input->pads[0].is_analog;
            NSLog(@"[Input] DualShock Analog Mode: %@", input->pads[0].is_analog ? @"ON" : @"OFF");
            break;
    }
}

- (void)keyUp:(NSEvent *)event {
    if (!_context) return;
    PS1Input* input = &_context->input;
    switch (event.keyCode) {
        // D-Pad
        case 126: ps1_input_set_button(input, 0, PS1_BTN_UP, false); break;
        case 125: ps1_input_set_button(input, 0, PS1_BTN_DOWN, false); break;
        case 123: ps1_input_set_button(input, 0, PS1_BTN_LEFT, false); break;
        case 124: ps1_input_set_button(input, 0, PS1_BTN_RIGHT, false); break;

        // Action Buttons
        case 6:   ps1_input_set_button(input, 0, PS1_BTN_CROSS, false); break;
        case 7:   ps1_input_set_button(input, 0, PS1_BTN_CIRCLE, false); break;
        case 0:   ps1_input_set_button(input, 0, PS1_BTN_SQUARE, false); break;
        case 1:   ps1_input_set_button(input, 0, PS1_BTN_TRIANGLE, false); break;

        // Start / Select
        case 36:  ps1_input_set_button(input, 0, PS1_BTN_START, false); break;
        case 49:  ps1_input_set_button(input, 0, PS1_BTN_SELECT, false); break;
        case 51:  ps1_input_set_button(input, 0, PS1_BTN_SELECT, false); break;

        // Shoulders & Triggers
        case 12:  ps1_input_set_button(input, 0, PS1_BTN_L1, false); break;
        case 13:  ps1_input_set_button(input, 0, PS1_BTN_R1, false); break;
        case 14:  ps1_input_set_button(input, 0, PS1_BTN_L2, false); break;
        case 15:  ps1_input_set_button(input, 0, PS1_BTN_R2, false); break;

        // Thumbstick Buttons (L3 / R3)
        case 8:   ps1_input_set_button(input, 0, PS1_BTN_L3, false); break;
        case 9:   ps1_input_set_button(input, 0, PS1_BTN_R3, false); break;
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
