// Neo Geo (MVS) runtime - shared declarations for interpreter and statically
// translated code. Target: KOF98 (MVS, NEO-MVS PROGSF1 w/ ALTERA protection).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using u8 = uint8_t; using u16 = uint16_t; using u32 = uint32_t; using u64 = uint64_t;
using s8 = int8_t; using s16 = int16_t; using s32 = int32_t;

// ---- timing geometry (24MHz master, 68k 12MHz, pixel 6MHz) ----
static constexpr int CYC_LINE = 768;            // 68k cycles per scanline
static constexpr int LINES_FRAME = 264;
static constexpr int VISIBLE_LINES = 224;
static constexpr u64 CYC_FRAME = (u64)CYC_LINE * LINES_FRAME;
static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 224;

// ---- 68000 CPU state ----
struct Cpu {
    u32 D[8], A[8];
    u32 pc;
    u8 X, N, Z, V, C;   // flags (0/1)
    u8 S, T, iml;       // supervisor, trace, interrupt mask level
    u64 cyc;            // absolute cycle counter
    int stopped;
};
extern Cpu cpu;

inline u16 cpu_get_sr() {
    return (u16)((cpu.T << 15) | (cpu.S << 13) | (cpu.iml << 8) |
                 (cpu.X << 4) | (cpu.N << 3) | (cpu.Z << 2) | (cpu.V << 1) | cpu.C);
}
inline void cpu_set_sr(u16 v) {
    cpu.T = (v >> 15) & 1; cpu.S = (v >> 13) & 1; cpu.iml = (v >> 8) & 7;
    cpu.X = (v >> 4) & 1; cpu.N = (v >> 3) & 1; cpu.Z = (v >> 2) & 1;
    cpu.V = (v >> 1) & 1; cpu.C = v & 1;
}
inline void cpu_set_ccr(u8 v) {
    cpu.X = (v >> 4) & 1; cpu.N = (v >> 3) & 1; cpu.Z = (v >> 2) & 1;
    cpu.V = (v >> 1) & 1; cpu.C = v & 1;
}

// ---- ROM / RAM images (loaded at startup) ----
extern u8 *g_prom;      // 5 MiB decrypted program image
extern u8 *g_bios;      // 128 KiB MVS BIOS
extern u8 *g_wram;      // 64 KiB work RAM
extern u8 *g_bram;      // 64 KiB battery-backed RAM
extern u16 *g_palram;   // 2 banks x 0x1000 words
extern u16 *g_vram;     // 0x8800 words video RAM

// ---- hardware flags ----
extern int g_use_cart_vectors;
extern int g_use_cart_audio;   // also = cart fix layer source
extern int g_save_ram_unlocked;
extern int g_palette_bank;     // 0 or 0x1000
extern int g_screen_shadow;
extern u32 g_bank_base;        // banked ROM window base in g_prom

// ---- interrupt state ----
extern int g_irq_vblank, g_irq_raster, g_irq3;   // pending flags
extern u64 g_irq2_cycle;                          // scheduled raster irq (0 = none)
extern u32 g_vcount_base_line;                    // line at g_frame_base
extern u64 g_frame_base;                          // cpu cycle at line 0 of current frame

// ---- memory access ----
u16 io_read16(u32 addr);
void io_write16(u32 addr, u16 data);
u16 io_read8(u32 addr);
void io_write8(u32 addr, u8 data);
u32 unmapped_word();           // last-bus-value approximation

inline u16 mem_read16_raw(u32 addr) {
    addr &= 0xFFFFFF;
    if (addr < 0x100000) {
        if (addr < 0x80 && !g_use_cart_vectors) return (u16)((g_bios[addr] << 8) | g_bios[addr + 1]);
        // KOF98 ALTERA overlay at 0x100-0x103
        if (addr == 0x100 || addr == 0x102) {
            extern u16 kof98_prot_r(u32 offset);
            return kof98_prot_r((addr - 0x100) >> 1);
        }
        return (u16)((g_prom[addr] << 8) | g_prom[addr + 1]);
    }
    if (addr < 0x200000) { u32 a = addr & 0xFFFE; return (u16)((g_wram[a] << 8) | g_wram[a + 1]); }
    if (addr < 0x300000) {
        u32 a = g_bank_base + (addr & 0xFFFFF);
        extern u8 g_oob_panic;
        if (a > 0x4FFFFE) {   // prom.bin is 5 MiB; keep a+1 in bounds
            if (!g_oob_panic) { g_oob_panic = 1; fprintf(stderr, "OOB prom read a=%06x bank=%06x pc=%06x\n", a, g_bank_base, cpu.pc); }
            return 0xFFFF;
        }
        return (u16)((g_prom[a] << 8) | g_prom[a + 1]);
    }
    if (addr < 0x400000) return io_read16(addr);
    if (addr < 0x800000) return g_palram[g_palette_bank + ((addr >> 1) & 0xFFF)];   // word index per bank
    if (addr < 0xC00000) return 0xFFFF;                       // memory card (absent)
    if (addr < 0xD00000) { u32 a = addr & 0x1FFFE; return (u16)((g_bios[a] << 8) | g_bios[a + 1]); }
    if (addr < 0xE00000) { u32 a = addr & 0xFFFE; return (u16)((g_bram[a] << 8) | g_bram[a + 1]); }
    return unmapped_word();
}

