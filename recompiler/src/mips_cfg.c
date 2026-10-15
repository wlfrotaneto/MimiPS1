#include "mips_cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DISCOVERED_FUNCS 65536

static bool is_addr_in_text(const PS1Executable* exe, uint32_t addr) {
    return (addr >= exe->text_dest_addr && addr < (exe->text_dest_addr + exe->text_size));
}

static void add_discovered_entry(uint32_t* entries, int* count, uint32_t addr) {
    for (int j = 0; j < *count; j++) {
        if (entries[j] == addr) return;
    }
    if (*count < MAX_DISCOVERED_FUNCS) {
        entries[(*count)++] = addr;
    }
}

MipsProgram* mips_analyze_program(const PS1Executable* exe) {
    MipsProgram* prog = (MipsProgram*)calloc(1, sizeof(MipsProgram));
    if (!prog) return NULL;

    prog->entry_pc  = exe->init_pc;
    prog->base_addr = exe->text_dest_addr;
    prog->code_size = exe->text_size;

    uint32_t* discovered_entries = (uint32_t*)malloc(MAX_DISCOVERED_FUNCS * sizeof(uint32_t));
    int entry_count = 0;

    // Add entry PC
    if (is_addr_in_text(exe, exe->init_pc)) {
        add_discovered_entry(discovered_entries, &entry_count, exe->init_pc);
    }

    uint32_t num_insns = exe->text_size / 4;
    const uint32_t* words = (const uint32_t*)exe->code_buffer;

    // Precompute forward branch reach: any instruction spanned by an intra-function
    // forward branch cannot be an independent function entry point.
    uint32_t max_forward_target = 0;
    bool* is_spanned = (bool*)calloc(num_insns, sizeof(bool));
    for (uint32_t i = 0; i < num_insns; i++) {
        uint32_t cur_pc = exe->text_dest_addr + (i * 4);
        uint32_t raw = words[i];
        MipsInsn insn;
        mips_decode_instruction(raw, cur_pc, &insn);
        if (insn.is_branch && !insn.is_call && insn.target > cur_pc && insn.target < (cur_pc + 0x0400)) {
            if (insn.target > max_forward_target) {
                max_forward_target = insn.target;
            }
        }
        if (max_forward_target > cur_pc) {
            is_spanned[i] = true;
        }
    }

    // First pass: Discover all JAL call targets and standard function prologues
    for (uint32_t i = 0; i < num_insns; i++) {
        uint32_t cur_pc = exe->text_dest_addr + (i * 4);
        uint32_t raw = words[i];
        MipsInsn insn;
        mips_decode_instruction(raw, cur_pc, &insn);

        if (insn.is_call && insn.id == MIPS_OP_JAL) {
            if (is_addr_in_text(exe, insn.target)) {
                add_discovered_entry(discovered_entries, &entry_count, insn.target);
            }
        }
        // After a function return (JR $ra + delay slot),
        // the first non-NOP instruction begins a new function entry point if not spanned by a branch.
        if (i >= 2 && words[i] != 0 && !is_spanned[i]) {
            MipsInsn prev_term;
            mips_decode_instruction(words[i - 2], cur_pc - 8, &prev_term);
            if (prev_term.is_return) {
                add_discovered_entry(discovered_entries, &entry_count, cur_pc);
            } else if (i >= 3 && words[i - 1] == 0) {
                // If preceded by NOP padding after a return
                mips_decode_instruction(words[i - 3], cur_pc - 12, &prev_term);
                if (prev_term.is_return) {
                    add_discovered_entry(discovered_entries, &entry_count, cur_pc);
                }
            }
        }

        // Standard MIPS stack frame prologue: addiu $sp, $sp, -imm
        if (insn.id == MIPS_OP_ADDIU && insn.rs == 29 && insn.rt == 29 && insn.simm < 0 && !is_spanned[i]) {
            bool preceded_by_terminator = (i == 0);
            if (i >= 2) {
                MipsInsn prev_branch;
                mips_decode_instruction(words[i - 2], cur_pc - 8, &prev_branch);
                if (prev_branch.is_return || prev_branch.id == MIPS_OP_J) {
                    preceded_by_terminator = true;
                }
            }
            if (preceded_by_terminator) {
                add_discovered_entry(discovered_entries, &entry_count, cur_pc);
            }
        }
    }
    free(is_spanned);

    // Sort function entry points
    for (int i = 0; i < entry_count - 1; i++) {
        for (int j = i + 1; j < entry_count; j++) {
            if (discovered_entries[i] > discovered_entries[j]) {
                uint32_t tmp = discovered_entries[i];
                discovered_entries[i] = discovered_entries[j];
                discovered_entries[j] = tmp;
            }
        }
    }

    printf("[CFG Analysis] Discovered %d function entry points\n", entry_count);

    // Second pass: Build functions and blocks
    MipsFunction* last_func = NULL;

    for (int i = 0; i < entry_count; i++) {
        uint32_t start_pc = discovered_entries[i];
        uint32_t next_func_pc = (i + 1 < entry_count) ? discovered_entries[i + 1] : (exe->text_dest_addr + exe->text_size);
        // Safety cap: never allow a single function to exceed 32768 instructions (128KB)
        if (next_func_pc > (start_pc + 32768 * 4)) {
            next_func_pc = start_pc + 32768 * 4;
        }

        MipsFunction* fn = (MipsFunction*)calloc(1, sizeof(MipsFunction));
        fn->entry_pc = start_pc;
        snprintf(fn->name, sizeof(fn->name), "ps1_func_%08x", start_pc);

        uint32_t cur_pc = start_pc;
        MipsBlock* last_block = NULL;

        while (cur_pc < next_func_pc && is_addr_in_text(exe, cur_pc)) {
            uint32_t block_start = cur_pc;
            uint32_t block_insn_count = 0;
            MipsInsn block_insns[1024];
            bool block_ends = false;

            while (cur_pc < next_func_pc && !block_ends && block_insn_count < 1000) {
                uint32_t idx = (cur_pc - exe->text_dest_addr) / 4;
                MipsInsn insn;
                mips_decode_instruction(words[idx], cur_pc, &insn);
                block_insns[block_insn_count++] = insn;

                if (insn.has_delay_slot) {
                    // Include delay slot instruction in this block
                    cur_pc += 4;
                    if (cur_pc < (exe->text_dest_addr + exe->text_size)) {
                        uint32_t ds_idx = (cur_pc - exe->text_dest_addr) / 4;
                        MipsInsn ds_insn;
                        mips_decode_instruction(words[ds_idx], cur_pc, &ds_insn);
                        block_insns[block_insn_count++] = ds_insn;
                    }
                    block_ends = true;
                }
                cur_pc += 4;
            }

            MipsBlock* block = (MipsBlock*)calloc(1, sizeof(MipsBlock));
            block->start_pc = block_start;
            block->end_pc = cur_pc;
            block->num_insns = block_insn_count;
            block->insns = (MipsInsn*)malloc(block_insn_count * sizeof(MipsInsn));
            memcpy(block->insns, block_insns, block_insn_count * sizeof(MipsInsn));

            if (block_insn_count >= 2 && block->insns[block_insn_count - 2].has_delay_slot) {
                MipsInsn* branch_insn = &block->insns[block_insn_count - 2];
                block->has_branch = branch_insn->is_branch;
                block->has_call = branch_insn->is_call;
                block->branch_target = branch_insn->target;
                block->fallthrough_target = cur_pc;
            }

            if (!fn->first_block) fn->first_block = block;
            if (last_block) last_block->next = block;
            last_block = block;
            fn->last_block = block;
            fn->block_count++;
        }

        fn->end_pc = cur_pc;

        if (!prog->first_func) prog->first_func = fn;
        if (last_func) last_func->next = fn;
        last_func = fn;
        prog->func_count++;
    }

    free(discovered_entries);
    return prog;
}

void mips_free_program(MipsProgram* prog) {
    if (!prog) return;
    MipsFunction* fn = prog->first_func;
    while (fn) {
        MipsBlock* blk = fn->first_block;
        while (blk) {
            if (blk->insns) free(blk->insns);
            MipsBlock* next_blk = blk->next;
            free(blk);
            blk = next_blk;
        }
        MipsFunction* next_fn = fn->next;
        free(fn);
        fn = next_fn;
    }
    free(prog);
}
