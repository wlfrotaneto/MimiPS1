#include "mimips1_bios.h"
#include "mimips1_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Diagnostic: report each unimplemented BIOS function only once
static void bios_log_unhandled(char table, uint32_t func, PS1Context* ctx) {
    static uint8_t seen[3][256];
    int t = (table == 'A') ? 0 : (table == 'B') ? 1 : 2;
    uint32_t f = func & 0xFF;
    if (seen[t][f]) return;
    seen[t][f] = 1;
    fprintf(stderr, "[BIOS] Unimplemented %c0:%02X (ra=0x%08X a0=0x%08X a1=0x%08X)\n",
            table, f, ctx->cpu.r[31], ctx->cpu.r[4], ctx->cpu.r[5]);
}

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
    if (paddr < 0x00800000) {
        return &ctx->memory.ram[paddr & PS1_RAM_MIRROR_MASK];
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

    // Check if Memory Card path (bu00: or bu10:)
    bool is_bu00 = (strncasecmp(name, "bu00:", 5) == 0);
    bool is_bu10 = (strncasecmp(name, "bu10:", 5) == 0);
    if (is_bu00 || is_bu10) {
        int slot = is_bu00 ? 0 : 1;
        const char* card_file = name + 5;
        while (*card_file == '/' || *card_file == '\\') card_file++;

        int dir_idx = ps1_card_find_file(&ctx->card_mgr, slot, card_file);
        if (dir_idx < 0) {
            // If opening with write/create mode
            if (mode & 0x0200 || mode == 2 || mode == 0x200) {
                dir_idx = ps1_card_create_file(&ctx->card_mgr, slot, card_file, 8192);
            }
        }

        if (dir_idx >= 0) {
            for (int i = 0; i < 16; i++) {
                if (!ctx->bios.files[i].is_open) {
                    ctx->bios.files[i].is_open = true;
                    ctx->bios.files[i].is_card = true;
                    ctx->bios.files[i].card_slot = slot;
                    ctx->bios.files[i].card_file_idx = dir_idx;
                    ctx->bios.files[i].file_size = ctx->card_mgr.cards[slot].entries[dir_idx].file_size;
                    if (ctx->bios.files[i].file_size == 0) ctx->bios.files[i].file_size = 8192;
                    ctx->bios.files[i].cur_offset = 0;
                    strncpy(ctx->bios.files[i].name, card_file, sizeof(ctx->bios.files[i].name) - 1);
                    printf("[BIOS] Memory Card open('%s') -> fd=%d (Slot %d, block %d)\n", name, i, slot, dir_idx + 1);
                    return i;
                }
            }
        }
        printf("[BIOS] Memory Card file '%s' NOT FOUND on Slot %d\n", card_file, slot);
        return -1;
    }

    // Standard ISO CD-ROM path
    PS1ISOFile iso_file;
    if (!ps1_cdrom_find_file(&ctx->cdrom, name, &iso_file)) {
        printf("[BIOS] open('%s') NOT FOUND on disc\n", name);
        return -1;
    }

    for (int i = 0; i < 16; i++) {
        if (!ctx->bios.files[i].is_open) {
            ctx->bios.files[i].is_open = true;
            ctx->bios.files[i].is_card = false;
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

    if (f->is_card) {
        int r = ps1_card_read_file(&ctx->card_mgr, f->card_slot, f->card_file_idx, f->cur_offset, dst, (uint32_t)length);
        if (r > 0) f->cur_offset += r;
        return r;
    }

    uint32_t avail = (f->cur_offset < f->file_size) ? (f->file_size - f->cur_offset) : 0;
    uint32_t to_read = (uint32_t)length < avail ? (uint32_t)length : avail;
    if (to_read == 0) return 0;

    if (!ps1_cdrom_read_bytes(&ctx->cdrom, f->start_lba, f->cur_offset, dst, to_read)) {
        return -1;
    }
    f->cur_offset += to_read;
    return (int)to_read;
}

static int bios_file_write(PS1Context* ctx, int fd, const void* src, int length) {
    if ((fd == 1 || fd == 2) && src && length > 0) {
        fwrite(src, 1, length, stdout);
        fflush(stdout);
        return length;
    }
    if (fd < 0 || fd >= 16 || !ctx->bios.files[fd].is_open || !src || length <= 0) return -1;
    PS1OpenFile* f = &ctx->bios.files[fd];

    if (f->is_card) {
        int w = ps1_card_write_file(&ctx->card_mgr, f->card_slot, f->card_file_idx, f->cur_offset, src, (uint32_t)length);
        if (w > 0) {
            f->cur_offset += w;
            if (f->cur_offset > f->file_size) f->file_size = f->cur_offset;
        }
        return w;
    }
    return 0;
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
    if (fd >= 0 && fd < 16 && ctx->bios.files[fd].is_open) {
        if (ctx->bios.files[fd].is_card) {
            ps1_card_save(&ctx->card_mgr, ctx->bios.files[fd].card_slot);
        }
        ctx->bios.files[fd].is_open = false;
    }
    return 0;
}

static void ps1_bios_vprintf(PS1Context* ctx, const char* fmt) {
    if (!fmt) return;
    char out_buf[1024];
    int out_pos = 0;
    int arg_idx = 0;
    uint32_t* r = ctx->cpu.r;

    for (int i = 0; fmt[i] != '\0' && out_pos < (int)(sizeof(out_buf) - 64); i++) {
        if (fmt[i] == '%' && fmt[i+1] != '\0') {
            i++;
            if (fmt[i] == '%') {
                out_buf[out_pos++] = '%';
                continue;
            }

            uint32_t val = 0;
            if (arg_idx == 0) val = r[5];
            else if (arg_idx == 1) val = r[6];
            else if (arg_idx == 2) val = r[7];
            else val = ps1_mem_read32(ctx, r[29] + 16 + (arg_idx - 3) * 4);
            arg_idx++;

            char sub_fmt[16];
            int sub_idx = 0;
            sub_fmt[sub_idx++] = '%';
            while (fmt[i] && (fmt[i] == '0' || (fmt[i] >= '1' && fmt[i] <= '9') || fmt[i] == '.' || fmt[i] == '-' || fmt[i] == 'l')) {
                if (sub_idx < 14) sub_fmt[sub_idx++] = fmt[i];
                i++;
            }
            char spec = fmt[i];
            sub_fmt[sub_idx++] = spec;
            sub_fmt[sub_idx] = '\0';

            if (spec == 's') {
                const char* s = (const char*)get_host_ptr(ctx, val);
                out_pos += snprintf(&out_buf[out_pos], sizeof(out_buf) - out_pos, "%s", s ? s : "(null)");
            } else if (spec == 'd' || spec == 'i') {
                out_pos += snprintf(&out_buf[out_pos], sizeof(out_buf) - out_pos, sub_fmt, (int32_t)val);
            } else if (spec == 'u' || spec == 'x' || spec == 'X' || spec == 'p') {
                out_pos += snprintf(&out_buf[out_pos], sizeof(out_buf) - out_pos, sub_fmt, val);
            } else if (spec == 'c') {
                out_buf[out_pos++] = (char)val;
            }
        } else {
            out_buf[out_pos++] = fmt[i];
        }
    }
    out_buf[out_pos] = '\0';
    printf("[PS1 TTY] %s\n", out_buf);
    fflush(stdout);
}

bool ps1_bios_handle_a0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;
    if (func != 0x2A && func != 0x27 && func != 0x28 && func != 0x29 && func != 0x2B && func != 0x2C && func != 0x2D) {
        PS1_LOG(2, "[BIOS A0] func=0x%02X, a0=0x%08X, a1=0x%08X, a2=0x%08X, ra=0x%08X\n", func, r[4], r[5], r[6], r[31]);
    }

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
            {
                const void* src = get_host_ptr(ctx, r[5]);
                r[2] = (uint32_t)bios_file_write(ctx, r[4], src, (int)r[6]);
            }
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
        case 0x27: // bcopy(src, dst, len)
            {
                void* src = get_host_ptr(ctx, r[4]);
                void* dst = get_host_ptr(ctx, r[5]);
                if (dst && src) {
                    memmove(dst, src, r[6]);
                }
                r[2] = r[5];
            }
            return true;
        case 0x28: // bzero(dst, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                if (dst) {
                    memset(dst, 0, r[5]);
                }
                r[2] = r[4];
            }
            return true;
        case 0x2B: // memset(dst, val, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                if (dst) {
                    memset(dst, (uint8_t)r[5], r[6]);
                }
                r[2] = r[4];
            }
            return true;
        case 0x2C: // memmove(dst, src, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                void* src = get_host_ptr(ctx, r[5]);
                if (dst && src) {
                    memmove(dst, src, r[6]);
                }
                r[2] = r[4];
            }
            return true;
        case 0x29: // bcmp(s1, s2, len)
            {
                const void* s1 = get_host_ptr(ctx, r[4]);
                const void* s2 = get_host_ptr(ctx, r[5]);
                r[2] = (s1 && s2) ? (uint32_t)memcmp(s1, s2, r[6]) : 0;
            }
            return true;
        case 0x2A: // memcpy(dst, src, len)
            {
                void* dst = get_host_ptr(ctx, r[4]);
                const void* src = get_host_ptr(ctx, r[5]);
                if (dst && src) {
                    memcpy(dst, src, r[6]);
                }
                r[2] = r[4];
            }
            return true;
        case 0x2D: // memcmp(s1, s2, len)
            {
                const void* s1 = get_host_ptr(ctx, r[4]);
                const void* s2 = get_host_ptr(ctx, r[5]);
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
                    ps1_bios_vprintf(ctx, fmt);
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
        case 0x49: // GPU_cw(val)
            ps1_gpu_write_gp0(&ctx->gpu, r[4]);
            r[2] = 0;
            return true;
        case 0x70: // _bu_init()
        case 0x71: // _bu_deinit()
            r[2] = 0;
            return true;
        case 0x72: // SetCustomExitFromException / SetInterruptCallbackTable
            ctx->bios.irq_callback_table = r[5]; // r[5] ($a1) is the callback table
            printf("[BIOS A0:72] SetInterruptCallbackTable at 0x%08X (struct=0x%08X)\n", r[5], r[4]);
            r[2] = 0;
            return true;
        case 0xAD: // set_card_auto_format(flag)
            r[2] = 0;
            return true;
        default:
            bios_log_unhandled('A', func, ctx);
            return false;
    }
}

