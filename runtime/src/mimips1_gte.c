#include "mimips1_gte.h"
#include <string.h>

void ps1_gte_init(PS1GTE* gte) {
    memset(gte, 0, sizeof(PS1GTE));
    gte->h = 240; // Default screen distance
}

void ps1_gte_reset(PS1GTE* gte) {
    memset(gte, 0, sizeof(PS1GTE));
    gte->h = 240;
}

uint32_t ps1_gte_get_data(PS1GTE* gte, uint32_t reg) {
    switch (reg) {
        case 0: return (uint16_t)gte->v[0][0] | ((uint32_t)(uint16_t)gte->v[0][1] << 16);
        case 1: return (uint16_t)gte->v[0][2];
        case 2: return (uint16_t)gte->v[1][0] | ((uint32_t)(uint16_t)gte->v[1][1] << 16);
        case 3: return (uint16_t)gte->v[1][2];
        case 4: return (uint16_t)gte->v[2][0] | ((uint32_t)(uint16_t)gte->v[2][1] << 16);
        case 5: return (uint16_t)gte->v[2][2];
        case 6: return *(uint32_t*)gte->rgbc;
        case 7: return gte->otz;
        case 8: return (uint16_t)gte->ir[0];
        case 9: return (uint16_t)gte->ir[1];
        case 10: return (uint16_t)gte->ir[2];
        case 11: return (uint16_t)gte->ir[3];
        case 12: return *(uint32_t*)&gte->sxy[0];
        case 13: return *(uint32_t*)&gte->sxy[1];
        case 14: return *(uint32_t*)&gte->sxy[2];
        case 15: return *(uint32_t*)&gte->sxy[3];
        case 16: return gte->sz[0];
        case 17: return gte->sz[1];
        case 18: return gte->sz[2];
        case 19: return gte->sz[3];
        case 20: return *(uint32_t*)gte->rgb[0];
        case 21: return *(uint32_t*)gte->rgb[1];
        case 22: return *(uint32_t*)gte->rgb[2];
        case 24: return (uint32_t)gte->mac[0];
        case 25: return (uint32_t)gte->mac[1];
        case 26: return (uint32_t)gte->mac[2];
        case 27: return (uint32_t)gte->mac[3];
        case 28: return gte->irgb;
        case 29: return gte->orgb;
        case 30: return (uint32_t)gte->lzcs;
        case 31: return (uint32_t)gte->lzcr;
        default: return 0;
    }
}

void ps1_gte_set_data(PS1GTE* gte, uint32_t reg, uint32_t val) {
    switch (reg) {
        case 0:
            gte->v[0][0] = (int16_t)(val & 0xFFFF);
            gte->v[0][1] = (int16_t)(val >> 16);
            break;
        case 1:
            gte->v[0][2] = (int16_t)(val & 0xFFFF);
            break;
        case 2:
            gte->v[1][0] = (int16_t)(val & 0xFFFF);
            gte->v[1][1] = (int16_t)(val >> 16);
            break;
        case 3:
            gte->v[1][2] = (int16_t)(val & 0xFFFF);
            break;
        case 4:
            gte->v[2][0] = (int16_t)(val & 0xFFFF);
            gte->v[2][1] = (int16_t)(val >> 16);
            break;
        case 5:
            gte->v[2][2] = (int16_t)(val & 0xFFFF);
            break;
        case 6: *(uint32_t*)gte->rgbc = val; break;
        case 7: gte->otz = (uint16_t)val; break;
        case 8: gte->ir[0] = (int16_t)val; break;
        case 9: gte->ir[1] = (int16_t)val; break;
        case 10: gte->ir[2] = (int16_t)val; break;
        case 11: gte->ir[3] = (int16_t)val; break;
        case 12: *(uint32_t*)&gte->sxy[0] = val; break;
        case 13: *(uint32_t*)&gte->sxy[1] = val; break;
        case 14: *(uint32_t*)&gte->sxy[2] = val; break;
        case 15: *(uint32_t*)&gte->sxy[3] = val; break;
        case 16: gte->sz[0] = (uint16_t)val; break;
        case 17: gte->sz[1] = (uint16_t)val; break;
        case 18: gte->sz[2] = (uint16_t)val; break;
        case 19: gte->sz[3] = (uint16_t)val; break;
        case 24: gte->mac[0] = (int32_t)val; break;
        case 25: gte->mac[1] = (int32_t)val; break;
        case 26: gte->mac[2] = (int32_t)val; break;
        case 27: gte->mac[3] = (int32_t)val; break;
        case 30: gte->lzcs = (int32_t)val; break;
        case 31: gte->lzcr = (int32_t)val; break;
    }
}

