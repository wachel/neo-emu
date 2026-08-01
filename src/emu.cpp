// Machine state, memory-map I/O, ROM loading, event scheduling.
#include "rt.h"
#include "romload.h"

u8 *g_prom, *g_bios, *g_wram, *g_bram, *g_crom, *g_sfix, *g_s1, *g_zoomy;
u8 *g_m1, *g_sm1, *g_vrom;
u8 g_oob_panic;
int g_ntrace;
u16 *g_palram, *g_vram;
u32 *g_fb;
Cpu cpu;
int g_use_cart_vectors, g_use_cart_audio, g_save_ram_unlocked;
int g_palette_bank, g_screen_shadow;
u32 g_bank_base = 0x100000;
int g_irq_vblank, g_irq_raster, g_irq3, g_irq_level;
u64 g_irq2_cycle;
u64 g_frame_base;
int g_vpos;
FILE *g_trace;
int g_kof98_prot_state;
u8 g_in_p1 = 0xFF, g_in_p2 = 0xFF, g_in_start, g_in_coin = 0xFF, g_in_select = 0xFF;
int g_in_service = 1;   // active low: 1 = not pressed
u8 g_dsw = 0xFF;

// video register state
u16 g_vram_offset, g_vram_modulo, g_vram_readbuf;
u8 g_auto_anim_speed, g_auto_anim_disabled, g_auto_anim_counter, g_auto_anim_frame;
u32 g_display_counter;      // raster timer load value
u8 g_irq2_ctrl;             // data & 0xF0 of video control write
u64 g_watchdog_cycle;
u8 g_sound_reply;
int g_frame_count;

// coverage
static u8 *g_cov;           // 16MB bitmap of instruction starts executed
int g_cov_enabled;          // off by default (KOF98_COV=1 enables); per-instr cost otherwise

static u8 *load_file(const char *path, size_t *sz) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    u8 *b = (u8 *)malloc(n);
    if (fread(b, 1, n, f) != (size_t)n) { fprintf(stderr, "read error %s\n", path); exit(1); }
    fclose(f); *sz = n; return b;
}

// ---- KOF98 ALTERA protection ----
static u16 g_prot_rom0, g_prot_rom1;
u16 kof98_prot_r(u32 offset) {
    if (g_kof98_prot_state == 1) return offset ? 0x00fd : 0x00c2;
    if (g_kof98_prot_state == 2) return offset ? 0x4f2d : 0x4e45;
    return offset ? g_prot_rom1 : g_prot_rom0;
}
void kof98_prot_w(u16 data) {
    if (data == 0x0090) g_kof98_prot_state = 1;
    else if (data == 0x00f0) g_kof98_prot_state = 2;
}

// ---- unmapped read: last word on bus ~ next opcode word ----
u32 unmapped_word() {
    u32 pc = cpu.pc & 0xFFFFFE;
    if ((pc >> 16) == 0xC0) { u32 a = pc & 0x1FFFF; return (g_bios[a] << 8) | g_bios[a + 1]; }
    if (pc < 0x100000) return (g_prom[pc] << 8) | g_prom[pc + 1];
    if (pc < 0x200000) { u32 a = pc & 0xFFFF; return (g_wram[a] << 8) | g_wram[a + 1]; }
    if (pc < 0x300000) { u32 a = g_bank_base + (pc & 0xFFFFF); return (g_prom[a] << 8) | g_prom[a + 1]; }
    return 0xFFFF;
}

// ---- RTC (uPD4990A) stub: shift out current time, enough for BIOS ----
static u8 rtc_data_in, rtc_clk, rtc_stb;
static u8 rtc_shift[7];     // sec min hour day week month year (BCD), simplified
static int rtc_bitpos;
static u8 rtc_out;
u8 rtc_read_data() { return rtc_out; }
void rtc_write(u8 data) {
    u8 d = data & 1, c = (data >> 1) & 1, s = (data >> 2) & 1;
    if (s && !rtc_stb) {    // strobe rising: latch time
        rtc_bitpos = 0;
        rtc_out = rtc_shift[0] & 1;
    }
    rtc_stb = s;
    if (c && !rtc_clk) {    // clock rising: shift
        rtc_bitpos = (rtc_bitpos + 1) % 56;
        rtc_out = (rtc_shift[rtc_bitpos / 8] >> (rtc_bitpos % 8)) & 1;
    }
    rtc_clk = c; rtc_data_in = d;
}

// ---- Z80 audio subsystem ----
static u8 g_z80_ram[0x800];
static u32 g_z80_bank_base[4];  // g_m1 byte offsets: [0]=f000(2K) [1]=e000(4K) [2]=c000(8K) [3]=8000(16K)
static u32 g_m1_sz = 0x40000;   // 实际 M1 大小 (bank 掩码依赖它, 与 MAME 一致)
int g_z80_nmi_enabled;      // z80 armed NMI via out($08); idle-park is only safe after this
u8 g_sound_cmd;

