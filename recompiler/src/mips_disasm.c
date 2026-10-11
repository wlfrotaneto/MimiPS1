#include "mips_disasm.h"
#include <string.h>

static const char* reg_names[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"
};

const char* mips_reg_name(int reg) {
    if (reg >= 0 && reg < 32) return reg_names[reg];
    return "unk";
}

void mips_decode_instruction(uint32_t raw, uint32_t pc, MipsInsn* insn) {
    memset(insn, 0, sizeof(MipsInsn));
    insn->raw = raw;
    insn->pc = pc;

    insn->opcode = (raw >> 26) & 0x3F;
    insn->rs     = (raw >> 21) & 0x1F;
    insn->rt     = (raw >> 16) & 0x1F;
    insn->rd     = (raw >> 11) & 0x1F;
    insn->sa     = (raw >> 6) & 0x1F;
    insn->funct  = raw & 0x3F;
    insn->imm    = raw & 0xFFFF;
    insn->simm   = (int32_t)(int16_t)insn->imm;

    if (insn->opcode == 0x00) { // SPECIAL (R-type)
        switch (insn->funct) {
            case 0x00: insn->id = MIPS_OP_SLL; break;
            case 0x02: insn->id = MIPS_OP_SRL; break;
            case 0x03: insn->id = MIPS_OP_SRA; break;
            case 0x04: insn->id = MIPS_OP_SLLV; break;
            case 0x06: insn->id = MIPS_OP_SRLV; break;
            case 0x07: insn->id = MIPS_OP_SRAV; break;
            case 0x08: 
                insn->id = MIPS_OP_JR; 
                insn->is_jump = true; 
                insn->has_delay_slot = true;
                if (insn->rs == 31) insn->is_return = true;
                break;
            case 0x09: 
                insn->id = MIPS_OP_JALR; 
                insn->is_jump = true; 
                insn->is_call = true;
                insn->has_delay_slot = true;
                break;
            case 0x0C: insn->id = MIPS_OP_SYSCALL; break;
            case 0x0D: insn->id = MIPS_OP_BREAK; break;
            case 0x10: insn->id = MIPS_OP_MFHI; break;
            case 0x11: insn->id = MIPS_OP_MTHI; break;
            case 0x12: insn->id = MIPS_OP_MFLO; break;
            case 0x13: insn->id = MIPS_OP_MTLO; break;
            case 0x18: insn->id = MIPS_OP_MULT; break;
            case 0x19: insn->id = MIPS_OP_MULTU; break;
            case 0x1A: insn->id = MIPS_OP_DIV; break;
            case 0x1B: insn->id = MIPS_OP_DIVU; break;
            case 0x20: insn->id = MIPS_OP_ADD; break;
            case 0x21: insn->id = MIPS_OP_ADDU; break;
            case 0x22: insn->id = MIPS_OP_SUB; break;
            case 0x23: insn->id = MIPS_OP_SUBU; break;
            case 0x24: insn->id = MIPS_OP_AND; break;
            case 0x25: insn->id = MIPS_OP_OR; break;
            case 0x26: insn->id = MIPS_OP_XOR; break;
            case 0x27: insn->id = MIPS_OP_NOR; break;
            case 0x2A: insn->id = MIPS_OP_SLT; break;
            case 0x2B: insn->id = MIPS_OP_SLTU; break;
            default: insn->id = MIPS_OP_UNKNOWN; break;
        }
    } else if (insn->opcode == 0x01) { // REGIMM
        switch (insn->rt) {
            case 0x00: 
                insn->id = MIPS_OP_BLTZ; 
                insn->is_branch = true; 
                insn->has_delay_slot = true;
                insn->target = pc + 4 + (insn->simm << 2);
                break;
            case 0x01: 
                insn->id = MIPS_OP_BGEZ; 
                insn->is_branch = true; 
                insn->has_delay_slot = true;
                insn->target = pc + 4 + (insn->simm << 2);
                break;
            case 0x10: 
                insn->id = MIPS_OP_BLTZAL; 
                insn->is_branch = true; 
                insn->is_call = true;
                insn->has_delay_slot = true;
                insn->target = pc + 4 + (insn->simm << 2);
                break;
            case 0x11: 
                insn->id = MIPS_OP_BGEZAL; 
                insn->is_branch = true; 
                insn->is_call = true;
                insn->has_delay_slot = true;
                insn->target = pc + 4 + (insn->simm << 2);
                break;
            default: insn->id = MIPS_OP_UNKNOWN; break;
        }
    } else if (insn->opcode == 0x02) { // J
        insn->id = MIPS_OP_J;
        insn->is_jump = true;
        insn->has_delay_slot = true;
        insn->target = ((pc + 4) & 0xF0000000) | ((raw & 0x03FFFFFF) << 2);
    } else if (insn->opcode == 0x03) { // JAL
        insn->id = MIPS_OP_JAL;
        insn->is_jump = true;
        insn->is_call = true;
        insn->has_delay_slot = true;
        insn->target = ((pc + 4) & 0xF0000000) | ((raw & 0x03FFFFFF) << 2);
    } else if (insn->opcode == 0x04) {
        insn->id = MIPS_OP_BEQ; insn->is_branch = true; insn->has_delay_slot = true;
        insn->target = pc + 4 + (insn->simm << 2);
    } else if (insn->opcode == 0x05) {
        insn->id = MIPS_OP_BNE; insn->is_branch = true; insn->has_delay_slot = true;
        insn->target = pc + 4 + (insn->simm << 2);
    } else if (insn->opcode == 0x06) {
        insn->id = MIPS_OP_BLEZ; insn->is_branch = true; insn->has_delay_slot = true;
        insn->target = pc + 4 + (insn->simm << 2);
    } else if (insn->opcode == 0x07) {
        insn->id = MIPS_OP_BGTZ; insn->is_branch = true; insn->has_delay_slot = true;
        insn->target = pc + 4 + (insn->simm << 2);
    } else if (insn->opcode == 0x08) {
        insn->id = MIPS_OP_ADDI;
    } else if (insn->opcode == 0x09) {
        insn->id = MIPS_OP_ADDIU;
    } else if (insn->opcode == 0x0A) {
        insn->id = MIPS_OP_SLTI;
    } else if (insn->opcode == 0x0B) {
        insn->id = MIPS_OP_SLTIU;
    } else if (insn->opcode == 0x0C) {
        insn->id = MIPS_OP_ANDI;
    } else if (insn->opcode == 0x0D) {
        insn->id = MIPS_OP_ORI;
    } else if (insn->opcode == 0x0E) {
        insn->id = MIPS_OP_XORI;
    } else if (insn->opcode == 0x0F) {
        insn->id = MIPS_OP_LUI;
    } else if (insn->opcode == 0x10) { // COP0
        if (insn->rs == 0x00) insn->id = MIPS_OP_MFC0;
        else if (insn->rs == 0x04) insn->id = MIPS_OP_MTC0;
        else if (insn->funct == 0x10) insn->id = MIPS_OP_RFE;
    } else if (insn->opcode == 0x12) { // COP2 (GTE)
        if (raw & (1 << 25)) {
            insn->id = MIPS_OP_COP2_CMD;
        } else {
            if (insn->rs == 0x00) insn->id = MIPS_OP_MFC2;
            else if (insn->rs == 0x04) insn->id = MIPS_OP_MTC2;
            else if (insn->rs == 0x02) insn->id = MIPS_OP_CFC2;
            else if (insn->rs == 0x06) insn->id = MIPS_OP_CTC2;
        }
    } else if (insn->opcode == 0x20) {
        insn->id = MIPS_OP_LB;
    } else if (insn->opcode == 0x21) {
        insn->id = MIPS_OP_LH;
    } else if (insn->opcode == 0x22) {
        insn->id = MIPS_OP_LWL;
    } else if (insn->opcode == 0x23) {
        insn->id = MIPS_OP_LW;
    } else if (insn->opcode == 0x24) {
        insn->id = MIPS_OP_LBU;
    } else if (insn->opcode == 0x25) {
        insn->id = MIPS_OP_LHU;
    } else if (insn->opcode == 0x26) {
        insn->id = MIPS_OP_LWR;
    } else if (insn->opcode == 0x28) {
        insn->id = MIPS_OP_SB;
    } else if (insn->opcode == 0x29) {
        insn->id = MIPS_OP_SH;
    } else if (insn->opcode == 0x2A) {
        insn->id = MIPS_OP_SWL;
    } else if (insn->opcode == 0x2B) {
        insn->id = MIPS_OP_SW;
    } else if (insn->opcode == 0x2E) {
        insn->id = MIPS_OP_SWR;
    } else if (insn->opcode == 0x32) {
        insn->id = MIPS_OP_LWC2;
    } else if (insn->opcode == 0x3A) {
        insn->id = MIPS_OP_SWC2;
    } else {
        insn->id = MIPS_OP_UNKNOWN;
    }
}
