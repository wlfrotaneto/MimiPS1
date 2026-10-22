cmake_minimum_required(VERSION 3.20)
project(MimiPS1 LANGUAGES C OBJC)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64")

# Compiler optimization flags for Apple Silicon (ARM64)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Wall -Wextra -Wno-unused-parameter")
set(CMAKE_OBJC_FLAGS "${CMAKE_OBJC_FLAGS} -O3 -Wall -Wextra -Wno-unused-parameter -fobjc-arc")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/recompiler/include
)

# 1. Recompiler CLI executable
add_executable(mimips1-recomp
    recompiler/src/main.c
    recompiler/src/ps1_exe.c
    recompiler/src/mips_disasm.c
    recompiler/src/mips_cfg.c
    recompiler/src/c_emitter.c
)

# 2. Native PS1 Runtime Library
add_library(mimips1-runtime STATIC
    runtime/src/mimips1_memory.c
    runtime/src/mimips1_cpu.c
    runtime/src/mimips1_interp.c
    runtime/src/mimips1_mdec.c
    runtime/src/mimips1_gte.c
    runtime/src/mimips1_gpu.c
    runtime/src/mimips1_spu.c
    runtime/src/mimips1_dma.c
    runtime/src/mimips1_timers.c
    runtime/src/mimips1_cdrom.c
    runtime/src/mimips1_bios.c
    runtime/src/mimips1_input.c
    runtime/src/mimips1_card.c
    runtime/src/mimips1_runtime.c
    recompiler/src/mips_disasm.c
    runtime/src/platform/macos_metal.m
    runtime/src/platform/macos_audio.m
    runtime/src/platform/macos_main.m
)

target_link_libraries(mimips1-runtime PUBLIC
    "-framework Cocoa"
    "-framework Metal"
    "-framework MetalKit"
    "-framework AudioToolbox"
    "-framework GameController"
    "-framework CoreHaptics"
)

# 3. Native App Executable
add_executable(MimiPS1
    runtime/src/recompiled_main.m
)
target_link_libraries(MimiPS1 PRIVATE mimips1-runtime)

# 4. Custom target for .app bundle
add_custom_target(app_bundle
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_app_bundle.sh $<TARGET_FILE:MimiPS1> MimiPS1 ${CMAKE_BINARY_DIR}
    DEPENDS MimiPS1
    COMMENT "Packaging MimiPS1.app bundle..."
)

# 5. Custom target for .dmg packaging
add_custom_target(dmg
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/tools/package_dmg.sh ${CMAKE_BINARY_DIR}/MimiPS1.app MimiPS1-macOS-AppleSilicon ${CMAKE_CURRENT_SOURCE_DIR}/dist
    DEPENDS app_bundle
    COMMENT "Packaging MimiPS1 Apple Silicon DMG installer..."
)

# 6. GTE Hardware Accuracy Test Suite
add_executable(gte_test
    tests/gte_test.c
)
target_link_libraries(gte_test PRIVATE mimips1-runtime)