static void z80_banks_reset() {
    // NEO-ZMC power-on defaults (MAME init_audio hack values)
    g_z80_bank_base[0] = 0x10000 + (0x1e << 11);
    g_z80_bank_base[1] = 0x10000 + (0x0e << 12);
    g_z80_bank_base[2] = 0x10000 + (0x06 << 13);
    g_z80_bank_base[3] = 0x10000 + (0x02 << 14);
    g_z80_nmi_enabled = 0;
}

// ym2610 chip (ym2610.cpp)
u8 ym2610_read(u16 port);
void ym2610_write(u16 port, u8 data);
void ym2610_run_until(u64 target);
void ym2610_reset();
void rec_state_event(void);

u8 z80_mem_read(u16 addr) {
    if (addr < 0x8000) return g_use_cart_audio ? g_m1[addr] : g_sm1[addr];
    // bank 窗口读: base 已在选择时按 MAME 公式掩码, 这里线性读 ROM, 不能再加掩码
    // (旧代码 & 0x4FFFF 会把 bit16/17 清掉, 所有 banked 读全部塌缩到 M1 低 64K!)
    u32 off; u8 v;
    if (addr < 0xC000)      off = g_z80_bank_base[3] + (addr & 0x3FFF);
    else if (addr < 0xE000) off = g_z80_bank_base[2] + (addr & 0x1FFF);
    else if (addr < 0xF000) off = g_z80_bank_base[1] + (addr & 0x0FFF);
    else if (addr < 0xF800) off = g_z80_bank_base[0] + (addr & 0x07FF);
    else return g_z80_ram[addr & 0x7FF];
    if (off >= 0x50000) off = 0x4FFFF;   // 防御 (理论上不会越界)
    v = g_m1[off];
    static FILE *rlog;
    static int rlog_init;
    if (!rlog_init) { rlog_init = 1; const char *p = getenv("KOF98_Z80RLOG"); if (p) rlog = fopen(p, "w"); }
    if (rlog) fprintf(rlog, "pc=%04x addr=%04x m1=%05x v=%02x cyc=%llu\n",
                      z80_get_pc(), addr, off, v, (unsigned long long)cpu.cyc);
    return v;
}
void z80_mem_write(u16 addr, u8 data) {
    if (addr >= 0xF800) g_z80_ram[addr & 0x7FF] = data;
}
u8 z80_io_read(u16 port) {
    u8 p = port & 0xFF;
    if (p >= 0x08 && p <= 0x0B) {   // NEO-ZMC bank select: READ with bank# in A8-A15
        int region = p - 0x08;
        u32 bank = (port >> 8) & 0xFF;
        // 与 MAME 一致: address_mask = (m1_size - 0x10000 - 1) & 0x3ffff
        // (此前固定 0x3FFFF, 大号 bank 号会映射到错误的 ROM 区域 -> 音序器读到垃圾数据,
        //  表现为 BGM 中途死掉或播放"奇怪的音乐")
        u32 address_mask = (g_m1_sz - 0x10000 - 1) & 0x3FFFF;
        g_z80_bank_base[region] = 0x10000 + ((bank << (11 + region)) & address_mask);
        if (getenv("KOF98_Z80LOG"))
            fprintf(stderr, "ZBANK r%d bank=%02x base=%05x zpc=%04x cmd=%02x cyc=%llu\n",
                    region, bank, g_z80_bank_base[region], z80_get_pc(), g_sound_cmd,
                    (unsigned long long)cpu.cyc);
        static int zt_done = 0;
        if (!zt_done && region == 0 && z80_get_pc() == 0x1138 && g_sound_cmd == 0x21 &&
            getenv("KOF98_ZTRACE")) {
            zt_done = 1;
            void z80_trace_dump(FILE *f);
            z80_trace_dump(stderr);
        }
        return 0;
    }
    switch (p) {
    case 0x00: return g_sound_cmd;
    case 0x04: case 0x05: case 0x06: case 0x07: return ym2610_read(p & 3);
    default: return 0xFF;
    }
}
void z80_io_write(u16 port, u8 data) {
    switch (port & 0xFF) {
    case 0x04: ym2610_write(0, data); break;
    case 0x05: ym2610_write(1, data); break;
    case 0x06: ym2610_write(2, data); break;
    case 0x07: ym2610_write(3, data); break;
    case 0x08: case 0x18:
        g_z80_nmi_enabled = !((port >> 4) & 1);   // out($08)=enable NMI, out($18)=disable
        break;
    case 0x0C:
        g_sound_reply = data;
        if (getenv("KOF98_Z80LOG"))
            fprintf(stderr, "Z80RPLY %02x cmd=%02x cyc=%llu\n", data, g_sound_cmd, (unsigned long long)cpu.cyc);
        break;
    }
}

