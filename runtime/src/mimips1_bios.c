#include "mimips1_bios.h"
#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps1_bios_init(PS1Context* ctx) {
    memset(&ctx->bios, 0, sizeof(PS1BIOS));
    ctx->bios.hle_enabled = true;
    ctx->bios.heap_base = 0x80100000;
    ctx->bios.heap_size = 0x80000; // 512KB default heap
    ctx->bios.heap_cur = ctx->bios.heap_base;
}

void ps1_bios_reset(PS1Context* ctx) {
    ps1_bios_init(ctx);
}

// Convert MIPS virtual address to host RAM pointer if possible
static void* get_host_ptr(PS1Context* ctx, uint32_t addr) {
    uint32_t paddr = addr & 0x1FFFFFFF;
    if (paddr < PS1_RAM_SIZE) {
        return &ctx->memory.ram[paddr];
    }
    return NULL;
}

static inline uint8_t to_bcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

static bool do_cd_search_file(PS1Context* ctx) {
    uint32_t* r = ctx->cpu.r;
    uint32_t fp_addr = r[4];
    uint32_t name_addr = r[5];
    void* fp_host = get_host_ptr(ctx, fp_addr);
    const char* name = (const char*)get_host_ptr(ctx, name_addr);

    if (!fp_host || !name) {
        r[2] = 0;
        return true;
    }

    PS1ISOFile iso_file;
    if (ps1_cdrom_find_file(&ctx->cdrom, name, &iso_file)) {
        uint32_t lba_with_pregap = iso_file.lba + 150;
        uint8_t m = (uint8_t)(lba_with_pregap / (60 * 75));
        uint8_t s = (uint8_t)((lba_with_pregap % (60 * 75)) / 75);
        uint8_t f = (uint8_t)(lba_with_pregap % 75);

        uint8_t* p = (uint8_t*)fp_host;
        p[0] = to_bcd(m);
        p[1] = to_bcd(s);
        p[2] = to_bcd(f);
        p[3] = 0; // track
        *(uint32_t*)&p[4] = iso_file.size;
        strncpy((char*)&p[8], iso_file.name, 15);
        p[23] = '\0';

        r[2] = fp_addr; // Return pointer on success
        printf("[BIOS] CdSearchFile: '%s' -> LBA %u, Size %u bytes\n", name, iso_file.lba, iso_file.size);
    } else {
        r[2] = 0; // Not found
        printf("[BIOS] CdSearchFile: '%s' NOT FOUND\n", name);
    }
    return true;
}

static int bios_file_open(PS1Context* ctx, const char* name, int mode) {
    if (!name) return -1;
    (void)mode;
    PS1ISOFile iso_file;
    if (!ps1_cdrom_find_file(&ctx->cdrom, name, &iso_file)) {
        printf("[BIOS] open('%s') NOT FOUND on disc\n", name);
        return -1;
    }

    for (int i = 0; i < 16; i++) {
        if (!ctx->bios.files[i].is_open) {
            ctx->bios.files[i].is_open = true;
            ctx->bios.files[i].start_lba = iso_file.lba;
            ctx->bios.files[i].file_size = iso_file.size;
            ctx->bios.files[i].cur_offset = 0;
            strncpy(ctx->bios.files[i].name, iso_file.name, sizeof(ctx->bios.files[i].name) - 1);
            printf("[BIOS] open('%s') -> fd=%d (LBA %u, %u bytes)\n", name, i, iso_file.lba, iso_file.size);
            return i;
        }
    }
    return -1;
}

static int bios_file_read(PS1Context* ctx, int fd, void* dst, int length) {
    if (fd < 0 || fd >= 16 || !ctx->bios.files[fd].is_open || !dst || length <= 0) return -1;
    PS1OpenFile* f = &ctx->bios.files[fd];

    uint32_t avail = (f->cur_offset < f->file_size) ? (f->file_size - f->cur_offset) : 0;
    uint32_t to_read = (uint32_t)length < avail ? (uint32_t)length : avail;
    if (to_read == 0) return 0;

    if (!ps1_cdrom_read_bytes(&ctx->cdrom, f->start_lba, f->cur_offset, dst, to_read)) {
        return -1;
    }
    f->cur_offset += to_read;
    return (int)to_read;
}

static int bios_file_seek(PS1Context* ctx, int fd, int offset, int seektype) {
    if (fd < 0 || fd >= 16 || !ctx->bios.files[fd].is_open) return -1;
    PS1OpenFile* f = &ctx->bios.files[fd];
    switch (seektype) {
        case 0: f->cur_offset = offset; break; // SEEK_SET
        case 1: f->cur_offset += offset; break; // SEEK_CUR
        case 2: f->cur_offset = f->file_size + offset; break; // SEEK_END
    }
    if (f->cur_offset > f->file_size) f->cur_offset = f->file_size;
    return (int)f->cur_offset;
}

static int bios_file_close(PS1Context* ctx, int fd) {
    if (fd >= 0 && fd < 16) {
        ctx->bios.files[fd].is_open = false;
    }
    return 0;
}