uint32_t ps1_gte_get_ctrl(PS1GTE* gte, uint32_t reg) {
    switch (reg) {
        case 0: return (uint16_t)gte->rot_matrix[0][0] | ((uint32_t)(uint16_t)gte->rot_matrix[0][1] << 16);
        case 1: return (uint16_t)gte->rot_matrix[0][2] | ((uint32_t)(uint16_t)gte->rot_matrix[1][0] << 16);
        case 2: return (uint16_t)gte->rot_matrix[1][1] | ((uint32_t)(uint16_t)gte->rot_matrix[1][2] << 16);
        case 3: return (uint16_t)gte->rot_matrix[2][0] | ((uint32_t)(uint16_t)gte->rot_matrix[2][1] << 16);
        case 4: return (uint16_t)gte->rot_matrix[2][2];
        case 5: return (uint32_t)gte->trans_vector[0];
        case 6: return (uint32_t)gte->trans_vector[1];
        case 7: return (uint32_t)gte->trans_vector[2];
        case 24: return (uint32_t)gte->offset_x;
        case 25: return (uint32_t)gte->offset_y;
        case 26: return (uint32_t)(int16_t)gte->h;
        case 27: return (uint32_t)gte->dqa;
        case 28: return (uint32_t)gte->dqb;
        case 29: return (uint32_t)gte->zsf3;
        case 30: return (uint32_t)gte->zsf4;
        case 31: return gte->flag;
        default: return 0;
    }
}

void ps1_gte_set_ctrl(PS1GTE* gte, uint32_t reg, uint32_t val) {
    switch (reg) {
        case 0:
            gte->rot_matrix[0][0] = (int16_t)(val & 0xFFFF);
            gte->rot_matrix[0][1] = (int16_t)(val >> 16);
            break;
        case 1:
            gte->rot_matrix[0][2] = (int16_t)(val & 0xFFFF);
            gte->rot_matrix[1][0] = (int16_t)(val >> 16);
            break;
        case 2:
            gte->rot_matrix[1][1] = (int16_t)(val & 0xFFFF);
            gte->rot_matrix[1][2] = (int16_t)(val >> 16);
            break;
        case 3:
            gte->rot_matrix[2][0] = (int16_t)(val & 0xFFFF);
            gte->rot_matrix[2][1] = (int16_t)(val >> 16);
            break;
        case 4:
            gte->rot_matrix[2][2] = (int16_t)(val & 0xFFFF);
            break;
        case 5: gte->trans_vector[0] = (int32_t)val; break;
        case 6: gte->trans_vector[1] = (int32_t)val; break;
        case 7: gte->trans_vector[2] = (int32_t)val; break;
        case 24: gte->offset_x = (int32_t)val; break;
        case 25: gte->offset_y = (int32_t)val; break;
        case 26: gte->h = (uint16_t)val; break;
        case 27: gte->dqa = (int16_t)val; break;
        case 28: gte->dqb = (int32_t)val; break;
        case 29: gte->zsf3 = (int16_t)val; break;
        case 30: gte->zsf4 = (int16_t)val; break;
        case 31: gte->flag = val; break;
    }
}

// Transform single vertex helper
static void gte_transform_vector(PS1GTE* gte, int v_idx, int32_t* out_x, int32_t* out_y, int32_t* out_z) {
    int16_t vx = gte->v[v_idx][0];
    int16_t vy = gte->v[v_idx][1];
    int16_t vz = gte->v[v_idx][2];

    int64_t x = ((int64_t)gte->trans_vector[0] << 12) +
                ((int64_t)gte->rot_matrix[0][0] * vx + (int64_t)gte->rot_matrix[0][1] * vy + (int64_t)gte->rot_matrix[0][2] * vz);
    int64_t y = ((int64_t)gte->trans_vector[1] << 12) +
                ((int64_t)gte->rot_matrix[1][0] * vx + (int64_t)gte->rot_matrix[1][1] * vy + (int64_t)gte->rot_matrix[1][2] * vz);
    int64_t z = ((int64_t)gte->trans_vector[2] << 12) +
                ((int64_t)gte->rot_matrix[2][0] * vx + (int64_t)gte->rot_matrix[2][1] * vy + (int64_t)gte->rot_matrix[2][2] * vz);

    *out_x = (int32_t)(x >> 12);
    *out_y = (int32_t)(y >> 12);
    *out_z = (int32_t)(z >> 12);
}