// ---- I/O reads ----
u16 io_read16(u32 addr) {
    u32 zone = addr & 0x1F0000;
    switch (addr >> 16) {
    case 0x30: case 0x31:
        if (addr & 0x80) {  // TEST port
            u16 v = 0xFFBF;                     // bit7=1: service switch off (active low)
            if (!g_in_service) v &= ~0x0080;
            // bit6: JAMMA sense (MVS: 0)
            return v;
        }
        return (u16)((g_in_p1 << 8) | g_dsw);               // IN0
    case 0x32: case 0x33: { // AUDIO_COIN
        u16 lo = (u16)(g_in_coin & 0x1F);
        lo |= 0x20;                                         // sense: not 4-slot
        lo |= (u16)(((cpu.cyc / 6000000) & 1) << 6);        // rtc tp: 1Hz pulse (0.5s high/low)
        lo |= (u16)(rtc_read_data() << 7);
        return (u16)((g_sound_reply << 8) | lo);
    }
    case 0x34: case 0x35: return (u16)((g_in_p2 << 8) | 0xFF); // IN1
    case 0x36: case 0x37: return (u16)unmapped_word();
    case 0x38: case 0x39: { // SYSTEM (REG_STATUS_B): start/select are active low
        u16 v = 0xFFFF;
        if (g_in_start & 1) v &= ~0x0100;                   // P1 start
        if (g_in_start & 4) v &= ~0x0400;                   // P2 start
        if (!(g_in_select & 1)) v &= ~0x0200;               // select 1 (active low)
        if (!(g_in_select & 4)) v &= ~0x0800;               // select 2 (active low)
        return v;
    }
    case 0x3A: case 0x3B: return (u16)unmapped_word();
    case 0x3C: case 0x3D: { // video registers
        switch ((addr & 0xF) >> 1) {
        case 0: case 1: return g_vram_readbuf;
        case 2: return g_vram_modulo;
        case 3: {           // video control: raster counter
            u64 c = cpu.cyc - g_frame_base;
            int line = (int)((c / CYC_LINE) % LINES_FRAME);
            u16 vc = (u16)(line + 0x100);
            if (vc >= 0x200) vc -= LINES_FRAME;
            return (u16)((vc << 7) | (g_auto_anim_counter & 7));
        }
        }
        return 0xFFFF;
    }
    default: return (u16)unmapped_word();
    }
}
u16 io_read8(u32 addr) {
    u16 w = io_read16(addr & ~1u);
    return (u16)((addr & 1) ? (w & 0xFF) : (w >> 8));
}

static void vram_set_offset(u16 data) {
    g_vram_offset = (data & 0x8000) ? (data & 0x87FF) : data;
    if (g_vram_offset > 0x87FF) {   // hardware would wrap; guard the buffer
        static int warned = 0;
        if (!warned) { warned = 1; fprintf(stderr, "VRAMOFF %04x pc=%06x\n", data, cpu.pc); }
        g_vram_offset &= 0x87FF;
    }
    g_vram_readbuf = g_vram[g_vram_offset];
}

static void hc259_write(u32 addr, u8 data) {
    // 74HC259: odd byte writes; bit select = A1-A3, data = A4 (bus data ignored).
    // SWPBIOS=$3a0003(bit1=0) SWPROM=$3a0013(bit1=1) SRAMLOCK/UNLOCK=$3a000d/$3a001d(bit6)
    int bit = (addr >> 1) & 7;
    int val = (addr >> 4) & 1;
    if (getenv("KOF98_LATCHLOG"))
        fprintf(stderr, "LATCH %06x bit%d=%d pc=%06x\n", addr, bit, val, cpu.pc);
    switch (bit) {
    case 0: g_screen_shadow = val; break;
    case 1: g_use_cart_vectors = val; break;
    case 5: g_use_cart_audio = val; break;
    case 6: g_save_ram_unlocked = val; break;
    case 7: g_palette_bank = val ? 0x1000 : 0; break;
    }
}

void io_write16(u32 addr, u16 data) {
    // word writes: Neo Geo registers are mostly byte-wide; emulate as byte ops
    if ((addr >> 16) == 0x3C || (addr >> 16) == 0x3D) {
        switch ((addr & 0xF) >> 1) {
        case 0: vram_set_offset(data); return;
        case 1:
            if (g_vram_offset <= 0x87FF) g_vram[g_vram_offset] = data;
            else fprintf(stderr, "VRAMW OOB %04x pc=%06x\n", g_vram_offset, cpu.pc);
            vram_set_offset((u16)((g_vram_offset & 0x8000) | ((g_vram_offset + g_vram_modulo) & 0x7FFF)));
            return;
        case 2: g_vram_modulo = data; return;
        case 3:
            g_auto_anim_speed = (u8)(data >> 8);
            g_auto_anim_disabled = (data >> 3) & 1;
            g_irq2_ctrl = (u8)(data & 0xF0);
            return;
        case 4: g_display_counter = (g_display_counter & 0xFFFF) | ((u32)data << 16); return;
        case 5:
            g_display_counter = (g_display_counter & 0xFFFF0000) | data;
            if (g_irq2_ctrl & 0x20)     // LOAD_RELATIVE
                g_irq2_cycle = cpu.cyc + ((u64)g_display_counter + 1) * 2;
            return;
        case 6:
            if (getenv("KOF98_EXLOG"))
                fprintf(stderr, "IRQACK data=%d pc=%06x cyc=%llu\n", data, cpu.pc, (unsigned long long)cpu.cyc);
            if (data & 1) g_irq3 = 0;
            if (data & 2) g_irq_raster = 0;
            if (data & 4) g_irq_vblank = 0;
            update_irq_level();
            return;
        case 7: return;
        }
        return;
    }
    // for other zones treat as two byte writes (MSB first like real bus? do both)
    io_write8(addr, (u8)(data >> 8));
    io_write8(addr | 1, (u8)data);
}

