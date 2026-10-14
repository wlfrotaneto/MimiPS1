#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include "mimips1_gte.h"

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
        g_tests_failed++; \
        return; \
    } \
} while(0)

// Case 1: Identity rotation + translation: SX = OFX/65536 + x*H/z
static void test_identity_and_translation(void) {
    printf("Running test_identity_and_translation...\n");
    PS1GTE gte;
    ps1_gte_init(&gte);

    // Identity rotation matrix (scaled by 4096 = 0x1000)
    gte.rot_matrix[0][0] = 0x1000; gte.rot_matrix[0][1] = 0; gte.rot_matrix[0][2] = 0;
    gte.rot_matrix[1][0] = 0; gte.rot_matrix[1][1] = 0x1000; gte.rot_matrix[1][2] = 0;
    gte.rot_matrix[2][0] = 0; gte.rot_matrix[2][1] = 0; gte.rot_matrix[2][2] = 0x1000;

    // Translation vector TR = (0, 0, 1000)
    gte.trans_vector[0] = 0;
    gte.trans_vector[1] = 0;
    gte.trans_vector[2] = 1000;

    // Screen offset and FOV
    gte.offset_x = 160 << 16;
    gte.offset_y = 120 << 16;
    gte.h = 240;

    // Vertex 0: (100, 50, 0) -> in camera space: (100, 50, 1000)
    gte.v[0][0] = 100;
    gte.v[0][1] = 50;
    gte.v[0][2] = 0;

    // Execute RTPS with sf = 1 (bit 19 set)
    uint32_t opcode = 0x01 | (1 << 19);
    ps1_gte_rtps(&gte, opcode);

    // Expected:
    // z = 1000
    // sx = 160 + (100 * 240) / 1000 = 160 + 24 = 184
    // sy = 120 + (50 * 240) / 1000 = 120 + 12 = 132
    int16_t sx = gte.sxy[2][0];
    int16_t sy = gte.sxy[2][1];
    printf("  Result: SX = %d (expected ~184), SY = %d (expected ~132), SZ3 = %u\n", sx, sy, gte.sz[3]);

    TEST_ASSERT(abs(sx - 184) <= 1, "SX should match perspective projection ~184");
    TEST_ASSERT(abs(sy - 132) <= 1, "SY should match perspective projection ~132");
    TEST_ASSERT(gte.sz[3] == 1000, "SZ3 should equal 1000");
    TEST_ASSERT((gte.flag & (1u << 31)) == 0, "No error flags should be set");

    g_tests_passed++;
    printf("  [PASS] test_identity_and_translation\n");
}

// Case 2: Saturation of SX/SY in [-0x400, 0x3FF]
static void test_sx_sy_saturation(void) {
    printf("Running test_sx_sy_saturation...\n");
    PS1GTE gte;
    ps1_gte_init(&gte);

    gte.rot_matrix[0][0] = 0x1000; gte.rot_matrix[1][1] = 0x1000; gte.rot_matrix[2][2] = 0x1000;
    gte.trans_vector[2] = 200; // z = 200
    gte.h = 240;
    gte.offset_x = 0;
    gte.offset_y = 0;

    // Huge positive coordinate -> should saturate at 0x3FF and set flag 14
    gte.v[0][0] = 20000;
    gte.v[0][1] = 20000;
    gte.v[0][2] = 0;
    ps1_gte_rtps(&gte, 0x01 | (1 << 19));

    TEST_ASSERT(gte.sxy[2][0] == 0x3FF, "SX should saturate at +0x3FF");
    TEST_ASSERT(gte.sxy[2][1] == 0x3FF, "SY should saturate at +0x3FF");
    TEST_ASSERT((gte.flag & (1u << 14)) != 0, "Flag bit 14 (SX saturation) must be set");
    TEST_ASSERT((gte.flag & (1u << 13)) != 0, "Flag bit 13 (SY saturation) must be set");
    TEST_ASSERT((gte.flag & (1u << 31)) != 0, "Master error flag bit 31 must be set");

    // Huge negative coordinate -> should saturate at -0x400
    gte.v[0][0] = -20000;
    gte.v[0][1] = -20000;
    gte.v[0][2] = 0;
    ps1_gte_rtps(&gte, 0x01 | (1 << 19));

    TEST_ASSERT(gte.sxy[2][0] == -0x400, "SX should saturate at -0x400");
    TEST_ASSERT(gte.sxy[2][1] == -0x400, "SY should saturate at -0x400");

    g_tests_passed++;
    printf("  [PASS] test_sx_sy_saturation\n");
}

