#include "ps1_exe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ps1_exe_load(const char* filepath, PS1Executable* exe) {
    memset(exe, 0, sizeof(PS1Executable));

    FILE* f = fopen(filepath, "rb");
    if (!f) {
        fprintf(stderr, "[Error] Failed to open PS1 executable: %s\n", filepath);
        return false;
    }

    fseek(f, 0, SEEK_END);
    exe->file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (exe->file_size < 2048) {
        fprintf(stderr, "[Error] File too small to be a valid PS-X EXE (size=%zu)\n", exe->file_size);
        fclose(f);
        return false;
    }

    uint8_t header[2048];
    if (fread(header, 1, 2048, f) != 2048) {
        fprintf(stderr, "[Error] Failed reading header\n");
        fclose(f);
        return false;
    }

    if (memcmp(header, "PS-X EXE", 8) != 0) {
        fprintf(stderr, "[Error] Magic mismatch, expected 'PS-X EXE'\n");
        fclose(f);
        return false;
    }

    memcpy(exe->magic, header, 8);
    exe->text_offset    = *(uint32_t*)&header[0x08];
    exe->data_offset    = *(uint32_t*)&header[0x0C];
    exe->init_pc        = *(uint32_t*)&header[0x10];
    exe->init_gp        = *(uint32_t*)&header[0x14];
    exe->text_dest_addr = *(uint32_t*)&header[0x18];
    exe->text_size      = *(uint32_t*)&header[0x1C];
    exe->data_dest_addr = *(uint32_t*)&header[0x20];
    exe->data_size      = *(uint32_t*)&header[0x24];
    exe->bss_dest_addr  = *(uint32_t*)&header[0x28];
    exe->bss_size       = *(uint32_t*)&header[0x2C];
    exe->init_sp_base   = *(uint32_t*)&header[0x30];
    exe->init_sp_offset = *(uint32_t*)&header[0x34];

    if (exe->text_size == 0 || exe->text_size > (PS1_RAM_SIZE)) {
        // Fallback size if header has 0 or garbage
        exe->text_size = (uint32_t)(exe->file_size - 0x800);
    }

    exe->code_buffer = (uint8_t*)malloc(exe->text_size);
    if (!exe->code_buffer) {
        fprintf(stderr, "[Error] Failed allocating %u bytes for text section\n", exe->text_size);
        fclose(f);
        return false;
    }

    fseek(f, 0x800, SEEK_SET);
    size_t read_bytes = fread(exe->code_buffer, 1, exe->text_size, f);
    if (read_bytes == 0) {
        fprintf(stderr, "[Error] No bytes read from text section\n");
        free(exe->code_buffer);
        exe->code_buffer = NULL;
        fclose(f);
        return false;
    }

    fclose(f);
    printf("[PS1-EXE] Successfully loaded: %s\n", filepath);
    printf("          PC: 0x%08X | GP: 0x%08X | SP: 0x%08X\n", exe->init_pc, exe->init_gp, exe->init_sp_base + exe->init_sp_offset);
    printf("          Dest: 0x%08X | Size: %u bytes\n", exe->text_dest_addr, exe->text_size);
    return true;
}

void ps1_exe_free(PS1Executable* exe) {
    if (exe->code_buffer) {
        free(exe->code_buffer);
        exe->code_buffer = NULL;
    }
}
