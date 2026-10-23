#import <Cocoa/Cocoa.h>
#import <objc/runtime.h>
#import <Metal/Metal.h>
#import <MetalKit/MetalKit.h>
#import <GameController/GameController.h>
#include "mimips1_runtime.h"
#include "mimips1_card.h"

extern bool macos_audio_init(PS1Context* ctx);
extern void macos_audio_shutdown(void);

#import <CoreHaptics/CoreHaptics.h>

@interface MimiPS1ViewDelegate : NSObject <MTKViewDelegate>
- (instancetype)initWithContext:(PS1Context*)ctx device:(id<MTLDevice>)device;
@property (nonatomic, assign) BOOL smoothFiltering;
@end

extern volatile float g_ps1_master_volume;
extern volatile bool  g_ps1_muted;

@interface MimiPS1WindowController : NSWindowController <NSWindowDelegate> {
    NSMutableArray<GCController*>* _connectedControllers;
    CHHapticEngine* _hapticEngines[4];
    id<CHHapticPatternPlayer> _hapticPlayers[4];
    NSString* _padSource[2];     // @"auto", @"keyboard", @"none" or @"pad:<name>#<n>" for PS1 port 1 / 2
}
@property (nonatomic, assign) PS1Context* context;
@property (nonatomic, strong) MTKView* mtkView;
@property (nonatomic, strong) MimiPS1ViewDelegate* viewDelegate;

- (void)handleRumblePad:(int)padIdx small:(uint8_t)small large:(uint8_t)large;
- (NSString*)padSourceForPlayer:(int)player;
- (void)setPadSource:(NSString*)source forPlayer:(int)player;
- (NSArray<NSArray<NSString*>*>*)sourceChoices;      // @[@[id, label], ...] for the Settings menu
- (void)attachController:(GCController*)controller;
- (void)detachController:(GCController*)controller;
@end

static void macos_rumble_callback(int pad_idx, uint8_t small_motor, uint8_t large_motor, void* userdata) {
    MimiPS1WindowController* win = (__bridge MimiPS1WindowController*)userdata;
    [win handleRumblePad:pad_idx small:small_motor large:large_motor];
}

@implementation MimiPS1WindowController

- (NSString*)padSourceForPlayer:(int)player { return _padSource[player] ?: @"auto"; }

- (NSString*)idForController:(GCController*)c {
    NSString* name = c.vendorName ?: @"Gamepad";
    NSUInteger n = 0;
    for (GCController* o in _connectedControllers) {
        if (o == c) break;
        if ([(o.vendorName ?: @"Gamepad") isEqualToString:name]) n++;
    }
    return [NSString stringWithFormat:@"pad:%@#%lu", name, (unsigned long)n];
}

// PS1 port driven by a gamepad (-1: not assigned to anyone)
- (NSInteger)padForController:(GCController*)c {
    NSInteger idx = [_connectedControllers indexOfObject:c];
    if (idx == NSNotFound) return -1;
    NSString* cid = [self idForController:c];
    for (int p = 0; p < 2; p++) {
        NSString* src = [self padSourceForPlayer:p];
        if ([src isEqualToString:cid]) return p;
        if ([src isEqualToString:@"auto"] && idx == p) return p;
    }
    return -1;
}

// PS1 port driven by the keyboard (-1: keyboard disabled)
- (NSInteger)padForKeyboard {
    for (int p = 0; p < 2; p++) {
        NSString* src = [self padSourceForPlayer:p];
        if ([src isEqualToString:@"keyboard"] || (p == 0 && [src isEqualToString:@"auto"])) return p;
    }
    return -1;
}

- (NSArray<NSArray<NSString*>*>*)sourceChoices {
    NSMutableArray* r = [NSMutableArray array];
    [r addObject:@[@"auto", @"Automatic"]];
    [r addObject:@[@"keyboard", @"Keyboard"]];
    for (GCController* c in _connectedControllers) [r addObject:@[[self idForController:c], c.vendorName ?: @"Gamepad"]];
    [r addObject:@[@"none", @"None"]];
    return r;
}