void io_write8(u32 addr, u8 data) {
    switch (addr >> 16) {
    case 0x30: case 0x31:
        if (addr & 1) g_watchdog_cycle = cpu.cyc + 3244030 / 2;  // watchdog kick
        return;
    case 0x32: case 0x33:
        if (!(addr & 1)) {   // even byte: sound command latch (NMI if enabled)
            g_sound_cmd = data;
            if (getenv("KOF98_Z80LOG"))
                fprintf(stderr, "SNDCMD %02x pc=%06x cyc=%llu\n", data, cpu.pc, (unsigned long long)cpu.cyc);
            if (g_z80_nmi_enabled) z80_nmi();
        }
        return;
    case 0x38: case 0x39: { // io control (offset = addr & 0xFF)
        u32 off = addr & 0xFF;
        switch (off & 0x78) {
        case 0x28: rtc_write(data); break;
        default: break;     // coin counters / leds / slots: ignore
        }
        return;
    }
    case 0x3A: case 0x3B:
        hc259_write(addr, data);
        return;
    case 0x3C: case 0x3D:   // byte write to video regs: MSB-only stores replicate
        io_write16(addr & ~1u, (u16)((data << 8) | data));
        return;
    default: return;
    }
}

// ---- palette ----
static u8 g_pal_lut[32][4]; // [level 0..31][normal,dark,shadow,dark+shadow]
extern u32 *g_pens;         // 4096*2*2 RGBA pens
u32 *g_pens;

static void pal_init() {
    static const double r[5] = {3900, 2200, 1000, 470, 220};
    double sum = 0; for (int i = 0; i < 5; i++) sum += 1.0 / r[i];
    const double pd[4] = {0, 8200, 150, 1.0 / (1.0 / 8200 + 1.0 / 150)};
    for (int lv = 0; lv < 32; lv++) {
        for (int k = 0; k < 4; k++) {
            double denom = sum + (k ? 1.0 / pd[k] : 0);
            double acc = 0;
            for (int i = 0; i < 5; i++) if (lv & (1 << i)) acc += 1.0 / r[i];
            g_pal_lut[lv][k] = (u8)(255.0 * acc / denom + 0.5);
        }
    }
}

static inline u32 pen_rgb(int lvl5, int dark, int shadow) {
    int k = (shadow ? 2 : 0) + (dark ? 1 : 0);
    u8 v = g_pal_lut[lvl5 & 31][k];
    return v;
}

static void pen_decode(u32 off, u16 data) {
    int dark = data >> 15;
    int r = ((data >> 14) & 1) | ((data >> 7) & 0x1E);
    int g = ((data >> 13) & 1) | ((data >> 3) & 0x1E);
    int b = ((data >> 12) & 1) | ((data << 1) & 0x1E);
    for (int sh = 0; sh < 2; sh++) {
        u8 R = g_pal_lut[r][dark + 2 * sh], G = g_pal_lut[g][dark + 2 * sh], B = g_pal_lut[b][dark + 2 * sh];
        g_pens[off + 0x2000 * sh] = 0xFF000000 | (R << 16) | (G << 8) | B;
    }
}

void pal_write(u32 offset, u16 data) {
    u32 off = g_palette_bank + offset;
    g_palram[off] = data;
    pen_decode(off, data);
}

// rebuild the derived pens cache from palram (needed after state load)
static void pal_rebuild() {
    for (u32 i = 0; i < 0x2000; i++) pen_decode(i, g_palram[i]);
}

// ---- interrupts ----
void update_irq_level() {
    int lv = 0;
    if (g_irq_vblank) lv = 1;
    if (g_irq_raster) lv = 2;
    if (g_irq3) lv = 3;
    g_irq_level = lv;
}

void cpu_reset(int cold) {
    if (cold) {
        g_use_cart_vectors = 0; g_use_cart_audio = 0; g_save_ram_unlocked = 0;
        g_palette_bank = 0; g_screen_shadow = 0; g_bank_base = 0x100000;
        g_irq_vblank = g_irq_raster = 0; g_irq3 = 1;    // power-on IRQ3
        g_irq2_cycle = 0; g_irq2_ctrl = 0;
        g_kof98_prot_state = 0;
        g_sound_reply = 0;
        z80_reset();
        z80_banks_reset();
        ym2610_reset();
        update_irq_level();
    }
    cpu.S = 1; cpu.T = 0; cpu.iml = 7; cpu.stopped = 0;
    cpu.A[7] = mem_read32(0);
    cpu.pc = mem_read32(4);
    cpu.cyc = 0;
    g_watchdog_cycle = cpu.cyc + 3244030 / 2;
}

