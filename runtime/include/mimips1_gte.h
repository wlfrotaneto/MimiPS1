#ifndef MIMIPS1_GTE_H
#define MIMIPS1_GTE_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Vector & Matrix structures
typedef struct {
    int16_t x, y, z;
} GTEVector16;

typedef struct {
    int32_t x, y, z;
} GTEVector32;

typedef struct {
    int16_t m[3][3];
} GTEMatrix;

struct PS1GTE {
    // Data Registers (32 registers)
    int16_t  v[3][3];           // V0, V1, V2: Vectors (X, Y, Z)
    uint8_t  rgbc[4];           // R, G, B, CODE
    uint16_t otz;               // Average Z (Ordering Table)
    int16_t  ir[4];             // Intermediate values IR0..IR3
    int16_t  sxy[4][2];         // Screen coords FIFO: SXY0, SXY1, SXY2, SXYP (X, Y)
    uint16_t sz[4];             // Screen depth FIFO: SZ0, SZ1, SZ2, SZ3
    uint8_t  rgb[3][4];         // Color FIFO: RGB0, RGB1, RGB2
    int32_t  mac[4];            // Accumulators: MAC0..MAC3
    uint32_t irgb;
    uint32_t orgb;
    int32_t  lzcs;
    int32_t  lzcr;

    // Control Registers (32 registers)
    int16_t  rot_matrix[3][3];  // 3x3 Rotation Matrix
    int32_t  trans_vector[3];   // Translation Vector (TRX, TRY, TRZ)
    int16_t  light_matrix[3][3];// Light Matrix
    int32_t  back_color[3];     // Background Color (RBK, GBK, BBK)
    int16_t  color_matrix[3][3];// Color Matrix
    int32_t  far_color[3];      // Far Color (RFC, GFC, BFC)
    int32_t  offset_x;          // Screen Offset X (OFX)
    int32_t  offset_y;          // Screen Offset Y (OFY)
    uint16_t h;                 // Projection Plane Distance (FOV)
    int16_t  dqa;               // Depth Cueing A
    int32_t  dqb;               // Depth Cueing B
    int16_t  zsf3;              // Z scale factor 3
    int16_t  zsf4;              // Z scale factor 4
    uint32_t flag;              // Error / Saturation Flags
};

void ps1_gte_init(PS1GTE* gte);
void ps1_gte_reset(PS1GTE* gte);

// Register access
uint32_t ps1_gte_get_data(PS1GTE* gte, uint32_t reg);
void     ps1_gte_set_data(PS1GTE* gte, uint32_t reg, uint32_t val);
uint32_t ps1_gte_get_ctrl(PS1GTE* gte, uint32_t reg);
void     ps1_gte_set_ctrl(PS1GTE* gte, uint32_t reg, uint32_t val);

// GTE Commands (invoked directly from recompiled code)
void ps1_gte_rtps(PS1GTE* gte, uint32_t instruction); // Perspective Transform Single
void ps1_gte_rtpt(PS1GTE* gte, uint32_t instruction); // Perspective Transform Triple
void ps1_gte_nclip(PS1GTE* gte);                      // Normal Clip
void ps1_gte_dpcs(PS1GTE* gte, uint32_t instruction); // Depth Cue Color Single
void ps1_gte_dpct(PS1GTE* gte, uint32_t instruction); // Depth Cue Color Triple
void ps1_gte_intpl(PS1GTE* gte, uint32_t instruction);// Interpolate
void ps1_gte_mvmva(PS1GTE* gte, uint32_t instruction);// Matrix Multiply Vector Add
void ps1_gte_avsz3(PS1GTE* gte);                      // Average 3 Z values
void ps1_gte_avsz4(PS1GTE* gte);                      // Average 4 Z values
void ps1_gte_sqr(PS1GTE* gte, uint32_t instruction);  // Square
void ps1_gte_op(PS1GTE* gte, uint32_t instruction);   // Outer Product (Cross Product)
void ps1_gte_gpf(PS1GTE* gte, uint32_t instruction);  // General Purpose Fraction
void ps1_gte_gpl(PS1GTE* gte, uint32_t instruction);  // General Purpose Leading
void ps1_gte_ncds(PS1GTE* gte, uint32_t instruction); // Normal Color Depth Cue Single
void ps1_gte_ncdt(PS1GTE* gte, uint32_t instruction); // Normal Color Depth Cue Triple
void ps1_gte_nccs(PS1GTE* gte, uint32_t instruction); // Normal Color Color Single
void ps1_gte_ncct(PS1GTE* gte, uint32_t instruction); // Normal Color Color Triple
void ps1_gte_cdp(PS1GTE* gte, uint32_t instruction);  // Color Depth Cue
void ps1_gte_cc(PS1GTE* gte, uint32_t instruction);   // Color Color
void ps1_gte_ncs(PS1GTE* gte, uint32_t instruction);  // Normal Color Single
void ps1_gte_nct(PS1GTE* gte, uint32_t instruction);  // Normal Color Triple
void ps1_gte_dcpl(PS1GTE* gte, uint32_t instruction); // Depth Cue Color Light

void ps1_gte_execute_command(PS1GTE* gte, uint32_t opcode);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_GTE_H