// Releases everything on both ports and flags a port as analog when a gamepad drives it
- (void)applyAssignments {
    if (!self.context) return;
    PS1Input* in = &self.context->input;
    for (int p = 0; p < 2; p++) {
        in->pads[p].buttons = 0xFFFF;
        in->phys[p] = 0; in->queued[p] = 0; in->release_pending[p] = 0;
        in->pads[p].left_stick_x = in->pads[p].left_stick_y = 128;
        in->pads[p].right_stick_x = in->pads[p].right_stick_y = 128;
        bool analog = false;
        for (GCController* c in _connectedControllers) if (c.extendedGamepad && [self padForController:c] == p) analog = true;
        ps1_input_set_analog(in, p, analog);
    }
}

- (void)setPadSource:(NSString*)source forPlayer:(int)player {
    if (player < 0 || player > 1) return;
    _padSource[player] = source;
    [[NSUserDefaults standardUserDefaults] setObject:source forKey:player ? @"MimiPS1.player2" : @"MimiPS1.player1"];
    [self applyAssignments];
}

- (void)loadAssignments {
    NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
    _padSource[0] = [d stringForKey:@"MimiPS1.player1"] ?: @"auto";
    _padSource[1] = [d stringForKey:@"MimiPS1.player2"] ?: @"auto";
}

- (void)setupControllerEvents {
    _connectedControllers = [[NSMutableArray alloc] init];
    [self loadAssignments];

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

    if (_connectedControllers.count >= 4) {
        NSLog(@"[Gamepad] Max 4 controllers tracked. Skipping %@", controller.vendorName);
        return;
    }

    [_connectedControllers addObject:controller];
    NSInteger playerIdx = [_connectedControllers indexOfObject:controller];
    NSLog(@"[Gamepad] Connected: %@ (assigned to port %ld)", controller.vendorName, (long)[self padForController:controller] + 1);

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

        gp.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
            MimiPS1WindowController* strongSelf = weakSelf;
            if (!strongSelf || !strongSelf.context) return;
            PS1Input* input = &strongSelf.context->input;
            NSInteger port = [strongSelf padForController:gamepad.controller];
            if (port < 0) return;
            int p = (int)port;

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
            NSInteger port = [strongSelf padForController:gamepad.controller];
            if (port < 0) return;
            int p = (int)port;
            ps1_input_set_button(input, p, PS1_BTN_CROSS, gamepad.buttonA.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_CIRCLE, gamepad.buttonX.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_UP, gamepad.dpad.up.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_DOWN, gamepad.dpad.down.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_LEFT, gamepad.dpad.left.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_RIGHT, gamepad.dpad.right.isPressed);
            ps1_input_set_button(input, p, PS1_BTN_START, gamepad.buttonMenu.isPressed);
        };
    }
    [self applyAssignments];
}

- (void)detachController:(GCController*)controller {
    NSInteger idx = [_connectedControllers indexOfObject:controller];
    if (idx != NSNotFound) {
        if (@available(macOS 11.0, *)) {
            if (_hapticPlayers[idx]) {
                [_hapticPlayers[idx] stopAtTime:0 error:nil];
                _hapticPlayers[idx] = nil;
            }
            _hapticEngines[idx] = nil;
            for (NSInteger i = idx; i < 3; i++) { _hapticEngines[i] = _hapticEngines[i + 1]; _hapticPlayers[i] = _hapticPlayers[i + 1]; }
            _hapticEngines[3] = nil; _hapticPlayers[3] = nil;
        }
        [_connectedControllers removeObjectAtIndex:idx];
        NSLog(@"[Gamepad] Disconnected: %@", controller.vendorName);
        [self applyAssignments];
    }
}