// Case 3: FIFO order after RTPS and RTPT
static void test_fifo_ordering(void) {
    printf("Running test_fifo_ordering...\n");
    PS1GTE gte;
    ps1_gte_init(&gte);

    gte.rot_matrix[0][0] = 0x1000; gte.rot_matrix[1][1] = 0x1000; gte.rot_matrix[2][2] = 0x1000;
    gte.h = 240;
    gte.offset_x = 0;
    gte.offset_y = 0;

    // Set initial values in SZ and SXY
    gte.sz[0] = 111; gte.sz[1] = 222; gte.sz[2] = 333; gte.sz[3] = 444;
    gte.sxy[0][0] = 10; gte.sxy[1][0] = 20; gte.sxy[2][0] = 30;

    // RTPS with V0 depth 500
    gte.trans_vector[2] = 500;
    gte.v[0][0] = 40; gte.v[0][1] = 0; gte.v[0][2] = 0;
    ps1_gte_rtps(&gte, 0x01 | (1 << 19));

    // After 1 RTPS:
    // SZ FIFO moved down: sz[0]=222, sz[1]=333, sz[2]=444, sz[3]=500
    TEST_ASSERT(gte.sz[0] == 222, "SZ0 should have shifted");
    TEST_ASSERT(gte.sz[1] == 333, "SZ1 should have shifted");
    TEST_ASSERT(gte.sz[2] == 444, "SZ2 should have shifted");
    TEST_ASSERT(gte.sz[3] == 500, "SZ3 should be 500");
    TEST_ASSERT(gte.sxy[0][0] == 20, "SXY0 should have shifted");
    TEST_ASSERT(gte.sxy[1][0] == 30, "SXY1 should have shifted");

    // RTPT with 3 vertices: depths 600, 700, 800
    gte.v[0][0] = 10; gte.v[0][1] = 0; gte.v[0][2] = 100; // z = 600
    gte.v[1][0] = 20; gte.v[1][1] = 0; gte.v[1][2] = 200; // z = 700
    gte.v[2][0] = 30; gte.v[2][1] = 0; gte.v[2][2] = 300; // z = 800
    ps1_gte_rtpt(&gte, 0x30 | (1 << 19));

    // In RTPT:
    // V0 pushed first, then V1, then V2:
    // SZ: sz[0] = previous sz[3] (=500), sz[1] = 600 (V0), sz[2] = 700 (V1), sz[3] = 800 (V2)!
    TEST_ASSERT(gte.sz[0] == 500, "SZ0 should retain old depth before RTPT (500)");
    TEST_ASSERT(gte.sz[1] == 600, "SZ1 should contain V0 depth (600)");
    TEST_ASSERT(gte.sz[2] == 700, "SZ2 should contain V1 depth (700)");
    TEST_ASSERT(gte.sz[3] == 800, "SZ3 should contain V2 depth (800)");

    // SXY: sxy[0]=V0, sxy[1]=V1, sxy[2]=V2
    int16_t sx0 = gte.sxy[0][0];
    int16_t sx1 = gte.sxy[1][0];
    int16_t sx2 = gte.sxy[2][0];
    printf("  RTPT Screen X coordinates: v0=%d, v1=%d, v2=%d\n", sx0, sx1, sx2);
    TEST_ASSERT(sx0 < sx1 && sx1 < sx2, "SXY FIFO must preserve V0 -> V1 -> V2 order");

    g_tests_passed++;
    printf("  [PASS] test_fifo_ordering\n");
}

