#ifndef RECOMP_PS1_EXE_H
#define RECOMP_PS1_EXE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char     magic[8];          // "PS-X EXE"
    uint32_t text_offset;
    uint32_t data_offset;
    uint32_t init_pc;
    uint32_t init_gp;
    uint32_t text_dest_addr;
    uint32_t text_size;
    uint32_t data_dest_addr;
    uint32_t data_size;
    uint32_t bss_dest_addr;
    uint32_t bss_size;
    uint32_t init_sp_base;
    uint32_t init_sp_offset;

    uint8_t* code_buffer;       // Raw text segment
    size_t   file_size;
} PS1Executable;

bool ps1_exe_load(const char* filepath, PS1Executable* exe);
void ps1_exe_free(PS1Executable* exe);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_PS1_EXE_H