// ---- coverage ----
void bram_save() {
    FILE *f = fopen("bram.bin", "wb");
    if (f) { fwrite(g_bram, 1, 0x10000, f); fclose(f); }
}
void cov_mark(u32 addr) {
    if (g_cov_enabled && addr < 0x1000000) g_cov[addr >> 3] |= (u8)(1 << (addr & 7));
}
void cov_dump() {
    FILE *f = fopen("coverage.bin", "wb");
    if (f) { fwrite(g_cov, 1, 0x200000, f); fclose(f); }
}

// ---- guarded allocations (debug) ----
struct GuardBlk { const char *name; u8 *raw; size_t size; };
static GuardBlk g_guards[24];
static int g_nguards;
u8 *alloc_guarded(const char *name, size_t size) {
    u8 *p = (u8 *)malloc(size + 64);
    memset(p, 0xAA, 32);
    memset(p + 32, 0, size);
    memset(p + 32 + size, 0xBB, 32);
    g_guards[g_nguards].name = name;
    g_guards[g_nguards].raw = p;
    g_guards[g_nguards].size = size;
    g_nguards++;
    return p + 32;
}
void guards_check(int frame) {
    extern u32 g_pcring[1024]; extern u32 g_pcring_pos;
    for (int g = 0; g < g_nguards; g++) {
        GuardBlk *G = &g_guards[g];
        for (int i = 0; i < 32; i++) {
            int bad_head = G->raw[i] != 0xAA;
            int bad_tail = G->raw[32 + G->size + i] != 0xBB;
            if (bad_head || bad_tail) {
                fprintf(stderr, "GUARD %s %s i=%d frame=%d pc=%06x\nlast pcs:",
                        G->name, bad_head ? "head" : "tail", i, frame, cpu.pc);
                for (int k = 40; k >= 1; k--)
                    fprintf(stderr, " %06x", g_pcring[(g_pcring_pos - k) & 1023]);
                fprintf(stderr, "\n");
                exit(6);
            }
        }
    }
}

// ---- frame execution ----
int g_frame_restart;
static void rec_frame_event(void);
void machine_frame() {
    rec_frame_event();
    g_frame_restart = 0;
    for (int line = 0; line < LINES_FRAME; line++) {
        g_ntrace = 1;
        g_vpos = line;
        u64 line_end = g_frame_base + (u64)(line + 1) * CYC_LINE;
        // vblank event at start of line 224
        if (line == VISIBLE_LINES) {
            g_irq_vblank = 1; update_irq_level();
            // auto animation
            if (g_auto_anim_frame == 0) { g_auto_anim_frame = g_auto_anim_speed; g_auto_anim_counter++; }
            else g_auto_anim_frame--;
            // raster reload at vblank
            if ((g_irq2_ctrl & 0x40) && g_display_counter)
                g_irq2_cycle = cpu.cyc + (1146 / 2) + ((u64)g_display_counter + 1) * 2;
        }
        // run cpu to end of line, honoring raster irq scheduling
        for (long guard = 0;; guard++) {
            if (guard > 4000000) {
                fprintf(stderr, "LINEGUARD line=%d pc=%06x cyc=%llu irq2=%llu wd=%llu ctrl=%02x dc=%u\n",
                        line, cpu.pc, (unsigned long long)cpu.cyc,
                        (unsigned long long)g_irq2_cycle, (unsigned long long)g_watchdog_cycle,
                        g_irq2_ctrl, g_display_counter);
                exit(5);
            }
            u64 target = line_end;
            if (g_irq2_cycle && g_irq2_cycle < target) target = g_irq2_cycle;
            if (g_watchdog_cycle && g_watchdog_cycle < target) target = g_watchdog_cycle;
            cpu_run_until(target);
            if (g_irq2_cycle && cpu.cyc >= g_irq2_cycle) {
                g_irq2_cycle = 0;
                if (g_irq2_ctrl & 0x10) { g_irq_raster = 1; update_irq_level(); }
                if (g_irq2_ctrl & 0x80) g_irq2_cycle = cpu.cyc + ((u64)g_display_counter + 1) * 2;
                continue;
            }
            if (cpu.cyc >= g_watchdog_cycle) {
                // watchdog reset: full hardware reset, restart frame timing
                if (getenv("KOF98_EXLOG"))
                    fprintf(stderr, "WDRST pc=%06x cyc=%llu\n", cpu.pc, (unsigned long long)cpu.cyc);
                cpu_reset(1);
                g_frame_base = 0;
                g_frame_restart = 1;
                return;
            }
            break;
        }
        // profiling switches: KOF98_SKIP_AUDIO / KOF98_SKIP_VIDEO skip subsystems
        static int skip_audio = -1, skip_video = -1;
        if (skip_audio < 0) {
            skip_audio = getenv("KOF98_SKIP_AUDIO") ? 1 : 0;
            skip_video = getenv("KOF98_SKIP_VIDEO") ? 1 : 0;
        }
        if (!skip_audio) {
            z80_run_until(cpu.cyc / 3);     // audio cpu at 4MHz
            ym2610_run_until((cpu.cyc * 2) / 3);    // YM2610 at 8MHz
        }
        if (line < VISIBLE_LINES && !skip_video) video_line(line);
    }
    g_frame_base += CYC_FRAME;
    g_frame_count++;
    guards_check(g_frame_count);
}