// Case 4: NCLIP sign for clockwise vs counter-clockwise triangle
static void test_nclip_sign(void) {
    printf("Running test_nclip_sign...\n");
    PS1GTE gte;
    ps1_gte_init(&gte);

    // Clockwise triangle:
    // (0, 0), (10, 0), (0, 10)
    gte.sxy[0][0] = 0;  gte.sxy[0][1] = 0;
    gte.sxy[1][0] = 10; gte.sxy[1][1] = 0;
    gte.sxy[2][0] = 0;  gte.sxy[2][1] = 10;
    ps1_gte_nclip(&gte);
    int32_t cw_result = gte.mac[0];
    printf("  Clockwise NCLIP MAC0 = %d\n", cw_result);
    TEST_ASSERT(cw_result > 0, "Clockwise triangle must yield positive NCLIP MAC0");

    // Counter-clockwise triangle:
    // (0, 0), (0, 10), (10, 0)
    gte.sxy[0][0] = 0;  gte.sxy[0][1] = 0;
    gte.sxy[1][0] = 0;  gte.sxy[1][1] = 10;
    gte.sxy[2][0] = 10; gte.sxy[2][1] = 0;
    ps1_gte_nclip(&gte);
    int32_t ccw_result = gte.mac[0];
    printf("  Counter-clockwise NCLIP MAC0 = %d\n", ccw_result);
    TEST_ASSERT(ccw_result < 0, "Counter-clockwise triangle must yield negative NCLIP MAC0");

    g_tests_passed++;
    printf("  [PASS] test_nclip_sign\n");
}

// Case 5: AVSZ3 / AVSZ4
static void test_avsz3_avsz4(void) {
    printf("Running test_avsz3_avsz4...\n");
    PS1GTE gte;
    ps1_gte_init(&gte);

    // Scaling factors (signed 16-bit)
    // ZSF3 = 4096 / 3 = 1365 (0x0555)
    gte.zsf3 = 1365;
    // ZSF4 = 4096 / 4 = 1024 (0x0400)
    gte.zsf4 = 1024;

    gte.sz[0] = 100;
    gte.sz[1] = 200;
    gte.sz[2] = 300;
    gte.sz[3] = 400;

    // AVSZ3: uses sz[1] + sz[2] + sz[3] = 200 + 300 + 400 = 900
    // OTZ = (900 * 1365) >> 12 = 1228500 >> 12 = 299 (~300)
    ps1_gte_avsz3(&gte);
    printf("  AVSZ3 OTZ = %u (expected ~300)\n", gte.otz);
    TEST_ASSERT(abs((int)gte.otz - 300) <= 2, "AVSZ3 average should be ~300");

    // AVSZ4: uses sz[0] + sz[1] + sz[2] + sz[3] = 100 + 200 + 300 + 400 = 1000
    // OTZ = (1000 * 1024) >> 12 = 1024000 >> 12 = 250
    ps1_gte_avsz4(&gte);
    printf("  AVSZ4 OTZ = %u (expected 250)\n", gte.otz);
    TEST_ASSERT(gte.otz == 250, "AVSZ4 average should be exactly 250");

    g_tests_passed++;
    printf("  [PASS] test_avsz3_avsz4\n");
}

// Case 6: Division overflow check (H >= 2 * SZ3 -> flag 17)
static void test_division_overflow(void) {
    printf("Running test_division_overflow...\n");
    PS1GTE gte;
    ps1_gte_init(&gte);

    gte.rot_matrix[0][0] = 0x1000; gte.rot_matrix[1][1] = 0x1000; gte.rot_matrix[2][2] = 0x1000;
    gte.h = 400; // H = 400
    gte.trans_vector[2] = 150; // z = 150 -> 2 * SZ3 = 300 <= H (400) -> OVERFLOW!
    gte.v[0][0] = 0; gte.v[0][1] = 0; gte.v[0][2] = 0;

    ps1_gte_rtps(&gte, 0x01 | (1 << 19));

    TEST_ASSERT((gte.flag & (1u << 17)) != 0, "Flag bit 17 (divide overflow) must be set when H >= 2*SZ3");
    TEST_ASSERT((gte.flag & (1u << 31)) != 0, "Master error flag bit 31 must be set");

    g_tests_passed++;
    printf("  [PASS] test_division_overflow\n");
}

int main(void) {
    printf("=== MimiPS1 GTE Hardware Accuracy Test Suite ===\n");
    test_identity_and_translation();
    test_sx_sy_saturation();
    test_fifo_ordering();
    test_nclip_sign();
    test_avsz3_avsz4();
    test_division_overflow();

    printf("\nTotal: %d passed, %d failed\n", g_tests_passed, g_tests_failed);
    return g_tests_failed == 0 ? 0 : 1;
}
