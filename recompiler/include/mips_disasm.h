#ifndef RECOMP_MIPS_DISASM_H
#define RECOMP_MIPS_DISASM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MIPS_OP_UNKNOWN = 0,
    // ALU
    MIPS_OP_ADD, MIPS_OP_ADDU, MIPS_OP_SUB, MIPS_OP_SUBU,
    MIPS_OP_AND, MIPS_OP_OR, MIPS_OP_XOR, MIPS_OP_NOR,
    MIPS_OP_SLT, MIPS_OP_SLTU,
    MIPS_OP_ADDI, MIPS_OP_ADDIU, MIPS_OP_SLTI, MIPS_OP_SLTIU,
    MIPS_OP_ANDI, MIPS_OP_ORI, MIPS_OP_XORI, MIPS_OP_LUI,
    // Shifts
    MIPS_OP_SLL, MIPS_OP_SRL, MIPS_OP_SRA,
    MIPS_OP_SLLV, MIPS_OP_SRLV, MIPS_OP_SRAV,
    // Multiply / Divide
    MIPS_OP_MULT, MIPS_OP_MULTU, MIPS_OP_DIV, MIPS_OP_DIVU,
    MIPS_OP_MFHI, MIPS_OP_MFLO, MIPS_OP_MTHI, MIPS_OP_MTLO,
    // Branches & Jumps
    MIPS_OP_J, MIPS_OP_JAL, MIPS_OP_JR, MIPS_OP_JALR,
    MIPS_OP_BEQ, MIPS_OP_BNE, MIPS_OP_BLEZ, MIPS_OP_BGTZ,
    MIPS_OP_BLTZ, MIPS_OP_BGEZ, MIPS_OP_BLTZAL, MIPS_OP_BGEZAL,
    // Memory
    MIPS_OP_LB, MIPS_OP_LH, MIPS_OP_LWL, MIPS_OP_LW,
    MIPS_OP_LBU, MIPS_OP_LHU, MIPS_OP_LWR,
    MIPS_OP_SB, MIPS_OP_SH, MIPS_OP_SWL, MIPS_OP_SW, MIPS_OP_SWR,
    // System / Exceptions
    MIPS_OP_SYSCALL, MIPS_OP_BREAK,
    // Coprocessor 0
    MIPS_OP_MFC0, MIPS_OP_MTC0, MIPS_OP_RFE,
    // Coprocessor 2 (GTE)
    MIPS_OP_MFC2, MIPS_OP_MTC2, MIPS_OP_CFC2, MIPS_OP_CTC2,
    MIPS_OP_LWC2, MIPS_OP_SWC2,
    MIPS_OP_COP2_CMD
} MipsOpcode;

typedef struct {
    uint32_t raw;
    uint32_t pc;
    MipsOpcode id;
    uint8_t opcode;
    uint8_t rs, rt, rd, sa, funct;
    uint16_t imm;
    int32_t simm;
    uint32_t target;
    bool is_branch;
    bool is_jump;
    bool is_call;
    bool is_return;
    bool has_delay_slot;
} MipsInsn;

void mips_decode_instruction(uint32_t raw, uint32_t pc, MipsInsn* insn);
const char* mips_opcode_name(MipsOpcode id);
const char* mips_reg_name(int reg);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_MIPS_DISASM_H
