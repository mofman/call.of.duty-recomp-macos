// macOS-specific bits the SDL layer does not cover.
#import <AppKit/AppKit.h>
#include "platform.h"

// Before SDL_Init (which creates NSApp): stop holding a key from opening the accent ("press and hold") popup.
// registerDefaults is in-memory only, so the user's global preference is untouched.
void plat_mac_init(void) {
    @autoreleasepool {
        [[NSUserDefaults standardUserDefaults] registerDefaults:@{@"ApplePressAndHoldEnabled": @NO}];
    }
}

// A bare executable launched from a terminal is not made frontmost by macOS: bring the game window forward and
// make it key so mouse capture works without alt-tabbing.
void plat_mac_activate(void) {
    @autoreleasepool {
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        if (@available(macOS 14.0, *)) {
            [[NSRunningApplication currentApplication] activateWithOptions:NSApplicationActivateAllWindows];
            [NSApp activate];
        }
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
        }
        for (NSWindow *w in [NSApp windows]) {
            if ([w isVisible]) { [w makeKeyAndOrderFront:nil]; break; }
        }
    }
}

// Pixel size of the main screen's area usable by a fullscreen window: below the camera notch on MacBooks
// (NSScreen.safeAreaInsets), so the native r_mode fills fullscreen exactly. Returns 0 if unavailable.
int plat_mac_fullscreen_size(int *w, int *h) {
    @autoreleasepool {
        NSScreen *s = [NSScreen mainScreen];
        if (!s) return 0;
        NSRect f = s.frame;
        CGFloat top = 0;
        if (@available(macOS 12.0, *)) top = s.safeAreaInsets.top;
        CGFloat sc = s.backingScaleFactor > 0 ? s.backingScaleFactor : 1;
        *w = (int)(f.size.width * sc + 0.5);
        *h = (int)((f.size.height - top) * sc + 0.5);
        return *w > 0 && *h > 0;
    }
}

// Bundle resources stay immutable; both games share a writable first-launch seed.
void plat_mac_bundle_init(void) {
    @autoreleasepool {
        NSBundle *bundle = [NSBundle mainBundle];
        NSString *resources = bundle.resourcePath;
        NSFileManager *fm = [NSFileManager defaultManager];
        NSString *seed = [resources stringByAppendingPathComponent:@"game"];
        if (![fm fileExistsAtPath:seed]) return;
        if (!getenv("COD_BIN_DIR"))
            setenv("COD_BIN_DIR", [[resources stringByAppendingPathComponent:@"bin"] fileSystemRepresentation], 1);
        if (getenv("COD_GAME_DIR")) return;
        NSString *support = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Application Support/Call of Duty Recomp"];
        NSError *error = nil;
        if (![fm createDirectoryAtPath:support withIntermediateDirectories:YES attributes:nil error:&error]) {
            fprintf(stderr, "Cannot create user directory: %s\n", error.description.UTF8String); exit(1);
        }
        NSString *game = [support stringByAppendingPathComponent:@"game"];
        if (![fm fileExistsAtPath:game]) {
            NSString *temp = [support stringByAppendingPathComponent:[@"seed-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            if (![fm copyItemAtPath:seed toPath:temp error:&error] || ![fm moveItemAtPath:temp toPath:game error:&error]) {
                [fm removeItemAtPath:temp error:nil];
                if (![fm fileExistsAtPath:game]) { fprintf(stderr, "Cannot seed game data: %s\n", error.description.UTF8String); exit(1); }
            }
        }
        setenv("COD_GAME_DIR", game.fileSystemRepresentation, 1);
        NSString *log = [support stringByAppendingPathComponent:@"launch.log"];
        freopen(log.fileSystemRepresentation, "a", stderr);
    }
}
