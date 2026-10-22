#import <Cocoa/Cocoa.h>
#include "mimips1_runtime.h"

extern int run_native_macos_app(PS1Context* ctx, const char* window_title);
__attribute__((weak)) void ps1_init_recompiled_game(PS1Context* ctx) {
    (void)ctx;
}
__attribute__((weak)) void ps1_run_recompiled_game(PS1Context* ctx) {
    (void)ctx;
}

int main(int argc, char** argv) {
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);
    PS1Context* ctx = ps1_runtime_create();
    if (!ctx) {
        fprintf(stderr, "[Fatal] Failed to create PS1 runtime context\n");
        return 1;
    }

    // Register recompiled functions if statically linked
    if (ps1_init_recompiled_game) {
        printf("[MimiPS1] Initializing AOT recompiled function tables...\n");
        ps1_init_recompiled_game(ctx);
    }

    NSString* bundleName = [[[NSBundle mainBundle] infoDictionary] objectForKey:@"CFBundleName"];
    const char* app_name = bundleName ? [bundleName UTF8String] : "MimiPS1 - Recompiled Native macOS";

    // Auto-detect resources from App Bundle and User directories
    @autoreleasepool {
        NSBundle* bundle = [NSBundle mainBundle];
        NSString* resPath = [bundle resourcePath];
        NSFileManager* fm = [NSFileManager defaultManager];
        NSString* homeDir = NSHomeDirectory();

        // 1. Auto-discover official BIOS if present (prioritize app bundle)
        NSMutableArray* biosCandidates = [NSMutableArray array];
        if (resPath) {
            [biosCandidates addObject:[resPath stringByAppendingPathComponent:@"bios.bin"]];
        }
        [biosCandidates addObject:[homeDir stringByAppendingPathComponent:@".mimips1/bios/SCPH1001.BIN"]];
        [biosCandidates addObject:[homeDir stringByAppendingPathComponent:@".mimips1/bios/scph1001.bin"]];

        for (NSString* cand in biosCandidates) {
            if (cand.length > 0 && [fm fileExistsAtPath:cand]) {
                if (ps1_memory_load_bios(ctx, [cand UTF8String])) {
                    printf("[MimiPS1] Official PS1 BIOS detected & loaded: %s\n", [cand UTF8String]);
                    break;
                }
            }
        }
        if (!ctx->memory.has_bios) {
            printf("[MimiPS1] Using built-in Clean-Room BIOS HLE\n");
        }

        // 2. Auto-load initial boot executable (code and data segment)
        if (resPath) {
            NSString* bootExe = [resPath stringByAppendingPathComponent:@"boot.exe"];
            if ([fm fileExistsAtPath:bootExe]) {
                printf("[MimiPS1] Auto-loading boot executable from bundle: %s\n", [bootExe UTF8String]);
                ps1_runtime_load_exe(ctx, [bootExe UTF8String]);
            }

            // 3. Auto-mount game disc image (.bin/.iso)
            NSArray* entries = [fm contentsOfDirectoryAtPath:resPath error:nil];
            for (NSString* entry in entries) {
                if ([entry.lowercaseString isEqualToString:@"bios.bin"] || [entry.lowercaseString isEqualToString:@"boot.exe"]) {
                    continue;
                }
                NSString* ext = [entry.pathExtension lowercaseString];
                if ([ext isEqualToString:@"bin"] || [ext isEqualToString:@"iso"] || [ext isEqualToString:@"img"]) {
                    NSString* full = [resPath stringByAppendingPathComponent:entry];
                    printf("[MimiPS1] Auto-mounting game disc from bundle: %s\n", [full UTF8String]);
                    ps1_cdrom_open_image(&ctx->cdrom, [full UTF8String]);
                    break;
                }
            }
        }
    }

    // Command-line overrides if passed
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            const char* arg = argv[i];
            size_t len = strlen(arg);
            if (strcmp(arg, "--bios") == 0 && i + 1 < argc) {
                ps1_memory_load_bios(ctx, argv[++i]);
                printf("[MimiPS1] Loaded BIOS from CLI: %s\n", argv[i]);
            } else if (len > 4 && (strcasecmp(arg + len - 4, ".iso") == 0 ||
                                   strcasecmp(arg + len - 4, ".bin") == 0)) {
                ps1_cdrom_open_image(&ctx->cdrom, arg);
                app_name = arg;
            } else {
                ps1_runtime_load_exe(ctx, arg);
                app_name = arg;
            }
        }
    }

    // With the official BIOS present, boot through it (it loads the game from the disc and
    // provides the real kernel). MIMIPS1_HLE=1 forces the built-in HLE kernel.
    if (ctx->memory.has_bios && !getenv("MIMIPS1_HLE") && ctx->cdrom.image_file) {
        ps1_runtime_boot_bios(ctx);
        printf("[MimiPS1] Booting through the real BIOS\n");
    }

    printf("[MimiPS1] Launching native Apple Silicon Metal window...\n");
    int result = run_native_macos_app(ctx, app_name);

    ps1_runtime_destroy(ctx);
    return result;
}