inline u8 mem_read8(u32 addr) {
    addr &= 0xFFFFFF;
    if (addr >= 0x300000 && addr < 0x400000) return io_read8(addr);
    u16 w = mem_read16_raw(addr & ~1u);
    return (u8)((addr & 1) ? (w & 0xFF) : (w >> 8));
}
inline u16 mem_read16(u32 addr) {
    if ((addr & 0xFFFFFF) >= 0x300000 && (addr & 0xFFFFFF) < 0x400000) return io_read16(addr & 0xFFFFFF);
    return mem_read16_raw(addr);
}
inline u32 mem_read32(u32 addr) { return ((u32)mem_read16(addr) << 16) | mem_read16(addr + 2); }

inline void mem_write16(u32 addr, u16 data) {
    addr &= 0xFFFFFF;
    if (addr < 0x100000) return;                    // ROM
    if (addr < 0x200000) { u32 a = addr & 0xFFFE; g_wram[a] = (u8)(data >> 8); g_wram[a + 1] = (u8)data; return; }
    if (addr < 0x300000) {
        if (addr >= 0x2FFFF0) {                     // banksel
            u32 bank = data & 7;
            if ((bank + 1) * 0x100000 < 0x600000) g_bank_base = (bank + 1) * 0x100000;
            else g_bank_base = 0x100000;
            return;
        }
        if (addr == 0x20AAAA) { extern void kof98_prot_w(u16 data); kof98_prot_w(data); return; }
        return;                                     // ROM window
    }
    if (addr < 0x400000) { io_write16(addr, data); return; }
    if (addr < 0x800000) { extern void pal_write(u32 offset, u16 data); pal_write((addr >> 1) & 0xFFF, data); return; }
    if (addr < 0xC00000) return;                    // memory card
    if (addr < 0xD00000) return;                    // BIOS ROM
    if (addr < 0xE00000) {
        // note: BIOS never unlocks SRAM via the latch, so writes are always allowed
        // (matches power-on behavior needed for the POST backup-RAM test).
        u32 a = addr & 0xFFFE;
        static int bram_log = -1;
        if (bram_log < 0) bram_log = getenv("KOF98_BRAMLOG") ? 1 : 0;
        if (bram_log && a < 0x200)
            fprintf(stderr, "BRAMW %06x=%04x pc=%06x cyc=%llu\n", a, data, cpu.pc, (unsigned long long)cpu.cyc);
        g_bram[a] = (u8)(data >> 8); g_bram[a + 1] = (u8)data;
        return;
    }
}
inline void mem_write8(u32 addr, u8 data) {
    addr &= 0xFFFFFF;
    if (addr >= 0x300000 && addr < 0x400000) { io_write8(addr, data); return; }
    u32 a = addr & ~1u;
    u16 w = mem_read16_raw(a);
    w = (addr & 1) ? ((w & 0xFF00) | data) : ((w & 0x00FF) | ((u16)data << 8));
    mem_write16(a, w);
}
inline void mem_write32(u32 addr, u32 data) { mem_write16(addr, (u16)(data >> 16)); mem_write16(addr + 2, (u16)data); }

// ---- execution ----
void cpu_reset(int cold);
void cpu_run_until(u64 target);
void cpu_interp_step();
void cpu_check_irq();
void update_irq_level();
extern int g_irq_level;     // highest pending level (0 = none)

// translated code segment table: 256 slots (16MB / 64KB), nullptr = interpret
typedef void (*SegFn)();
extern SegFn g_segtab[256];
extern u8 g_seg_code[256];  // per-64KB: 1 if segment has translated code
// coverage log (debug builds): executed ROM addresses
void cov_mark(u32 addr);
void cov_dump();

// ---- z80 audio cpu ----
void z80_reset();
void z80_run_until(u64 target);   // target in z80 cycles (= 68k cycles / 3)
void z80_nmi();
void z80_set_irq(int on);
u32 z80_get_pc();
u8 z80_get_a();
extern u64 g_z80_cyc;

// ---- events / machine ----
void machine_init();
void machine_frame();       // run one video frame
void render_frame_done();
extern int g_vpos;          // current scanline (updated as frame runs)
extern int g_ntrace;        // native checkpoint for crash debugging

// ---- audio ring (ym2610 -> platform audio) ----
void audio_ring_push(s16 l, s16 r);
u32 audio_ring_avail();
u32 audio_ring_pop(s16 *dst, u32 frames);
void audio_ring_clear();

// guarded allocation (debug: find heap overflows)
u8 *alloc_guarded(const char *name, size_t size);
void guards_check(int frame);

// ---- video ----
void video_init();
void video_line(int line);  // render one visible scanline
extern u32 *g_fb;           // 320x224 RGBA framebuffer
void pal_write(u32 offset, u16 data);

// ---- rtc stub ----
u8 rtc_read_data();
void rtc_write(u8 data);

// ---- input (set by platform layer) ----
extern u8 g_in_p1, g_in_p2;       // active-low: bit0 up,1 down,2 left,3 right,4 A,5 B,6 C,7 D
extern u8 g_in_start;             // bit0 P1 start, bit2 P2 start (active HIGH in SYSTEM)
extern u8 g_in_coin;              // AUDIO_COIN low byte bits (active low): 0 c1,1 c2,2 service1,3 c3,4 c4
extern u8 g_in_select;            // SYSTEM: bit1 ->0x0200, bit3 ->0x0800 (active low)
extern int g_in_service;          // TEST port 0x0080 (active low)
extern u8 g_dsw;                  // dipswitches (default 0xFF)

// misc
extern FILE *g_trace;
extern int g_kof98_prot_state;