bool ps1_bios_handle_a0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;

    switch (func) {
        case 0x00: // FileOpen(name, mode)
            {
                const char* name = (const char*)get_host_ptr(ctx, r[4]);
                r[2] = (uint32_t)bios_file_open(ctx, name, r[5]);
            }
            return true;
        case 0x01: // FileSeek(fd, offset, seektype)
            r[2] = (uint32_t)bios_file_seek(ctx, r[4], (int)r[5], (int)r[6]);
            return true;
        case 0x02: // FileRead(fd, dst, len)
            {
                void* dst = get_host_ptr(ctx, r[5]);
                r[2] = (uint32_t)bios_file_read(ctx, r[4], dst, (int)r[6]);
            }
            return true;
        case 0x03: // FileWrite(fd, src, len)
            r[2] = 0;
            return true;
        case 0x04: // FileClose(fd)
            r[2] = (uint32_t)bios_file_close(ctx, r[4]);
            return true;
        case 0x13: // setjmp(env)
            {
                uint32_t* env = (uint32_t*)get_host_ptr(ctx, r[4]);
                if (env) {
                    for (int i = 0; i < 16; i++) env[i] = r[16 + i];
                }
                r[2] = 0;
            }
            return true;
        case 0x14: // longjmp(env, val)
            {
                uint32_t* env = (uint32_t*)get_host_ptr(ctx, r[4]);
                if (env) {
                    for (int i = 0; i < 16; i++) r[16 + i] = env[i];
                }
                r[2] = r[5] ? r[5] : 1;
            }
            return true;
        case 0x17: // strcmp(s1, s2)
            {
                const char* s1 = (const char*)get_host_ptr(ctx, r[4]);
                const char* s2 = (const char*)get_host_ptr(ctx, r[5]);
                r[2] = (s1 && s2) ? (uint32_t)strcmp(s1, s2) : 0;
            }
            return true;
        case 0x18: // strncmp(s1, s2, n)
            {
                const char* s1 = (const char*)get_host_ptr(ctx, r[4]);
                const char* s2 = (const char*)get_host_ptr(ctx, r[5]);
                r[2] = (s1 && s2) ? (uint32_t)strncmp(s1, s2, r[6]) : 0;
            }
            return true;
        case 0x19: // strcpy(dst, src)
            {
                char* dst = (char*)get_host_ptr(ctx, r[4]);
                const char* src = (const char*)get_host_ptr(ctx, r[5]);
                if (dst && src) strcpy(dst, src);
                r[2] = r[4];
            }
            return true;
        case 0x1A: // strncpy(dst, src, n)
            {
                char* dst = (char*)get_host_ptr(ctx, r[4]);
                const char* src = (const char*)get_host_ptr(ctx, r[5]);
                if (dst && src) strncpy(dst, src, r[6]);
                r[2] = r[4];
            }
            return true;
        case 0x1B: // strlen(s)
            {
                const char* s = (const char*)get_host_ptr(ctx, r[4]);
                r[2] = s ? (uint32_t)strlen(s) : 0;
            }
            return true;
        case 0x27: // memcpy(dst, src, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                void* src = get_host_ptr(ctx, r[5]);
                if (dst && src) {
                    memcpy(dst, src, r[6]);
                }
                r[2] = r[4]; // return dst
            }
            return true;
        case 0x28: // memset(dst, val, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                if (dst) {
                    memset(dst, (uint8_t)r[5], r[6]);
                }
                r[2] = r[4];
            }
            return true;
        case 0x2A: // memcmp(s1, s2, n)
            {
                void* s1 = get_host_ptr(ctx, r[4]);
                void* s2 = get_host_ptr(ctx, r[5]);
                r[2] = (s1 && s2) ? (uint32_t)memcmp(s1, s2, r[6]) : 0;
            }
            return true;
        case 0x39: // InitCARD(pad_enable)
        case 0x3C: // _bu_init()
            r[2] = 0;
            return true;
        case 0x3A: // StartCARD()
        case 0x3B: // StopCARD()
        case 0x4A: // InitSpu()
            r[2] = 1;
            return true;
        case 0x3E: // puts(str)
        case 0x3F: // printf(fmt, ...)
            {
                const char* fmt = (const char*)get_host_ptr(ctx, r[4]);
                if (fmt) {
                    printf("[PS1 TTY] %s\n", fmt);
                    fflush(stdout);
                }
                r[2] = 0;
            }
            return true;
        case 0x42: // rand()
            r[2] = (uint32_t)rand();
            return true;
        case 0x43: // srand(seed)
            srand(r[4]);
            r[2] = 0;
            return true;
        case 0x44: // FlushCache()
            return true;
        default:
            return false;
    }
}

void ps1_bios_deliver_event(PS1Context* ctx, uint32_t class_id, uint32_t spec) {
    for (int i = 0; i < PS1_MAX_EVENTS; i++) {
        if (ctx->bios.events[i].enabled &&
            ctx->bios.events[i].class_id == class_id &&
            (ctx->bios.events[i].spec == spec || ctx->bios.events[i].spec == 0)) {
            ctx->bios.events[i].triggered = true;
            if (ctx->bios.events[i].func != 0) {
                ps1_call_indirect(ctx, ctx->bios.events[i].func);
            }
        }
    }
}