const char *g_roms_dir = "roms";

void machine_init() {
    size_t sz;
    char path[1024];
    // Preferred: load straight from the MAME zip sets. Fall back to the
    // prebuilt rom/*.bin images when the zips are not present.
    RomSet rs;
    snprintf(path, sizeof(path), "%s/kof98.zip", g_roms_dir);
    char path2[1024];
    snprintf(path2, sizeof(path2), "%s/neogeo.zip", g_roms_dir);
    if (romset_load_zip(path, path2, &rs) == 0) {
        fprintf(stderr, "ROMs: loaded from %s + %s\n", path, path2);
        g_prom = rs.prom; g_bios = rs.bios; g_sfix = rs.sfix; g_s1 = rs.s1;
        g_zoomy = rs.zoomy; g_crom = rs.crom; g_sm1 = rs.sm1; g_vrom = rs.vrom;
        // M1: 线性放入 0x50000 缓冲区 (bank 窗口经掩码后线性读 0x10000..sz-1,
        // 与 MAME 一致); 顶端放一份低 64K 镜像仅作越界兜底.
        // 注意: 不能再往 +0x10000 复制整份 M1 —— 那会把 bank 区 (0x10000..sz-1)
        // 覆盖成 M1 开头, 导致音乐数据流全部读错 (BGM 死亡/奇怪音乐的根因)
        g_m1 = (u8 *)malloc(0x50000);
        memset(g_m1, 0, 0x50000);
        memcpy(g_m1, rs.m1, rs.m1_sz < 0x40000 ? rs.m1_sz : 0x40000);
        memcpy(g_m1 + 0x40000, rs.m1, rs.m1_sz < 0x10000 ? rs.m1_sz : 0x10000);
        g_m1_sz = (u32)rs.m1_sz;
        free(rs.m1);
    } else {
        fprintf(stderr, "ROMs: %s/*.zip not usable, falling back to rom/*.bin\n", g_roms_dir);
        g_prom = load_file("rom/prom.bin", &sz);
        g_bios = load_file("rom/bios.bin", &sz);
        g_sfix = load_file("rom/sfix.bin", &sz);
        g_s1 = load_file("rom/s1.bin", &sz);
        g_zoomy = load_file("rom/zoomy.bin", &sz);
        g_crom = load_file("rom/crom.bin", &sz);
        g_sm1 = load_file("rom/sm1.bin", &sz);
        g_vrom = load_file("rom/vrom.bin", &sz);
        // M1: 同上 —— 线性放入, 顶端镜像仅作兜底 (见上方注释)
        g_m1 = (u8 *)malloc(0x50000);
        {
            u8 *t = load_file("rom/m1.bin", &sz);
            memset(g_m1, 0, 0x50000);
            memcpy(g_m1, t, sz < 0x40000 ? sz : 0x40000);
            memcpy(g_m1 + 0x40000, t, sz < 0x10000 ? sz : 0x10000);
            g_m1_sz = (u32)sz;
            free(t);
        }
    }
    g_wram = alloc_guarded("wram", 0x10000);
    g_bram = alloc_guarded("bram", 0x10000);
    // battery-backed RAM: load previous contents if present (real MVS keeps BRAM
    // across power cycles). Fresh boards start all-zero; the BIOS formats it.
    {
        FILE *f = fopen("bram.bin", "rb");
        if (f) { if (fread(g_bram, 1, 0x10000, f) != 0x10000) memset(g_bram, 0, 0x10000); fclose(f); }
    }
    g_palram = (u16 *)alloc_guarded("palram", 0x2000 * 2);
    g_vram = (u16 *)alloc_guarded("vram", 0x8800 * 2);
    g_pens = (u32 *)alloc_guarded("pens", 4096 * 2 * 2 * 4);
    g_fb = (u32 *)alloc_guarded("fb", SCREEN_W * SCREEN_H * 4);
    g_cov = alloc_guarded("cov", 0x200000);
    g_prot_rom0 = (u16)((g_prom[0x100] << 8) | g_prom[0x101]);
    g_prot_rom1 = (u16)((g_prom[0x102] << 8) | g_prom[0x103]);
    if (getenv("KOF98_COV")) g_cov_enabled = 1;
    pal_init();
    for (int i = 0; i < 0x2000; i++) pal_write(i & 0x1FFF, 0);
    g_palette_bank = 0;
    video_init();
    if (getenv("KOF98_TRACE")) g_trace = fopen("trace.txt", "w");
    cpu_reset(1);
    rec_state_event();   // 录像: 记录启动快照 (KOF98_RECREC 开启时)
}

// ---------------------------------------------------------------------------
// save-state (RL interface). Layout: scalar header, then the big RAM buffers.
// ROMs and derived caches (fb/pens/spr8/cov) are intentionally excluded.
// ---------------------------------------------------------------------------