void ps1_bios_deliver_event(PS1Context* ctx, uint32_t class_id, uint32_t spec) {
    for (int i = 0; i < PS1_MAX_EVENTS; i++) {
        if (ctx->bios.events[i].is_open && ctx->bios.events[i].enabled &&
            ctx->bios.events[i].class_id == class_id &&
            (ctx->bios.events[i].spec == spec || ctx->bios.events[i].spec == 0)) {
            ctx->bios.events[i].triggered = true;
            if (ctx->bios.events[i].func != 0 && ctx->bios.events[i].func != 0xFFFFFFFF && !ctx->in_irq_callback) {
                // Save complete CPU context so interrupt callback does not corrupt active registers
                uint32_t saved_r[32];
                uint32_t saved_pc = ctx->cpu.pc;
                uint32_t saved_hi = ctx->cpu.hi;
                uint32_t saved_lo = ctx->cpu.lo;
                memcpy(saved_r, ctx->cpu.r, sizeof(saved_r));

                uint32_t p_sp = ctx->cpu.r[29] & 0x1FFFFFFF;
                if (p_sp < 0x2000 || p_sp >= PS1_RAM_SIZE) {
                    ctx->cpu.r[29] = 0x801FFF00; // Safe dedicated interrupt stack
                }

                bool prev_in_tick = ctx->in_tick;
                bool prev_irq = ctx->in_irq_callback;
                ctx->in_tick = false;
                ctx->in_irq_callback = true;

                ps1_call_indirect(ctx, ctx->bios.events[i].func);

                ctx->in_irq_callback = prev_irq;
                ctx->in_tick = prev_in_tick;

                memcpy(ctx->cpu.r, saved_r, sizeof(saved_r));
                ctx->cpu.pc = saved_pc;
                ctx->cpu.hi = saved_hi;
                ctx->cpu.lo = saved_lo;
            }
        }
    }
}

