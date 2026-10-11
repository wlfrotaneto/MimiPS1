#ifndef RECOMP_C_EMITTER_H
#define RECOMP_C_EMITTER_H

#include "mips_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* output_c_path;
    const char* output_h_path;
    bool        enable_opt;
    bool        emit_comments;
} EmitterOptions;

bool mips_emit_c_code(const MipsProgram* prog, const EmitterOptions* opts);

#ifdef __cplusplus
}
#endif

#endif // RECOMP_C_EMITTER_H