// full machine state (emu + z80 + ym), same K98S layout as kof98_api's
// kof98_state_save, so replay files and .k98s checkpoints stay compatible.
int emu_state_size(); void emu_state_save(u8 *buf); void emu_state_load(const u8 *buf);
int z80_state_size(); void z80_state_save(u8 *buf); void z80_state_load(const u8 *buf);
int ym_state_size(); void ym_state_save(u8 *buf); void ym_state_load(const u8 *buf);

int full_state_size() { return 16 + emu_state_size() + z80_state_size() + ym_state_size(); }

void full_state_save(u8 *buf) {
    u32 esz = (u32)emu_state_size(), zsz = (u32)z80_state_size(), ysz = (u32)ym_state_size();
    *(u32 *)(buf + 0) = 0x4B393853;      // 'K98S'
    *(u32 *)(buf + 4) = 1;               // version
    memcpy(buf + 8, &esz, 4);
    memcpy(buf + 12, &ysz, 4);
    u8 *p = buf + 16;
    emu_state_save(p); p += esz;
    z80_state_save(p); p += zsz;
    ym_state_save(p);
}

void full_state_load(const u8 *buf) {
    u32 esz, ysz;
    memcpy(&esz, buf + 8, 4);
    memcpy(&ysz, buf + 12, 4);
    const u8 *p = buf + 16;
    emu_state_load(p); p += esz;
    z80_state_load(p); p += z80_state_size();
    ym_state_load(p);
}

// ---------------------------------------------------------------------------
// 录像 (KOF98_RECREC=文件): 启动快照 + 每次读档后的完整快照 + 每帧输入.
// 用 exe 的 KOF98_REPLAY=文件 可确定性重放. 格式:
//   "K98R" ver(1) | 'S' u32len state | 'I' p1 p2 start coin select ...
// ---------------------------------------------------------------------------
static FILE *g_rec;
static int g_rec_init, g_rec_frames;
static const char *g_rec_path;   // exe 可用 --record 指定 (优先于环境变量)

void rec_set_path(const char *p) { g_rec_path = p; }

static void rec_open(void) {
    if (g_rec_init) return;
    g_rec_init = 1;
    const char *p = g_rec_path;
    if (!p || !p[0]) p = getenv("KOF98_RECREC");
    if (p && p[0]) {
        g_rec = fopen(p, "wb");
        if (g_rec) { fwrite("K98R", 1, 4, g_rec); fputc(1, g_rec); }
    }
}

// 记录当前完整机器状态 (启动后/读档后调用)
void rec_state_event(void) {
    rec_open();
    if (!g_rec) return;
    static u8 *buf;
    if (!buf) buf = (u8 *)malloc(full_state_size());
    full_state_save(buf);
    fputc('S', g_rec);
    int n = full_state_size();
    fwrite(&n, 4, 1, g_rec);
    fwrite(buf, 1, n, g_rec);
}

static void rec_frame_event(void) {    // machine_frame 开头调用
    rec_open();
    if (!g_rec) return;
    fputc('I', g_rec);
    u8 b[5] = { g_in_p1, g_in_p2, g_in_start, g_in_coin, g_in_select };
    fwrite(b, 1, 5, g_rec);
    if (++g_rec_frames % 60 == 0) fflush(g_rec);   // 每秒冲刷, 崩溃也最多丢1秒
}

extern u32 g_usp, g_ssp;

struct EmuStateHdr {
    Cpu cpu;
    u32 usp, ssp;
    s32 use_cart_vectors, use_cart_audio, save_ram_unlocked, palette_bank, screen_shadow;
    u32 bank_base;
    s32 irq_vblank, irq_raster, irq3, irq_level;
    u64 irq2_cycle, frame_base;
    s32 vpos, kof98_prot_state;
    u16 prot_rom0, prot_rom1;
    u8 in_p1, in_p2, in_start, in_coin, in_select;
    s32 in_service;
    u8 dsw;
    u16 vram_offset, vram_modulo, vram_readbuf;
    u8 auto_anim_speed, auto_anim_disabled, auto_anim_counter, auto_anim_frame;
    u32 display_counter;
    u8 irq2_ctrl;
    u64 watchdog_cycle;
    u8 sound_reply;
    s32 frame_count, frame_restart;
    s32 z80_nmi_enabled;
    u8 sound_cmd;
    u8 rtc_data_in, rtc_clk, rtc_stb, rtc_shift[7], rtc_out;
    s32 rtc_bitpos;
    u32 z80_bank_base[4];
};

int emu_state_size() {
    return (int)(sizeof(EmuStateHdr) + 0x10000 + 0x10000 + 0x800 + 0x4000 + 0x11000);
}