bool ps1_bios_handle_b0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;
    if (func != 0x3D && func != 0x16) {
        PS1_LOG(2, "[BIOS B0] 0x%02X (a0=0x%08X, a1=0x%08X, ra=0x%08X)\n", func, r[4], r[5], r[31]);
    }

    switch (func) {
        case 0x12: // InitPAD(buf1, len1, buf2, len2)
            ctx->bios.pad_buf[0] = r[4];
            ctx->bios.pad_len[0] = r[5];
            ctx->bios.pad_buf[1] = r[6];
            ctx->bios.pad_len[1] = r[7];
            r[2] = 1;
            return true;
        case 0x13: // StartPAD()
            ctx->bios.pad_active = true;
            r[2] = 1;
            return true;
        case 0x14: // StopPAD()
            ctx->bios.pad_active = false;
            r[2] = 1;
            return true;
        case 0x15: // OutdatedPadInitAndStart(type, button_dest, unused, unused)
            ctx->bios.pad_button_dest = r[5];
            ctx->bios.pad_active = true;
            r[2] = 2;
            return true;
        case 0x16: // OutdatedPadGetButtons()
            {
                uint16_t b0 = ctx->input.pads[0].buttons;
                uint16_t b1 = ctx->input.pads[1].buttons;
                uint16_t rev0 = ((b0 & 0xFF) << 8) | ((b0 >> 8) & 0xFF);
                uint16_t rev1 = ((b1 & 0xFF) << 8) | ((b1 >> 8) & 0xFF);
                uint32_t ret = ((uint32_t)rev1 << 16) | rev0;
                if (ctx->bios.pad_button_dest != 0) {
                    ps1_mem_write32(ctx, ctx->bios.pad_button_dest, ret);
                }
                r[2] = ret;
            }
            return true;
        case 0x07: // DeliverEvent(class, spec) (Rev 1)
        case 0x17: // DeliverEvent(class, spec) (Rev 2)
            ps1_bios_deliver_event(ctx, r[4], r[5]);
            r[2] = 0;
            return true;
        case 0x08: // OpenEvent(class, spec, mode, func) (Rev 1)
        case 0x18: // OpenEvent(class, spec, mode, func) (Rev 2)
            for (int i = 0; i < PS1_MAX_EVENTS; i++) {
                if (!ctx->bios.events[i].is_open) {
                    ctx->bios.events[i].class_id = r[4];
                    ctx->bios.events[i].spec = r[5];
                    ctx->bios.events[i].mode = r[6];
                    ctx->bios.events[i].func = r[7];
                    ctx->bios.events[i].is_open = true;
                    ctx->bios.events[i].enabled = false;
                    ctx->bios.events[i].triggered = false;
                    r[2] = i | 0xF0000000;
                    return true;
                }
            }
            r[2] = 0xFFFFFFFF;
            return true;
        case 0x09: // CloseEvent(event)
            {
                int ev_idx = r[4] & (PS1_MAX_EVENTS - 1);
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].is_open = false;
                    ctx->bios.events[ev_idx].enabled = false;
                    ctx->bios.events[ev_idx].triggered = false;
                    r[2] = 1;
                }
            }
            return true;
        case 0x0A: // WaitEvent(event)
            {
                int ev_idx = r[4] & (PS1_MAX_EVENTS - 1);
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].triggered = false;
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x19: // setjmp(env)
            {
                uint32_t* env = (uint32_t*)get_host_ptr(ctx, r[4]);
                if (env) {
                    for (int i = 0; i < 16; i++) env[i] = r[16 + i];
                }
                r[2] = 0;
            }
            return true;
        case 0x1A: // longjmp(env, val)
            {
                uint32_t* env = (uint32_t*)get_host_ptr(ctx, r[4]);
                if (env) {
                    for (int i = 0; i < 16; i++) r[16 + i] = env[i];
                }
                r[2] = r[5] ? r[5] : 1;
            }
            return true;
        case 0x0B: // TestEvent(event) (Rev 1)
        case 0x1B: // TestEvent(event) (Rev 2)
            {
                int ev_idx = r[4] & (PS1_MAX_EVENTS - 1);
                if (ev_idx < PS1_MAX_EVENTS) {
                    bool was_trig = ctx->bios.events[ev_idx].triggered;
                    ctx->bios.events[ev_idx].triggered = false;
                    r[2] = was_trig ? 1 : 0;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x0C: // EnableEvent(event) (Rev 1)
        case 0x1C: // EnableEvent(event) (Rev 2)
            {
                int ev_idx = r[4] & (PS1_MAX_EVENTS - 1);
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = true;
                    r[2] = 1;
                }
            }
            return true;
        case 0x0D: // DisableEvent(event) (Rev 1)
        case 0x1D: // DisableEvent(event) (Rev 2)
            {
                int ev_idx = r[4] & (PS1_MAX_EVENTS - 1);
                if (ev_idx < PS1_MAX_EVENTS) {
                    ctx->bios.events[ev_idx].enabled = false;
                    r[2] = 1;
                }
            }
            return true;
        case 0x3D: // std_out_putchar(c)
            putchar((char)(r[4] & 0xFF));
            fflush(stdout);
            r[2] = r[4];
            return true;
        case 0x3E: // std_out_puts(s)
            {
                const char* s = (const char*)get_host_ptr(ctx, r[4]);
                if (s) {
                    printf("%s\n", s);
                    fflush(stdout);
                }
                r[2] = 0;
                return true;
            }
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
            {
                const void* src = get_host_ptr(ctx, r[5]);
                r[2] = (uint32_t)bios_file_write(ctx, r[4], src, (int)r[6]);
            }
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
        case 0x4A: // _card_info(port)
        case 0x4B: // _card_status(port)
            r[2] = 1;
            return true;
        case 0x4E: // _card_read(port, block, buf)
            {
                int port = (int)r[4];
                int block = (int)r[5];
                void* buf = get_host_ptr(ctx, r[6]);
                if (buf && ps1_card_read_frame(&ctx->card_mgr, port, block, buf)) {
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x4F: // _card_write(port, block, buf)
            {
                int port = (int)r[4];
                int block = (int)r[5];
                const void* buf = get_host_ptr(ctx, r[6]);
                if (buf && ps1_card_write_frame(&ctx->card_mgr, port, block, buf)) {
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        case 0x50: // _card_clear(port)
            r[2] = 1;
            return true;
        case 0x56: // GetGraphType() / _ChangeClearPAD
            r[2] = 2; // PSX
            return true;
        case 0x57: // GetB0Table()
            r[2] = 0;
            return true;
        case 0x5B: // ChangeClearPAD(val) / ChangeClearRCnt
            r[2] = 0;
            return true;
        case 0x5C: // CdSearchFile(fp, name)
            return do_cd_search_file(ctx);
        case 0x5D: // format(name)
            {
                const char* name = (const char*)get_host_ptr(ctx, r[4]);
                if (name) {
                    int slot = (strncasecmp(name, "bu10:", 5) == 0) ? 1 : 0;
                    ps1_card_format(&ctx->card_mgr, slot);
                    ps1_card_save(&ctx->card_mgr, slot);
                    r[2] = 1;
                } else {
                    r[2] = 0;
                }
            }
            return true;
        default:
            bios_log_unhandled('B', func, ctx);
            return false;
    }
}

bool ps1_bios_handle_c0(PS1Context* ctx, uint32_t func) {
    uint32_t* r = ctx->cpu.r;
    switch (func) {
        case 0x00: // EnqueueTimerAndVblankIrqs(priority)
            r[2] = 1;
            return true;
        case 0x02: // SysEnqIntRP(priority, queue_struct)
            {
                uint32_t prio = r[4] & 3;
                uint32_t new_node = r[5];
                if (new_node != 0) {
                    uint32_t old_head = ctx->bios.intrp_heads[prio];
                    ps1_mem_write32(ctx, new_node, old_head); // new_node->next = old_head
                    ctx->bios.intrp_heads[prio] = new_node;
                    uint32_t fn = ps1_mem_read32(ctx, new_node + 4);
                    printf("[BIOS C0:02] SysEnqIntRP(prio=%u, node=0x%08X, func=0x%08X)\n", prio, new_node, fn);
                }
                r[2] = 0;
                return true;
            }
        case 0x03: // SysDeqIntRP(priority, queue_struct)
            {
                uint32_t prio = r[4] & 3;
                uint32_t target_node = r[5];
                uint32_t cur = ctx->bios.intrp_heads[prio];
                uint32_t prev = 0;
                while (cur != 0) {
                    if (cur == target_node) {
                        uint32_t next = ps1_mem_read32(ctx, cur);
                        if (prev != 0) {
                            ps1_mem_write32(ctx, prev, next);
                        } else {
                            ctx->bios.intrp_heads[prio] = next;
                        }
                        break;
                    }
                    prev = cur;
                    cur = ps1_mem_read32(ctx, cur);
                }
                r[2] = target_node;
                return true;
            }
        case 0x0A: // ChangeClearRCnt(rcnt, flag)
            r[2] = 0;
            return true;
        case 0x15: // CdSearchFile(fp, name)
            return do_cd_search_file(ctx);
        default:
            bios_log_unhandled('C', func, ctx);
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

void ps1_bios_update_pads(PS1Context* ctx) {
    if (!ctx || !ctx->bios.pad_active) return;

    if (ctx->bios.pad_button_dest != 0) {
        uint16_t b0 = ctx->input.pads[0].buttons;
        uint16_t b1 = ctx->input.pads[1].buttons;
        uint16_t rev0 = ((b0 & 0xFF) << 8) | ((b0 >> 8) & 0xFF);
        uint16_t rev1 = ((b1 & 0xFF) << 8) | ((b1 >> 8) & 0xFF);
        uint32_t ret = ((uint32_t)rev1 << 16) | rev0;
        ps1_mem_write32(ctx, ctx->bios.pad_button_dest, ret);
    }

    for (int i = 0; i < 2; i++) {
        uint32_t buf_addr = ctx->bios.pad_buf[i];
        uint32_t len = ctx->bios.pad_len[i];
        if (!buf_addr || len < 4) continue;

        uint8_t* p = (uint8_t*)get_host_ptr(ctx, buf_addr);
        if (!p) continue;

        PS1Controller* pad = &ctx->input.pads[i];
        p[0] = 0x00; // Success / controller connected
        p[1] = pad->is_analog ? 0x73 : 0x41; // Terminal type
        p[2] = (uint8_t)(pad->buttons & 0xFF);
        p[3] = (uint8_t)((pad->buttons >> 8) & 0xFF);

        if (pad->is_analog && len >= 8) {
            p[4] = pad->right_stick_x;
            p[5] = pad->right_stick_y;
            p[6] = pad->left_stick_x;
            p[7] = pad->left_stick_y;
        }
    }
}