// RTPS: Perspective Transform Single
void ps1_gte_rtps(PS1GTE* gte, uint32_t instruction) {
    int32_t x, y, z;
    gte_transform_vector(gte, 0, &x, &y, &z);

    // Push SZ FIFO
    gte->sz[0] = gte->sz[1];
    gte->sz[1] = gte->sz[2];
    gte->sz[2] = gte->sz[3];
    uint16_t sz3 = (z < 0) ? 0 : (z > 0xFFFF ? 0xFFFF : (uint16_t)z);
    gte->sz[3] = sz3;

    // Perspective Division
    int32_t p_z = (z <= 0) ? 1 : z;
    int64_t sx = ((int64_t)x * gte->h) / p_z + (gte->offset_x >> 16);
    int64_t sy = ((int64_t)y * gte->h) / p_z + (gte->offset_y >> 16);

    // Push SXY FIFO
    gte->sxy[0][0] = gte->sxy[1][0];
    gte->sxy[0][1] = gte->sxy[1][1];
    gte->sxy[1][0] = gte->sxy[2][0];
    gte->sxy[1][1] = gte->sxy[2][1];
    gte->sxy[2][0] = (int16_t)sx;
    gte->sxy[2][1] = (int16_t)sy;
}

// RTPT: Perspective Transform Triple
void ps1_gte_rtpt(PS1GTE* gte, uint32_t instruction) {
    for (int i = 0; i < 3; i++) {
        int32_t x, y, z;
        gte_transform_vector(gte, i, &x, &y, &z);

        uint16_t sz_val = (z < 0) ? 0 : (z > 0xFFFF ? 0xFFFF : (uint16_t)z);
        gte->sz[i + 1] = sz_val;

        int32_t p_z = (z <= 0) ? 1 : z;
        int64_t sx = ((int64_t)x * gte->h) / p_z + (gte->offset_x >> 16);
        int64_t sy = ((int64_t)y * gte->h) / p_z + (gte->offset_y >> 16);

        gte->sxy[i][0] = (int16_t)sx;
        gte->sxy[i][1] = (int16_t)sy;
    }
}

// NCLIP: Normal Clip (Backface culling)
void ps1_gte_nclip(PS1GTE* gte) {
    int64_t x0 = gte->sxy[0][0];
    int64_t y0 = gte->sxy[0][1];
    int64_t x1 = gte->sxy[1][0];
    int64_t y1 = gte->sxy[1][1];
    int64_t x2 = gte->sxy[2][0];
    int64_t y2 = gte->sxy[2][1];

    int64_t nclip = x0 * (y1 - y2) + x1 * (y2 - y0) + x2 * (y0 - y1);
    gte->mac[0] = (int32_t)nclip;
}

// AVSZ3 / AVSZ4: Average Z
void ps1_gte_avsz3(PS1GTE* gte) {
    uint32_t sum = gte->sz[1] + gte->sz[2] + gte->sz[3];
    uint32_t avg = (sum * (uint32_t)gte->zsf3) >> 12;
    gte->otz = (avg > 0xFFFF) ? 0xFFFF : (uint16_t)avg;
}

void ps1_gte_avsz4(PS1GTE* gte) {
    uint32_t sum = gte->sz[0] + gte->sz[1] + gte->sz[2] + gte->sz[3];
    uint32_t avg = (sum * (uint32_t)gte->zsf4) >> 12;
    gte->otz = (avg > 0xFFFF) ? 0xFFFF : (uint16_t)avg;
}

void ps1_gte_dpcs(PS1GTE* gte, uint32_t instruction) {
    // Depth Cueing Single: shifts RGB FIFO
    gte->rgb[0][0] = gte->rgb[1][0];
    gte->rgb[0][1] = gte->rgb[1][1];
    gte->rgb[0][2] = gte->rgb[1][2];
    gte->rgb[0][3] = gte->rgb[1][3];

    gte->rgb[1][0] = gte->rgb[2][0];
    gte->rgb[1][1] = gte->rgb[2][1];
    gte->rgb[1][2] = gte->rgb[2][2];
    gte->rgb[1][3] = gte->rgb[2][3];

    gte->rgb[2][0] = gte->rgbc[0];
    gte->rgb[2][1] = gte->rgbc[1];
    gte->rgb[2][2] = gte->rgbc[2];
    gte->rgb[2][3] = gte->rgbc[3];
}