- (void)handleRumblePad:(int)padIdx small:(uint8_t)small large:(uint8_t)large {
    if (padIdx < 0 || padIdx > 1) return;
    if (@available(macOS 11.0, *)) {
        NSInteger ci = -1;
        for (NSUInteger i = 0; i < _connectedControllers.count; i++) if ([self padForController:_connectedControllers[i]] == padIdx) { ci = (NSInteger)i; break; }
        if (ci < 0) return;
        CHHapticEngine* engine = _hapticEngines[ci];
        if (!engine) return;

        if (small == 0 && large == 0) {
            if (_hapticPlayers[ci]) {
                [_hapticPlayers[ci] stopAtTime:0 error:nil];
                _hapticPlayers[ci] = nil;
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
            if (_hapticPlayers[ci]) {
                [_hapticPlayers[ci] stopAtTime:0 error:nil];
            }
            id<CHHapticPatternPlayer> player = [engine createPlayerWithPattern:pattern error:&err];
            [player startAtTime:0 error:&err];
            _hapticPlayers[ci] = player;
        }
    }
}

- (void)windowWillClose:(NSNotification *)notification {
    [NSApp terminate:nil];
}

- (void)keyDown:(NSEvent *)event {
    if (!_context) return;
    if (event.modifierFlags & NSEventModifierFlagCommand) return;   // menu shortcuts
    if (event.keyCode == 53) { // Escape
        if ([self.window styleMask] & NSWindowStyleMaskFullScreen) {
            [self.window toggleFullScreen:nil];
            return;
        }
    }

    PS1Input* input = &_context->input;
    NSInteger kport = [self padForKeyboard];
    if (kport < 0) return;
    int kp = (int)kport;
    switch (event.keyCode) {
        // D-Pad
        case 126: ps1_input_set_button(input, kp, PS1_BTN_UP, true); break;    // Up arrow
        case 125: ps1_input_set_button(input, kp, PS1_BTN_DOWN, true); break;  // Down arrow
        case 123: ps1_input_set_button(input, kp, PS1_BTN_LEFT, true); break;  // Left arrow
        case 124: ps1_input_set_button(input, kp, PS1_BTN_RIGHT, true); break; // Right arrow

        // Action Buttons
        case 6:   ps1_input_set_button(input, kp, PS1_BTN_CROSS, true); break; // Z
        case 7:   ps1_input_set_button(input, kp, PS1_BTN_CIRCLE, true); break;// X
        case 0:   ps1_input_set_button(input, kp, PS1_BTN_SQUARE, true); break;// A
        case 1:   ps1_input_set_button(input, kp, PS1_BTN_TRIANGLE, true); break;// S

        // Start / Select
        case 36:  ps1_input_set_button(input, kp, PS1_BTN_START, true); break; // Enter
        case 49:  ps1_input_set_button(input, kp, PS1_BTN_SELECT, true); break;// Space
        case 51:  ps1_input_set_button(input, kp, PS1_BTN_SELECT, true); break;// Backspace

        // Shoulders & Triggers
        case 12:  ps1_input_set_button(input, kp, PS1_BTN_L1, true); break;    // Q
        case 13:  ps1_input_set_button(input, kp, PS1_BTN_R1, true); break;    // W
        case 14:  ps1_input_set_button(input, kp, PS1_BTN_L2, true); break;    // E
        case 15:  ps1_input_set_button(input, kp, PS1_BTN_R2, true); break;    // R

        // Thumbstick Buttons (L3 / R3)
        case 8:   ps1_input_set_button(input, kp, PS1_BTN_L3, true); break;    // C
        case 9:   ps1_input_set_button(input, kp, PS1_BTN_R3, true); break;    // V

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
    NSInteger kport = [self padForKeyboard];
    if (kport < 0) return;
    int kp = (int)kport;
    switch (event.keyCode) {
        // D-Pad
        case 126: ps1_input_set_button(input, kp, PS1_BTN_UP, false); break;
        case 125: ps1_input_set_button(input, kp, PS1_BTN_DOWN, false); break;
        case 123: ps1_input_set_button(input, kp, PS1_BTN_LEFT, false); break;
        case 124: ps1_input_set_button(input, kp, PS1_BTN_RIGHT, false); break;

        // Action Buttons
        case 6:   ps1_input_set_button(input, kp, PS1_BTN_CROSS, false); break;
        case 7:   ps1_input_set_button(input, kp, PS1_BTN_CIRCLE, false); break;
        case 0:   ps1_input_set_button(input, kp, PS1_BTN_SQUARE, false); break;
        case 1:   ps1_input_set_button(input, kp, PS1_BTN_TRIANGLE, false); break;

        // Start / Select
        case 36:  ps1_input_set_button(input, kp, PS1_BTN_START, false); break;
        case 49:  ps1_input_set_button(input, kp, PS1_BTN_SELECT, false); break;
        case 51:  ps1_input_set_button(input, kp, PS1_BTN_SELECT, false); break;

        // Shoulders & Triggers
        case 12:  ps1_input_set_button(input, kp, PS1_BTN_L1, false); break;
        case 13:  ps1_input_set_button(input, kp, PS1_BTN_R1, false); break;
        case 14:  ps1_input_set_button(input, kp, PS1_BTN_L2, false); break;
        case 15:  ps1_input_set_button(input, kp, PS1_BTN_R2, false); break;

        // Thumbstick Buttons (L3 / R3)
        case 8:   ps1_input_set_button(input, kp, PS1_BTN_L3, false); break;
        case 9:   ps1_input_set_button(input, kp, PS1_BTN_R3, false); break;
    }
}

@end


// ---------------------------------------------------------------------------------------------
// Settings menu (volume, controllers, video, memory cards). Preferences live in NSUserDefaults.

@interface MimiPS1Settings : NSObject <NSMenuDelegate>
@property (nonatomic, assign) PS1Context* context;
@property (nonatomic, strong) MimiPS1WindowController* windowController;
@property (nonatomic, strong) NSWindow* window;
@property (nonatomic, strong) NSSlider* volumeSlider;
@property (nonatomic, strong) NSMenuItem* muteItem;
@property (nonatomic, strong) NSMenuItem* scanlinesItem;
@property (nonatomic, strong) NSMenuItem* smoothItem;
@property (nonatomic, strong) NSMenu* player1Menu;
@property (nonatomic, strong) NSMenu* player2Menu;
- (NSMenuItem*)buildMenuItem;
- (void)applyStoredSettings;
@end

@implementation MimiPS1Settings

- (void)applyStoredSettings {
    NSUserDefaults* d = [NSUserDefaults standardUserDefaults];
    g_ps1_master_volume = [d objectForKey:@"MimiPS1.volume"] ? [d floatForKey:@"MimiPS1.volume"] : 1.0f;
    g_ps1_muted = [d boolForKey:@"MimiPS1.muted"];
    self.context->crt_scanlines = [d boolForKey:@"MimiPS1.scanlines"];
    self.windowController.viewDelegate.smoothFiltering = [d boolForKey:@"MimiPS1.smooth"];
}

- (void)volumeChanged:(NSSlider*)sender {
    g_ps1_master_volume = sender.floatValue;
    if (g_ps1_muted && sender.floatValue > 0) g_ps1_muted = false;
    [[NSUserDefaults standardUserDefaults] setFloat:sender.floatValue forKey:@"MimiPS1.volume"];
    [[NSUserDefaults standardUserDefaults] setBool:g_ps1_muted forKey:@"MimiPS1.muted"];
}

- (void)toggleMute:(id)sender {
    g_ps1_muted = !g_ps1_muted;
    [[NSUserDefaults standardUserDefaults] setBool:g_ps1_muted forKey:@"MimiPS1.muted"];
}

- (void)toggleScanlines:(id)sender {
    self.context->crt_scanlines = !self.context->crt_scanlines;
    [[NSUserDefaults standardUserDefaults] setBool:self.context->crt_scanlines forKey:@"MimiPS1.scanlines"];
}

- (void)toggleSmooth:(id)sender {
    BOOL v = !self.windowController.viewDelegate.smoothFiltering;
    self.windowController.viewDelegate.smoothFiltering = v;
    [[NSUserDefaults standardUserDefaults] setBool:v forKey:@"MimiPS1.smooth"];
}

- (void)toggleFullScreen:(id)sender { [self.window toggleFullScreen:nil]; }

- (void)chooseSource:(NSMenuItem*)item {
    NSArray* info = item.representedObject;       // @[player (NSNumber), id]
    [self.windowController setPadSource:info[1] forPlayer:[info[0] intValue]];
}

- (void)openCardFolder:(id)sender {
    const char* path = self.context->card_mgr.cards[0].file_path;
    NSURL* url = path[0] ? [NSURL fileURLWithPath:[NSString stringWithUTF8String:path]] : nil;
    if (url) [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
    else [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:[NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/MimiPS1/memcards"]]];
}

- (void)resetCard:(NSMenuItem*)item {
    int slot = (int)item.tag;
    NSAlert* alert = [[NSAlert alloc] init];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = [NSString stringWithFormat:@"Erase Memory Card %d?", slot + 1];
    alert.informativeText = @"Every save on this card will be deleted. The card is shared by all MimiPS1 games, and the running game should be restarted afterwards.";
    [alert addButtonWithTitle:@"Cancel"];
    [alert addButtonWithTitle:@"Erase Card"];
    if ([alert runModal] != NSAlertSecondButtonReturn) return;
    PS1CardManager* mgr = &self.context->card_mgr;
    ps1_card_format(mgr, slot);
    ps1_card_save(mgr, slot);
    self.context->input.card_flag = 0x08;      // looks like a new card to the game
    NSLog(@"[Settings] Memory card %d formatted", slot + 1);
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
    if (item == self.muteItem) item.state = g_ps1_muted ? NSControlStateValueOn : NSControlStateValueOff;
    else if (item == self.scanlinesItem) item.state = self.context->crt_scanlines ? NSControlStateValueOn : NSControlStateValueOff;
    else if (item == self.smoothItem) item.state = self.windowController.viewDelegate.smoothFiltering ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}

// Player submenus list the gamepads that are connected right now
- (void)menuNeedsUpdate:(NSMenu*)menu {
    if (menu == self.volumeSlider.enclosingMenuItem.menu) { self.volumeSlider.floatValue = g_ps1_master_volume; return; }
    int player = (menu == self.player1Menu) ? 0 : (menu == self.player2Menu) ? 1 : -1;
    if (player < 0) return;
    [menu removeAllItems];
    NSString* current = [self.windowController padSourceForPlayer:player];
    BOOL found = NO;
    for (NSArray<NSString*>* choice in [self.windowController sourceChoices]) {
        NSMenuItem* it = [menu addItemWithTitle:choice[1] action:@selector(chooseSource:) keyEquivalent:@""];
        it.target = self;
        it.representedObject = @[@(player), choice[0]];
        if ([choice[0] isEqualToString:current]) { it.state = NSControlStateValueOn; found = YES; }
    }
    if (!found) {   // the saved gamepad is not connected: keep showing the choice
        NSString* name = [current hasPrefix:@"pad:"] ? [current substringFromIndex:4] : current;
        NSMenuItem* it = [menu insertItemWithTitle:[name stringByAppendingString:@" (not connected)"] action:nil keyEquivalent:@"" atIndex:2];
        it.state = NSControlStateValueOn; it.enabled = NO;
    }
}

- (NSMenuItem*)buildMenuItem {
    NSMenuItem* root = [[NSMenuItem alloc] init];
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@"Settings"];
    menu.autoenablesItems = YES;
    menu.delegate = self;

    // Volume slider
    NSView* box = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 230, 34)];
    NSTextField* label = [NSTextField labelWithString:@"Volume"];
    label.frame = NSMakeRect(14, 8, 52, 18);
    NSSlider* slider = [NSSlider sliderWithValue:g_ps1_master_volume minValue:0 maxValue:1 target:self action:@selector(volumeChanged:)];
    slider.frame = NSMakeRect(70, 8, 148, 18);
    slider.continuous = YES;
    [box addSubview:label]; [box addSubview:slider];
    self.volumeSlider = slider;
    NSMenuItem* volItem = [[NSMenuItem alloc] init];
    volItem.view = box;
    [menu addItem:volItem];

    self.muteItem = [menu addItemWithTitle:@"Mute" action:@selector(toggleMute:) keyEquivalent:@"m"];
    self.muteItem.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
    self.muteItem.target = self;
    [menu addItem:[NSMenuItem separatorItem]];

    // Controllers
    NSMenuItem* p1 = [menu addItemWithTitle:@"Player 1 Controller" action:nil keyEquivalent:@""];
    self.player1Menu = [[NSMenu alloc] initWithTitle:@"Player 1"]; self.player1Menu.delegate = self; p1.submenu = self.player1Menu;
    NSMenuItem* p2 = [menu addItemWithTitle:@"Player 2 Controller" action:nil keyEquivalent:@""];
    self.player2Menu = [[NSMenu alloc] initWithTitle:@"Player 2"]; self.player2Menu.delegate = self; p2.submenu = self.player2Menu;
    [menu addItem:[NSMenuItem separatorItem]];

    // Video
    self.scanlinesItem = [menu addItemWithTitle:@"CRT Scanlines" action:@selector(toggleScanlines:) keyEquivalent:@"s"];
    self.scanlinesItem.target = self;
    self.smoothItem = [menu addItemWithTitle:@"Smooth Scaling" action:@selector(toggleSmooth:) keyEquivalent:@""];
    self.smoothItem.target = self;
    NSMenuItem* fs = [menu addItemWithTitle:@"Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"];
    fs.target = self;
    [menu addItem:[NSMenuItem separatorItem]];

    // Memory cards
    NSMenuItem* open = [menu addItemWithTitle:@"Show Memory Card Files" action:@selector(openCardFolder:) keyEquivalent:@""];
    open.target = self;
    for (int slot = 0; slot < 2; slot++) {
        NSMenuItem* r = [menu addItemWithTitle:[NSString stringWithFormat:@"Erase Memory Card %d…", slot + 1] action:@selector(resetCard:) keyEquivalent:@""];
        r.target = self; r.tag = slot;
    }

    root.submenu = menu;
    return root;
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
        MimiPS1Settings* settings = [[MimiPS1Settings alloc] init];
        settings.context = ctx;
        NSMenuItem* settingsItem = [settings buildMenuItem];
        [menubar addItem:settingsItem];

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
        settings.windowController = winController;
        settings.window = window;
        [settings applyStoredSettings];
        objc_setAssociatedObject(NSApp, "mimips1.settings", settings, OBJC_ASSOCIATION_RETAIN);   // menu items only hold weak targets

        // Init CoreAudio
        macos_audio_init(ctx);

        // Start CPU Game Execution on dedicated background thread
        dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
            printf("[MimiPS1] Starting native game execution thread (PC: 0x%08X, %s)...\n", ctx->cpu.pc,
                   ctx->bios_boot ? "real BIOS" : "HLE kernel");
            ps1_runtime_run(ctx);
            printf("[MimiPS1] Native game execution finished.\n");
        });

        if (getenv("MIMIPS1_SETTINGS_TEST")) {   // debug aid: walk the menu bar and drive a few items without a human
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
                for (NSMenuItem* top in NSApp.mainMenu.itemArray) {
                    NSMenu* m = top.submenu;
                    if ([m.delegate respondsToSelector:@selector(menuNeedsUpdate:)]) [m.delegate menuNeedsUpdate:m];
                    printf("[Menu] %s\n", top.submenu.title.UTF8String ?: "(app)");
                    for (NSMenuItem* it in m.itemArray) {
                        if (it.submenu) { if ([it.submenu.delegate respondsToSelector:@selector(menuNeedsUpdate:)]) [it.submenu.delegate menuNeedsUpdate:it.submenu];
                            printf("[Menu]   %s >\n", it.title.UTF8String); for (NSMenuItem* sub in it.submenu.itemArray) printf("[Menu]       %s%s\n", sub.title.UTF8String, sub.state == NSControlStateValueOn ? " [x]" : ""); }
                        else printf("[Menu]   %s\n", it.isSeparatorItem ? "----" : (it.title.length ? it.title.UTF8String : "(slider)"));
                    }
                }
                settings.volumeSlider.floatValue = 0.5f; [settings volumeChanged:settings.volumeSlider];
                [settings toggleMute:nil]; printf("[Menu] volume=%.2f muted=%d\n", g_ps1_master_volume, g_ps1_muted); [settings toggleMute:nil];
                [settings toggleScanlines:nil]; printf("[Menu] scanlines=%d\n", ctx->crt_scanlines);
                [winController setPadSource:@"none" forPlayer:0]; printf("[Menu] keyboard port with P1=none: %ld\n", (long)[winController padForKeyboard]);
                [winController setPadSource:@"keyboard" forPlayer:1]; printf("[Menu] keyboard port with P2=keyboard: %ld\n", (long)[winController padForKeyboard]);
                [winController setPadSource:@"auto" forPlayer:0]; [winController setPadSource:@"auto" forPlayer:1]; [settings toggleScanlines:nil];
                [[NSUserDefaults standardUserDefaults] removeObjectForKey:@"MimiPS1.volume"];
                fflush(stdout);
            });
        }
        [window makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp run];

        macos_audio_shutdown();
    }
    return 0;
}
