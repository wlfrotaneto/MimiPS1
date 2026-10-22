#ifndef MIMIPS1_TYPES_H
#define MIMIPS1_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// PlayStation 1 Hardware Clock Frequencies
#define PS1_CPU_CLOCK_HZ        33868800ULL
#define PS1_GPU_DOT_CLOCK_NTSC  53222400ULL
#define PS1_CYCLES_PER_FRAME    (PS1_CPU_CLOCK_HZ / 60)
#define PS1_CYCLES_PER_SCANLINE (PS1_CPU_CLOCK_HZ / (60 * 263)) // ~2145 cycles

// Memory Sizes
#define PS1_RAM_SIZE            (2 * 1024 * 1024)   // 2MB Main RAM
#define PS1_SCRATCHPAD_SIZE     (1024)              // 1KB Scratchpad (D-Cache)
#define PS1_VRAM_WIDTH          1024
#define PS1_VRAM_HEIGHT         512
#define PS1_VRAM_SIZE_BYTES     (PS1_VRAM_WIDTH * PS1_VRAM_HEIGHT * 2) // 1MB 16-bit VRAM
#define PS1_SPU_RAM_SIZE        (512 * 1024)        // 512KB SPU Sound RAM
#define PS1_BIOS_SIZE           (512 * 1024)        // 512KB BIOS ROM

// Memory Map Bases
#define PS1_KUSEG_BASE          0x00000000
#define PS1_KSEG0_BASE          0x80000000
#define PS1_KSEG1_BASE          0xA0000000
#define PS1_KSEG2_BASE          0xC0000000

#define PS1_RAM_MIRROR_MASK     0x001FFFFF
#define PS1_SCRATCHPAD_BASE     0x1F800000
#define PS1_IO_BASE             0x1F801000
#define PS1_BIOS_BASE           0x1FC00000

// Standard PS1 Button Flags (active low in hardware, normalized here)
typedef enum {
    PS1_BTN_SELECT   = (1 << 0),
    PS1_BTN_L3       = (1 << 1),
    PS1_BTN_R3       = (1 << 2),
    PS1_BTN_START    = (1 << 3),
    PS1_BTN_UP       = (1 << 4),
    PS1_BTN_RIGHT    = (1 << 5),
    PS1_BTN_DOWN     = (1 << 6),
    PS1_BTN_LEFT     = (1 << 7),
    PS1_BTN_L2       = (1 << 8),
    PS1_BTN_R2       = (1 << 9),
    PS1_BTN_L1       = (1 << 10),
    PS1_BTN_R1       = (1 << 11),
    PS1_BTN_TRIANGLE = (1 << 12),
    PS1_BTN_CIRCLE   = (1 << 13),
    PS1_BTN_CROSS    = (1 << 14),
    PS1_BTN_SQUARE   = (1 << 15),
} PS1Button;

// Forward Declarations
typedef struct PS1Context PS1Context;
typedef struct PS1CPU PS1CPU;
typedef struct PS1Memory PS1Memory;
typedef struct PS1GPU PS1GPU;
typedef struct PS1GTE PS1GTE;
typedef struct PS1SPU PS1SPU;
typedef struct PS1DMA PS1DMA;
typedef struct PS1Timers PS1Timers;
typedef struct PS1CDROM PS1CDROM;
typedef struct PS1MDEC PS1MDEC;
typedef struct PS1BIOS PS1BIOS;
typedef struct PS1Input PS1Input;

// Function pointer for recompiled functions
typedef void (*PS1RecompiledFunc)(PS1Context* ctx);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_TYPES_H