void ps1_gte_dpct(PS1GTE* gte, uint32_t instruction) {
    // Depth cue 3 colors
    for (int i = 0; i < 3; i++) {
        gte->rgb[i][0] = gte->rgbc[0];
        gte->rgb[i][1] = gte->rgbc[1];
        gte->rgb[i][2] = gte->rgbc[2];
        gte->rgb[i][3] = gte->rgbc[3];
    }
}

void ps1_gte_intpl(PS1GTE* gte, uint32_t instruction) {
    // Linear interpolation
}

void ps1_gte_mvmva(PS1GTE* gte, uint32_t instruction) {
    int32_t x, y, z;
    gte_transform_vector(gte, 0, &x, &y, &z);
    gte->mac[1] = x;
    gte->mac[2] = y;
    gte->mac[3] = z;
    gte->ir[1] = (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
    gte->ir[2] = (int16_t)(y > 32767 ? 32767 : (y < -32768 ? -32768 : y));
    gte->ir[3] = (int16_t)(z > 32767 ? 32767 : (z < -32768 ? -32768 : z));
}

void ps1_gte_sqr(PS1GTE* gte, uint32_t instruction) {
    gte->mac[1] = (int32_t)gte->ir[1] * (int32_t)gte->ir[1];
    gte->mac[2] = (int32_t)gte->ir[2] * (int32_t)gte->ir[2];
    gte->mac[3] = (int32_t)gte->ir[3] * (int32_t)gte->ir[3];
}

void ps1_gte_op(PS1GTE* gte, uint32_t instruction) {}
void ps1_gte_gpf(PS1GTE* gte, uint32_t instruction) {}
void ps1_gte_gpl(PS1GTE* gte, uint32_t instruction) {}
void ps1_gte_ncds(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpcs(gte, instruction); }
void ps1_gte_ncdt(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpct(gte, instruction); }
void ps1_gte_nccs(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpcs(gte, instruction); }
void ps1_gte_ncct(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpct(gte, instruction); }
void ps1_gte_cdp(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpcs(gte, instruction); }
void ps1_gte_cc(PS1GTE* gte, uint32_t instruction) {}
void ps1_gte_ncs(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpcs(gte, instruction); }
void ps1_gte_nct(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpct(gte, instruction); }
void ps1_gte_dcpl(PS1GTE* gte, uint32_t instruction) { ps1_gte_dpcs(gte, instruction); }

void ps1_gte_execute_command(PS1GTE* gte, uint32_t opcode) {
    uint32_t cmd = opcode & 0x3F;
    switch (cmd) {
        case 0x01: ps1_gte_rtps(gte, opcode); break;
        case 0x06: ps1_gte_nclip(gte); break;
        case 0x0C: ps1_gte_op(gte, opcode); break;
        case 0x10: ps1_gte_dpcs(gte, opcode); break;
        case 0x11: ps1_gte_intpl(gte, opcode); break;
        case 0x12: ps1_gte_mvmva(gte, opcode); break;
        case 0x13: ps1_gte_ncds(gte, opcode); break;
        case 0x14: ps1_gte_cdp(gte, opcode); break;
        case 0x16: ps1_gte_ncdt(gte, opcode); break;
        case 0x1B: ps1_gte_nccs(gte, opcode); break;
        case 0x1C: ps1_gte_cc(gte, opcode); break;
        case 0x1E: ps1_gte_ncs(gte, opcode); break;
        case 0x20: ps1_gte_nct(gte, opcode); break;
        case 0x28: ps1_gte_sqr(gte, opcode); break;
        case 0x29: ps1_gte_dcpl(gte, opcode); break;
        case 0x2A: ps1_gte_dpct(gte, opcode); break;
        case 0x2D: ps1_gte_avsz3(gte); break;
        case 0x2E: ps1_gte_avsz4(gte); break;
        case 0x30: ps1_gte_rtpt(gte, opcode); break;
        case 0x3D: ps1_gte_gpf(gte, opcode); break;
        case 0x3E: ps1_gte_gpl(gte, opcode); break;
        case 0x3F: ps1_gte_ncct(gte, opcode); break;
        default: break;
    }
}
