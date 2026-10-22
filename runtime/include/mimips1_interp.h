#ifndef MIMIPS1_INTERP_H
#define MIMIPS1_INTERP_H

#include "mimips1_types.h"

#ifdef __cplusplus
extern "C" {
#endif

// Master loop: executes guest code from ctx->cpu.pc until the emulator stops.
void ps1_interp_run(PS1Context* ctx);

// Called from recompiled code to run guest code that was never recompiled. Returns
// when the callee returns to the caller with the stack pointer restored.
void ps1_interp_call(PS1Context* ctx, uint32_t target);

// With the real BIOS in control, native code that must take an interrupt or issue a
// syscall hands the exception to the kernel running in the interpreter.
void ps1_interp_interrupt_from_native(PS1Context* ctx);
void ps1_interp_syscall_from_native(PS1Context* ctx);

#ifdef __cplusplus
}
#endif

#endif // MIMIPS1_INTERP_H
