#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "ps1_exe.h"
#include "mips_cfg.h"
#include "c_emitter.h"

static void print_banner(void) {
    printf("===============================================================\n");
    printf(" MimiPS1-Recomp - Next-Gen Ahead-of-Time Static Recompiler\n");
    printf(" PlayStation 1 (MIPS R3000A) -> Native Apple Silicon ARM64 (M4)\n");
    printf("===============================================================\n\n");
}

static void print_usage(const char* prog) {
    printf("Usage: %s <game.exe> [options]\n\n", prog);
    printf("Options:\n");
    printf("  -o, --output <dir>    Output directory for recompiled C code (default: ./recompiled)\n");
    printf("  -v, --verbose         Enable detailed disasm logging\n");
    printf("  -h, --help            Show this help message\n\n");
}

int main(int argc, char** argv) {
    print_banner();

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* input_path = NULL;
    const char* output_dir = "./recompiled";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) {
            if (i + 1 < argc) output_dir = argv[++i];
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        }
    }

    if (!input_path) {
        fprintf(stderr, "[Error] No input executable specified.\n");
        return 1;
    }

    // Create output directory if needed
    mkdir(output_dir, 0755);

    // 1. Load PS-X EXE
    PS1Executable exe;
    if (!ps1_exe_load(input_path, &exe)) {
        return 1;
    }

    // 2. CFG Analysis
    printf("[1/3] Performing Control Flow Graph & Function Analysis...\n");
    MipsProgram* prog = mips_analyze_program(&exe);
    if (!prog) {
        fprintf(stderr, "[Error] CFG analysis failed.\n");
        ps1_exe_free(&exe);
        return 1;
    }
    printf("      Total functions identified: %d\n", prog->func_count);

    // 3. Emit C Code
    printf("[2/3] Generating High-Performance C Code for Apple Silicon...\n");
    char c_path[512];
    char h_path[512];
    snprintf(c_path, sizeof(c_path), "%s/recompiled_game.c", output_dir);
    snprintf(h_path, sizeof(h_path), "%s/recompiled_game.h", output_dir);

    EmitterOptions opts;
    opts.output_c_path = c_path;
    opts.output_h_path = h_path;
    opts.enable_opt = true;
    opts.emit_comments = true;

    if (!mips_emit_c_code(prog, &opts)) {
        fprintf(stderr, "[Error] C code emission failed.\n");
        mips_free_program(prog);
        ps1_exe_free(&exe);
        return 1;
    }

    printf("[3/3] Successfully generated:\n");
    printf("      -> %s\n", c_path);
    printf("      -> %s\n", h_path);
    printf("\n[Done] Ready to compile into native Apple Silicon DMG!\n");

    mips_free_program(prog);
    ps1_exe_free(&exe);
    return 0;
}