void emu_state_save(u8 *buf) {
    EmuStateHdr h;
    h.cpu = cpu; h.usp = g_usp; h.ssp = g_ssp;
    h.use_cart_vectors = g_use_cart_vectors; h.use_cart_audio = g_use_cart_audio;
    h.save_ram_unlocked = g_save_ram_unlocked; h.palette_bank = g_palette_bank;
    h.screen_shadow = g_screen_shadow;
    h.bank_base = g_bank_base;
    h.irq_vblank = g_irq_vblank; h.irq_raster = g_irq_raster; h.irq3 = g_irq3;
    h.irq_level = g_irq_level;
    h.irq2_cycle = g_irq2_cycle; h.frame_base = g_frame_base;
    h.vpos = g_vpos; h.kof98_prot_state = g_kof98_prot_state;
    h.prot_rom0 = g_prot_rom0; h.prot_rom1 = g_prot_rom1;
    h.in_p1 = g_in_p1; h.in_p2 = g_in_p2; h.in_start = g_in_start;
    h.in_coin = g_in_coin; h.in_select = g_in_select;
    h.in_service = g_in_service; h.dsw = g_dsw;
    h.vram_offset = g_vram_offset; h.vram_modulo = g_vram_modulo;
    h.vram_readbuf = g_vram_readbuf;
    h.auto_anim_speed = g_auto_anim_speed; h.auto_anim_disabled = g_auto_anim_disabled;
    h.auto_anim_counter = g_auto_anim_counter; h.auto_anim_frame = g_auto_anim_frame;
    h.display_counter = g_display_counter;
    h.irq2_ctrl = g_irq2_ctrl;
    h.watchdog_cycle = g_watchdog_cycle;
    h.sound_reply = g_sound_reply;
    h.frame_count = g_frame_count; h.frame_restart = g_frame_restart;
    h.z80_nmi_enabled = g_z80_nmi_enabled;
    h.sound_cmd = g_sound_cmd;
    h.rtc_data_in = rtc_data_in; h.rtc_clk = rtc_clk; h.rtc_stb = rtc_stb;
    memcpy(h.rtc_shift, rtc_shift, 7); h.rtc_out = rtc_out;
    h.rtc_bitpos = rtc_bitpos;
    memcpy(h.z80_bank_base, g_z80_bank_base, sizeof(h.z80_bank_base));
    memcpy(buf, &h, sizeof(h)); buf += sizeof(h);
    memcpy(buf, g_wram, 0x10000); buf += 0x10000;
    memcpy(buf, g_bram, 0x10000); buf += 0x10000;
    memcpy(buf, g_z80_ram, 0x800); buf += 0x800;
    memcpy(buf, g_palram, 0x4000); buf += 0x4000;
    memcpy(buf, g_vram, 0x11000); buf += 0x11000;
}

void emu_state_load(const u8 *buf) {
    EmuStateHdr h;
    memcpy(&h, buf, sizeof(h)); buf += sizeof(h);
    cpu = h.cpu; g_usp = h.usp; g_ssp = h.ssp;
    g_use_cart_vectors = h.use_cart_vectors; g_use_cart_audio = h.use_cart_audio;
    g_save_ram_unlocked = h.save_ram_unlocked; g_palette_bank = h.palette_bank;
    g_screen_shadow = h.screen_shadow;
    g_bank_base = h.bank_base;
    g_irq_vblank = h.irq_vblank; g_irq_raster = h.irq_raster; g_irq3 = h.irq3;
    g_irq_level = h.irq_level;
    g_irq2_cycle = h.irq2_cycle; g_frame_base = h.frame_base;
    g_vpos = h.vpos; g_kof98_prot_state = h.kof98_prot_state;
    g_prot_rom0 = h.prot_rom0; g_prot_rom1 = h.prot_rom1;
    g_in_p1 = h.in_p1; g_in_p2 = h.in_p2; g_in_start = h.in_start;
    g_in_coin = h.in_coin; g_in_select = h.in_select;
    g_in_service = h.in_service; g_dsw = h.dsw;
    g_vram_offset = h.vram_offset; g_vram_modulo = h.vram_modulo;
    g_vram_readbuf = h.vram_readbuf;
    g_auto_anim_speed = h.auto_anim_speed; g_auto_anim_disabled = h.auto_anim_disabled;
    g_auto_anim_counter = h.auto_anim_counter; g_auto_anim_frame = h.auto_anim_frame;
    g_display_counter = h.display_counter;
    g_irq2_ctrl = h.irq2_ctrl;
    g_watchdog_cycle = h.watchdog_cycle;
    g_sound_reply = h.sound_reply;
    g_frame_count = h.frame_count; g_frame_restart = h.frame_restart;
    g_z80_nmi_enabled = h.z80_nmi_enabled;
    g_sound_cmd = h.sound_cmd;
    rtc_data_in = h.rtc_data_in; rtc_clk = h.rtc_clk; rtc_stb = h.rtc_stb;
    memcpy(rtc_shift, h.rtc_shift, 7); rtc_out = h.rtc_out;
    rtc_bitpos = h.rtc_bitpos;
    memcpy(g_z80_bank_base, h.z80_bank_base, sizeof(g_z80_bank_base));
    memcpy(g_wram, buf, 0x10000); buf += 0x10000;
    memcpy(g_bram, buf, 0x10000); buf += 0x10000;
    memcpy(g_z80_ram, buf, 0x800); buf += 0x800;
    memcpy(g_palram, buf, 0x4000); buf += 0x4000;
    memcpy(g_vram, buf, 0x11000); buf += 0x11000;
    pal_rebuild();  // pens 是 palram 的派生缓存, 不存入快照, 载入后必须重建
}