bool ps1_bios_handle_b0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;

    switch (func) {
        case 0x12: // InitPAD(buf1, len1, buf2, len2)
        case 0x13: // StartPAD()
        case 0x14: // StopPAD()
            r[2] = 1;
            return true;
        case 0x17: // DeliverEvent(class, spec)
            ps1_bios_deliver_event(ctx, r[4], r[5]);
            r[2] = 0;
            return true;
        case 0x18: // OpenEvent(class, spec, mode, func)
            for (int i = 0; i < PS1_MAX_EVENTS; i++) {
                if (!ctx->bios.events[i].enabled && !ctx->bios.events[i].triggered) {
                    ctx->bios.events[i].class_id = r[4];
                    ctx->bios.events[i].spec = r[5];
                    ctx->bios.events[i].mode = r[6];
                    ctx->bios.events[i].func = r[7];
                    ctx->bios.events[i].enabled = true;
                    ctx->bios.events[i].triggered = false;
                    r[2] = i | 0xF0000000;
                    return true;
                }
            }
            r[2] = 0xFFFFFFFF;
            return true;
        case 0x19: // CloseEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = false;
                    r[2] = 1;
                }
            }
            return true;
        case 0x1A: // WaitEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    int attempts = 0;
                    while (!ctx->bios.events[ev_idx].triggered && attempts++ < 2000) {
                        ps1_cdrom_tick(ctx, 50000);
                        ps1_timers_tick(ctx, 50000);
                    }
                    ctx->bios.events[ev_idx].triggered = false;
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x1B: // TestEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS && ctx->bios.events[ev_idx].triggered) {
                    ctx->bios.events[ev_idx].triggered = false;
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x1C: // EnableEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = true;
                    r[2] = 1;
                }
            }
            return true;
        case 0x1D: // DisableEvent(event)
            {
                int ev_idx = r[4] & 0x1F;
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = false;
                    r[2] = 1;
                }
            }
            return true;
        case 0x20: // UnDeliverEvent(class, spec)
            for (int i = 0; i < PS1_MAX_EVENTS; i++) {
                if (ctx->bios.events[i].class_id == r[4] &&
                    (ctx->bios.events[i].spec == r[5] || r[5] == 0)) {
                    ctx->bios.events[i].triggered = false;
                }
            }
            r[2] = 0;
            return true;
        case 0x32: // open(name, mode)
            {
                const char* name = (const char*)get_host_ptr(ctx, r[4]);
                r[2] = (uint32_t)bios_file_open(ctx, name, r[5]);
            }
            return true;
        case 0x33: // lseek(fd, offset, seektype)
            r[2] = (uint32_t)bios_file_seek(ctx, r[4], (int)r[5], (int)r[6]);
            return true;
        case 0x34: // read(fd, dst, len)
            {
                void* dst = get_host_ptr(ctx, r[5]);
                r[2] = (uint32_t)bios_file_read(ctx, r[4], dst, (int)r[6]);
            }
            return true;
        case 0x35: // write(fd, src, len)
            r[2] = 0;
            return true;
        case 0x36: // close(fd)
            r[2] = (uint32_t)bios_file_close(ctx, r[4]);
            return true;
        case 0x3F: // InitHeap(addr, size)
            ctx->bios.heap_base = r[4];
            ctx->bios.heap_size = r[5];
            ctx->bios.heap_cur = r[4];
            r[2] = ctx->bios.heap_base;
            return true;
        case 0x56: // GetGraphType()
            r[2] = 2; // PSX
            return true;
        case 0x5B: // ChangeClearPAD(val)
            r[2] = 0;
            return true;
        case 0x5C: // CdSearchFile(fp, name)
            return do_cd_search_file(ctx);
        default:
            return false;
    }
}

bool ps1_bios_handle_c0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;
    switch (func) {
        case 0x00: // EnqueueTimerAndVblankIrqs(priority)
            r[2] = 1;
            return true;
        case 0x0A: // ChangeClearRCnt(rcnt, flag)
            r[2] = 0;
            return true;
        case 0x15: // CdSearchFile(fp, name)
            return do_cd_search_file(ctx);
        default:
            r[2] = 0;
            return true;
    }
}

bool ps1_bios_handle_syscall(PS1Context* ctx) {
    uint32_t call_code = ctx->cpu.r[4]; // $a0

    if (call_code == 0) {
        // ExceptionReturn()
        ps1_rfe(ctx);
        return true;
    } else if (call_code == 1) {
        // EnterCriticalSection()
        ctx->cpu.r[2] = ctx->cpu.cop0[COP0_SR];
        ctx->cpu.cop0[COP0_SR] &= ~SR_IEc;
        return true;
    } else if (call_code == 2) {
        // ExitCriticalSection()
        ctx->cpu.cop0[COP0_SR] |= SR_IEc;
        return true;
    }
    return false;
}
