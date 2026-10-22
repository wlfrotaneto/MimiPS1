#include "c_emitter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_func_in_program(const MipsProgram* prog, uint32_t target) {
    if (!prog) return false;
    MipsFunction* cur = prog->first_func;
    while (cur) {
        if (cur->entry_pc == target) return true;
        cur = cur->next;
    }
    return false;
}

static void emit_instruction_c(FILE* f, const MipsInsn* insn) {
    switch (insn->id) {
        // ALU
        case MIPS_OP_ADD:
        case MIPS_OP_ADDU:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) + R(%u);\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_SUB:
        case MIPS_OP_SUBU:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) - R(%u);\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_AND:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) & R(%u);\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_OR:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) | R(%u);\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_XOR:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) ^ R(%u);\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_NOR:
            if (insn->rd != 0) fprintf(f, "    R(%u) = ~(R(%u) | R(%u));\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_SLT:
            if (insn->rd != 0) fprintf(f, "    R(%u) = ((int32_t)R(%u) < (int32_t)R(%u)) ? 1 : 0;\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_SLTU:
            if (insn->rd != 0) fprintf(f, "    R(%u) = (R(%u) < R(%u)) ? 1 : 0;\n", insn->rd, insn->rs, insn->rt);
            break;
        case MIPS_OP_ADDI:
        case MIPS_OP_ADDIU:
            if (insn->rt != 0) fprintf(f, "    R(%u) = R(%u) + (%d);\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_SLTI:
            if (insn->rt != 0) fprintf(f, "    R(%u) = ((int32_t)R(%u) < (%d)) ? 1 : 0;\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_SLTIU:
            if (insn->rt != 0) fprintf(f, "    R(%u) = (R(%u) < (uint32_t)(%d)) ? 1 : 0;\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_ANDI:
            if (insn->rt != 0) fprintf(f, "    R(%u) = R(%u) & 0x%04X;\n", insn->rt, insn->rs, insn->imm);
            break;
        case MIPS_OP_ORI:
            if (insn->rt != 0) fprintf(f, "    R(%u) = R(%u) | 0x%04X;\n", insn->rt, insn->rs, insn->imm);
            break;
        case MIPS_OP_XORI:
            if (insn->rt != 0) fprintf(f, "    R(%u) = R(%u) ^ 0x%04X;\n", insn->rt, insn->rs, insn->imm);
            break;
        case MIPS_OP_LUI:
            if (insn->rt != 0) fprintf(f, "    R(%u) = 0x%04X0000;\n", insn->rt, insn->imm);
            break;

        // Shifts
        case MIPS_OP_SLL:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) << %u;\n", insn->rd, insn->rt, insn->sa);
            break;
        case MIPS_OP_SRL:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) >> %u;\n", insn->rd, insn->rt, insn->sa);
            break;
        case MIPS_OP_SRA:
            if (insn->rd != 0) fprintf(f, "    R(%u) = (uint32_t)((int32_t)R(%u) >> %u);\n", insn->rd, insn->rt, insn->sa);
            break;
        case MIPS_OP_SLLV:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) << (R(%u) & 0x1F);\n", insn->rd, insn->rt, insn->rs);
            break;
        case MIPS_OP_SRLV:
            if (insn->rd != 0) fprintf(f, "    R(%u) = R(%u) >> (R(%u) & 0x1F);\n", insn->rd, insn->rt, insn->rs);
            break;
        case MIPS_OP_SRAV:
            if (insn->rd != 0) fprintf(f, "    R(%u) = (uint32_t)((int32_t)R(%u) >> (R(%u) & 0x1F));\n", insn->rd, insn->rt, insn->rs);
            break;

        // Multiply / Divide
        case MIPS_OP_MULT:
            fprintf(f, "    { int64_t res = (int64_t)(int32_t)R(%u) * (int64_t)(int32_t)R(%u); LO = (uint32_t)res; HI = (uint32_t)(res >> 32); }\n", insn->rs, insn->rt);
            break;
        case MIPS_OP_MULTU:
            fprintf(f, "    { uint64_t res = (uint64_t)R(%u) * (uint64_t)R(%u); LO = (uint32_t)res; HI = (uint32_t)(res >> 32); }\n", insn->rs, insn->rt);
            break;
        case MIPS_OP_DIV:
            fprintf(f, "    if (R(%u) != 0) { LO = (uint32_t)((int32_t)R(%u) / (int32_t)R(%u)); HI = (uint32_t)((int32_t)R(%u) %% (int32_t)R(%u)); }\n", insn->rt, insn->rs, insn->rt, insn->rs, insn->rt);
            break;
        case MIPS_OP_DIVU:
            fprintf(f, "    if (R(%u) != 0) { LO = R(%u) / R(%u); HI = R(%u) %% R(%u); }\n", insn->rt, insn->rs, insn->rt, insn->rs, insn->rt);
            break;
        case MIPS_OP_MFHI:
            if (insn->rd != 0) fprintf(f, "    R(%u) = HI;\n", insn->rd);
            break;
        case MIPS_OP_MFLO:
            if (insn->rd != 0) fprintf(f, "    R(%u) = LO;\n", insn->rd);
            break;
        case MIPS_OP_MTHI:
            fprintf(f, "    HI = R(%u);\n", insn->rs);
            break;
        case MIPS_OP_MTLO:
            fprintf(f, "    LO = R(%u);\n", insn->rs);
            break;

        // Memory
        case MIPS_OP_LB:
            if (insn->rt != 0) fprintf(f, "    R(%u) = (uint32_t)(int32_t)(int8_t)MEM_R8(R(%u) + (%d));\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_LBU:
            if (insn->rt != 0) fprintf(f, "    R(%u) = (uint32_t)MEM_R8(R(%u) + (%d));\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_LH:
            if (insn->rt != 0) fprintf(f, "    R(%u) = (uint32_t)(int32_t)(int16_t)MEM_R16(R(%u) + (%d));\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_LHU:
            if (insn->rt != 0) fprintf(f, "    R(%u) = (uint32_t)MEM_R16(R(%u) + (%d));\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_LW:
            if (insn->rt != 0) fprintf(f, "    R(%u) = MEM_R32(R(%u) + (%d));\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_LWL:
            if (insn->rt != 0) fprintf(f, "    R(%u) = MEM_LWL(R(%u) + (%d), R(%u));\n", insn->rt, insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_LWR:
            if (insn->rt != 0) fprintf(f, "    R(%u) = MEM_LWR(R(%u) + (%d), R(%u));\n", insn->rt, insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_SB:
            fprintf(f, "    MEM_W8(R(%u) + (%d), (uint8_t)R(%u));\n", insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_SH:
            fprintf(f, "    MEM_W16(R(%u) + (%d), (uint16_t)R(%u));\n", insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_SW:
            fprintf(f, "    MEM_W32(R(%u) + (%d), R(%u));\n", insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_SWL:
            fprintf(f, "    MEM_SWL(R(%u) + (%d), R(%u));\n", insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_SWR:
            fprintf(f, "    MEM_SWR(R(%u) + (%d), R(%u));\n", insn->rs, insn->simm, insn->rt);
            break;

        // Coprocessor 0
        case MIPS_OP_MFC0:
            if (insn->rt != 0) fprintf(f, "    R(%u) = ctx->cpu.cop0[%u];\n", insn->rt, insn->rd);
            break;
        case MIPS_OP_MTC0:
            fprintf(f, "    ctx->cpu.cop0[%u] = R(%u);\n", insn->rd, insn->rt);
            break;
        case MIPS_OP_RFE:
            fprintf(f, "    ps1_rfe(ctx);\n");
            break;

        // Coprocessor 2 (GTE)
        case MIPS_OP_MFC2:
            if (insn->rt != 0) fprintf(f, "    R(%u) = ps1_gte_get_data(&ctx->gte, %u);\n", insn->rt, insn->rd);
            break;
        case MIPS_OP_MTC2:
            fprintf(f, "    ps1_gte_set_data(&ctx->gte, %u, R(%u));\n", insn->rd, insn->rt);
            break;
        case MIPS_OP_CFC2:
            if (insn->rt != 0) fprintf(f, "    R(%u) = ps1_gte_get_ctrl(&ctx->gte, %u);\n", insn->rt, insn->rd);
            break;
        case MIPS_OP_CTC2:
            fprintf(f, "    ps1_gte_set_ctrl(&ctx->gte, %u, R(%u));\n", insn->rd, insn->rt);
            break;
        case MIPS_OP_LWC2:
            fprintf(f, "    ps1_gte_set_data(&ctx->gte, %u, MEM_R32(R(%u) + (%d)));\n", insn->rt, insn->rs, insn->simm);
            break;
        case MIPS_OP_SWC2:
            fprintf(f, "    MEM_W32(R(%u) + (%d), ps1_gte_get_data(&ctx->gte, %u));\n", insn->rs, insn->simm, insn->rt);
            break;
        case MIPS_OP_COP2_CMD:
            fprintf(f, "    ps1_gte_execute_command(&ctx->gte, 0x%08X);\n", insn->raw);
            break;

        // Syscalls
        case MIPS_OP_SYSCALL:
            fprintf(f, "    ps1_trigger_syscall(ctx);\n");
            break;
        case MIPS_OP_BREAK:
            fprintf(f, "    ps1_trigger_break(ctx);\n");
            break;

        default:
            break;
    }
}

bool mips_emit_c_code(const MipsProgram* prog, const EmitterOptions* opts) {
    FILE* fc = fopen(opts->output_c_path, "w");
    if (!fc) return false;

    FILE* fh = fopen(opts->output_h_path, "w");
    if (!fh) { fclose(fc); return false; }

    // Header File
    fprintf(fh, "// Auto-generated by MimiPS1 Recompiler\n");
    fprintf(fh, "#ifndef RECOMPILED_GAME_H\n");
    fprintf(fh, "#define RECOMPILED_GAME_H\n\n");
    fprintf(fh, "#include \"mimips1_runtime.h\"\n\n");
    fprintf(fh, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");

    // Forward declarations of all recompiled functions
    MipsFunction* fn = prog->first_func;
    while (fn) {
        fprintf(fh, "void %s(PS1Context* ctx);\n", fn->name);
        fn = fn->next;
    }

    fprintf(fh, "\nvoid ps1_init_recompiled_game(PS1Context* ctx);\n");
    fprintf(fh, "void ps1_run_recompiled_game(PS1Context* ctx);\n\n");
    fprintf(fh, "#ifdef __cplusplus\n}\n#endif\n\n");
    fprintf(fh, "#endif // RECOMPILED_GAME_H\n");
    fclose(fh);

    // C Source File
    const char* h_base = strrchr(opts->output_h_path, '/');
    h_base = h_base ? (h_base + 1) : opts->output_h_path;

    fprintf(fc, "// Auto-generated by MimiPS1 Ahead-of-Time Static Recompiler\n");
    fprintf(fc, "#pragma clang diagnostic ignored \"-Wunused-label\"\n");
    fprintf(fc, "#pragma clang diagnostic ignored \"-Wparentheses\"\n");
    fprintf(fc, "#include \"%s\"\n", h_base);
    fprintf(fc, "#include \"mimips1_runtime.h\"\n\n");

    // Emit functions
    fn = prog->first_func;
    while (fn) {
        // Body takes a flag: when entered via the function table at an address other
        // than the entry point (function pointer into code the CFG merged), jump straight
        // into the dispatch switch. ps1_call_indirect sets cpu.pc right before the call.
        fprintf(fc, "static void %s_impl(PS1Context* ctx, int _mid_entry) {\n", fn->name);
        fprintf(fc, "    bool _take_branch = false;\n");
        fprintf(fc, "    uint32_t _call_target = 0;\n");
        fprintf(fc, "    uint32_t _jr_target = 0;\n");
        fprintf(fc, "    if (_mid_entry) goto dispatch_entry;\n");

        MipsBlock* blk = fn->first_block;
        while (blk) {
            int n = blk->num_insns;
            bool ends_with_delay = (n >= 2 && blk->insns[n - 2].has_delay_slot);
            int normal_insns = ends_with_delay ? (n - 2) : n;

            // Emit normal instructions in basic block
            for (int i = 0; i < normal_insns; i++) {
                fprintf(fc, "loc_%08x:\n", blk->insns[i].pc);
                emit_instruction_c(fc, &blk->insns[i]);
            }

            if (ends_with_delay) {
                const MipsInsn* br = &blk->insns[n - 2];
                const MipsInsn* ds = &blk->insns[n - 1];

                fprintf(fc, "loc_%08x:\n", br->pc);

                // Conditional branches
                if (br->is_branch) {
                    const char* cond_expr = "true";
                    char buf[128];
                    switch (br->id) {
                        case MIPS_OP_BEQ:
                            snprintf(buf, sizeof(buf), "(R(%u) == R(%u))", br->rs, br->rt);
                            cond_expr = buf;
                            break;
                        case MIPS_OP_BNE:
                            snprintf(buf, sizeof(buf), "(R(%u) != R(%u))", br->rs, br->rt);
                            cond_expr = buf;
                            break;
                        case MIPS_OP_BLEZ:
                            snprintf(buf, sizeof(buf), "((int32_t)R(%u) <= 0)", br->rs);
                            cond_expr = buf;
                            break;
                        case MIPS_OP_BGTZ:
                            snprintf(buf, sizeof(buf), "((int32_t)R(%u) > 0)", br->rs);
                            cond_expr = buf;
                            break;
                        case MIPS_OP_BLTZ:
                        case MIPS_OP_BLTZAL:
                            snprintf(buf, sizeof(buf), "((int32_t)R(%u) < 0)", br->rs);
                            cond_expr = buf;
                            break;
                        case MIPS_OP_BGEZ:
                        case MIPS_OP_BGEZAL:
                            snprintf(buf, sizeof(buf), "((int32_t)R(%u) >= 0)", br->rs);
                            cond_expr = buf;
                            break;
                        default: break;
                    }

                    if (br->is_call) {
                        fprintf(fc, "    R(31) = 0x%08X;\n", br->pc + 8);
                    }

                    fprintf(fc, "    _take_branch = %s;\n", cond_expr);
                    fprintf(fc, "loc_%08x:\n", ds->pc);
                    emit_instruction_c(fc, ds); // Execute delay slot
                    if (br->target >= fn->entry_pc && br->target < fn->end_pc) {
                        if (br->target <= br->pc) {
                            fprintf(fc, "    if (_take_branch) { ps1_cpu_tick(ctx, 32); goto loc_%08x; }\n", br->target);
                        } else {
                            fprintf(fc, "    if (_take_branch) goto loc_%08x;\n", br->target);
                        }
                    } else {
                        if (br->is_call) {
                            fprintf(fc, "    if (_take_branch) { CALL_INDIRECT(0x%08X); }\n", br->target);
                        } else {
                            fprintf(fc, "    if (_take_branch) { CALL_INDIRECT(0x%08X); return; }\n", br->target);
                        }
                    }
                }
                // Function calls (JAL, JALR)
                else if (br->is_call) {
                    uint32_t ret_reg = (br->id == MIPS_OP_JALR) ? (br->rd != 0 ? br->rd : 31) : 31;
                    if (br->id == MIPS_OP_JALR) {
                        fprintf(fc, "    _call_target = R(%u);\n", br->rs);
                    }
                    fprintf(fc, "    R(%u) = 0x%08X;\n", ret_reg, br->pc + 8);
                    fprintf(fc, "loc_%08x:\n", ds->pc);
                    emit_instruction_c(fc, ds); // Execute delay slot
                    if (br->id == MIPS_OP_JAL) {
                        if (is_func_in_program(prog, br->target)) {
                            fprintf(fc, "    ps1_func_%08x(ctx);\n", br->target);
                        } else {
                            fprintf(fc, "    CALL_INDIRECT(0x%08X);\n", br->target);
                        }
                    } else {
                        fprintf(fc, "    CALL_INDIRECT(_call_target);\n");
                    }
                }
                // Return (JR $ra)
                else if (br->is_return) {
                    fprintf(fc, "loc_%08x:\n", ds->pc);
                    emit_instruction_c(fc, ds); // Execute delay slot
                    fprintf(fc, "    return;\n");
                }
                // Unconditional Jump (J)
                else if (br->id == MIPS_OP_J) {
                    fprintf(fc, "loc_%08x:\n", ds->pc);
                    emit_instruction_c(fc, ds); // Execute delay slot
                    if (br->target >= fn->entry_pc && br->target < fn->end_pc) {
                        if (br->target <= br->pc) {
                            fprintf(fc, "    ps1_cpu_tick(ctx, 32);\n");
                        }
                        fprintf(fc, "    goto loc_%08x;\n", br->target);
                    } else {
                        fprintf(fc, "    CALL_INDIRECT(0x%08X);\n    return;\n", br->target);
                    }
                }
                // Indirect Jump (JR $reg, e.g. switch jump tables)
                else if (br->id == MIPS_OP_JR) {
                    fprintf(fc, "    _jr_target = R(%u);\n", br->rs);
                    fprintf(fc, "loc_%08x:\n", ds->pc);
                    emit_instruction_c(fc, ds);
                    // Jump-table loops have no other tick point: advance time here too
                    fprintf(fc, "    ctx->cpu.pc = _jr_target;\n");
                    fprintf(fc, "    ps1_cpu_tick(ctx, 32);\n");
                    fprintf(fc, "    goto dispatch_entry;\n");
                }
            }

            blk = blk->next;
        }

        // If function does not end with an unconditional jump or return, fall through sequentially
        if (fn->next) {
            MipsBlock* lb = fn->last_block;
            bool ends_with_jump = false;
            if (lb && lb->num_insns >= 2 && lb->insns[lb->num_insns - 2].has_delay_slot) {
                const MipsInsn* last_branch = &lb->insns[lb->num_insns - 2];
                if (last_branch->is_return || last_branch->id == MIPS_OP_J || last_branch->id == MIPS_OP_JR) {
                    ends_with_jump = true;
                }
            }
            if (!ends_with_jump) {
                fprintf(fc, "    %s(ctx);\n    return;\n", fn->next->name);
            } else {
                fprintf(fc, "    return;\n");
            }
        } else {
            fprintf(fc, "    return;\n");
        }
        fprintf(fc, "dispatch_entry:\n");
        fprintf(fc, "    switch (ctx->cpu.pc) {\n");
        MipsBlock* b = fn->first_block;
        while (b) {
            for (int k = 0; k < b->num_insns; k++) {
                fprintf(fc, "        case 0x%08X: goto loc_%08x;\n", b->insns[k].pc, b->insns[k].pc);
            }
            b = b->next;
        }
        fprintf(fc, "        default: CALL_INDIRECT(ctx->cpu.pc); return;\n");
        fprintf(fc, "    }\n");
        fprintf(fc, "}\n");
        fprintf(fc, "void %s(PS1Context* ctx) { %s_impl(ctx, 0); }\n", fn->name, fn->name);
        fprintf(fc, "static void %s_mid(PS1Context* ctx) { %s_impl(ctx, 1); }\n\n", fn->name, fn->name);
        fn = fn->next;
    }

    // Master initialization function
    fprintf(fc, "void ps1_init_recompiled_game(PS1Context* ctx) {\n");
    fn = prog->first_func;
    while (fn) {
        fprintf(fc, "    ps1_register_function(ctx, 0x%08X, %s);\n", fn->entry_pc, fn->name);
        fn = fn->next;
    }
    // Mid-function trampolines: only fill slots not claimed by a real entry point.
    // Registered per basic block so every covered address has a case in the dispatch switch.
    fn = prog->first_func;
    while (fn) {
        for (MipsBlock* mb = fn->first_block; mb; mb = mb->next) {
            if (mb->num_insns > 0) {
                fprintf(fc, "    ps1_register_function_range(ctx, 0x%08X, 0x%08X, %s_mid);\n",
                        mb->insns[0].pc, mb->insns[mb->num_insns - 1].pc, fn->name);
            }
        }
        fn = fn->next;
    }
    fprintf(fc, "    if (ctx->cpu.pc == 0) {\n");
    fprintf(fc, "        ctx->cpu.pc = 0x%08X;\n", prog->entry_pc);
    fprintf(fc, "        ctx->cpu.r[28] = 0x80018000;\n");
    fprintf(fc, "        ctx->cpu.r[29] = 0x801FFFF0;\n");
    fprintf(fc, "        ctx->cpu.r[30] = 0x801FFFF0;\n");
    fprintf(fc, "    }\n");
    fprintf(fc, "}\n\n");

    fprintf(fc, "void ps1_run_recompiled_game(PS1Context* ctx) {\n");
    fprintf(fc, "    ps1_init_recompiled_game(ctx);\n");
    fprintf(fc, "    ps1_func_%08x(ctx);\n", prog->entry_pc);
    fprintf(fc, "}\n");

    fclose(fc);
    return true;
}
