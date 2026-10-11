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

    const char* app_name = "MimiPS1 - Recompiled Native macOS";

    // If an executable or disc image is passed
    if (argc > 1) {
        const char* file_arg = argv[1];
        size_t len = strlen(file_arg);
        if (len > 4 && (strcasecmp(file_arg + len - 4, ".iso") == 0 ||
                        strcasecmp(file_arg + len - 4, ".bin") == 0)) {
            ps1_cdrom_open_image(&ctx->cdrom, file_arg);
        } else {
            ps1_runtime_load_exe(ctx, file_arg);
        }
        app_name = file_arg;
    }

    printf("[MimiPS1] Launching native Apple Silicon Metal window...\n");
    int result = run_native_macos_app(ctx, app_name);

    ps1_runtime_destroy(ctx);
    return result;
}
