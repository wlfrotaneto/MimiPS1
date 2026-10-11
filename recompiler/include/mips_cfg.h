#ifndef RECOMP_MIPS_CFG_H
#define RECOMP_MIPS_CFG_H

#include "mips_disasm.h"
#include "ps1_exe.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MipsBlock {
    uint32_t start_pc;
    uint32_t end_pc;
    int      num_insns;
    MipsInsn* insns;
    
    // Jump / Branch information
    bool     has_branch;
    bool     has_call;
    bool     is_indirect;
    uint32_t branch_target;
    uint32_t fallthrough_target;
    
    struct MipsBlock* next;
} MipsBlock;

typedef struct MipsFunction {
    uint32_t   entry_pc;
    uint32_t   end_pc;
    char       name[64];
    int        block_count;
    MipsBlock* first_block;
    MipsBlock* last_block;
    
    struct MipsFunction* next;
} MipsFunction;

typedef struct {
    MipsFunction* first_func;
    int           func_count;
    uint32_t      entry_pc;
    uint32_t      base_addr;
    uint32_t      code_size;
} MipsProgram;

MipsProgram* mips_analyze_program(const PS1Executable* exe);
void         mips_free_program(MipsProgram* prog);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_MIPS_CFG_H
