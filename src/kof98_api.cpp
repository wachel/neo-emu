// Pure C RL interface. Platform-free: no window/audio/timer.
#include "kof98_api.h"
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void machine_init();
void machine_frame();
void cpu_interp_step();
int emu_state_size();
void emu_state_save(u8 *buf);
void emu_state_load(const u8 *buf);
int z80_state_size();
void z80_state_save(u8 *buf);
void z80_state_load(const u8 *buf);
int ym_state_size();
void ym_state_save(u8 *buf);
void ym_state_load(const u8 *buf);

extern const char *g_roms_dir;
extern u32 *g_fb;
u32 audio_ring_pop(s16 *dst, u32 n);    // ym2610.cpp

static void set_env(const char *kv) {
#ifdef _WIN32
    _putenv(_strdup(kv));
#else
    const char *eq = strchr(kv, '=');
    if (!eq) return;
    char name[64];
    size_t n = (size_t)(eq - kv);
    if (n >= sizeof(name)) return;
    memcpy(name, kv, n); name[n] = 0;
    setenv(name, eq + 1, 1);
#endif
}

int kof98_boot(const char *roms_dir, unsigned flags) {
    static int booted;
    if (booted) return 0;
    booted = 1;
    if (!(flags & KOF98F_VIDEO)) set_env("KOF98_SKIP_VIDEO=1");
    if (!(flags & KOF98F_AUDIO)) set_env("KOF98_LITE_AUDIO=1");
    if (!(flags & KOF98F_ZINT)) set_env("KOF98_NO_ZINT=1");
    if (roms_dir && roms_dir[0]) g_roms_dir = roms_dir;
    machine_init();
    return g_prom ? 0 : 1;
}

void kof98_step_frame(void) {
    machine_frame();
}

void kof98_set_input(uint8_t p1, uint8_t p2, uint8_t start, uint8_t coin) {
    // API takes 1=pressed bits; hardware globals are active-low
    g_in_p1 = (u8)~p1;
    g_in_p2 = (u8)~p2;
    g_in_start = start & 0x05;            // bit0 P1 start, bit2 P2 start
    g_in_coin = (u8)~(coin & 0x1F);       // bit0 c1, bit1 c2, bit2 service1 (active low)
}

uint8_t  kof98_peek8(uint32_t addr)  { return mem_read8(addr); }
uint16_t kof98_peek16(uint32_t addr) { return mem_read16(addr); }
uint32_t kof98_peek32(uint32_t addr) { return mem_read32(addr); }

const uint32_t *kof98_framebuffer(int *width, int *height) {
    if (width) *width = 320;
    if (height) *height = 224;
    return g_fb;
}

uint32_t kof98_audio_rate(void) { return 44100; }

uint32_t kof98_audio_drain(int16_t *dst, uint32_t max_frames) {
    return audio_ring_pop((s16 *)dst, max_frames);
}

int kof98_state_size(void) {
    return 16 + emu_state_size() + z80_state_size() + ym_state_size();
}

void kof98_state_save(void *buf_) {
    u8 *buf = (u8 *)buf_;
    u32 magic = 0x4B393853;   // 'K98S'
    u32 ver = 1;
    u32 esz = (u32)emu_state_size(), zsz = (u32)z80_state_size(), ysz = (u32)ym_state_size();
    memcpy(buf, &magic, 4); buf += 4;
    memcpy(buf, &ver, 4); buf += 4;
    memcpy(buf, &esz, 4); buf += 4;
    memcpy(buf, &ysz, 4); buf += 4;
    emu_state_save(buf); buf += esz;
    z80_state_save(buf); buf += zsz;
    ym_state_save(buf);
}

void ym2610_audio_resync(void);
void rec_state_event(void);

void kof98_state_load(const void *buf_) {
    const u8 *buf = (const u8 *)buf_;
    u32 magic, esz, zsz, ysz;
    memcpy(&magic, buf, 4); buf += 4;
    if (magic != 0x4B393853) return;
    buf += 4;   // version
    memcpy(&esz, buf, 4); buf += 4;
    memcpy(&ysz, buf, 4); buf += 4;
    zsz = (u32)z80_state_size();
    emu_state_load(buf); buf += esz;
    z80_state_load(buf); buf += zsz;
    ym_state_load(buf);
    // 丢弃加载前时间线上残留的未播音频, 否则会在新场景开头听到一小段旧音效
    ym2610_audio_resync();
    rec_state_event();   // 录像: 记录读档后的完整状态 (KOF98_RECREC 开启时)
}

// ---- 68k PC 周期剖析 (诊断用) ----
extern u32 *g_prof, *g_prof_n;
extern u64 g_prof_instr;
extern int g_prof_on;

void kof98_prof_start() {
    if (!g_prof) { g_prof = (u32 *)calloc(1 << 20, sizeof(u32)); g_prof_n = (u32 *)calloc(1 << 20, sizeof(u32)); }
    else { memset(g_prof, 0, (1 << 20) * sizeof(u32)); memset(g_prof_n, 0, (1 << 20) * sizeof(u32)); }
    g_prof_instr = 0;
    g_prof_on = 1;
}

// 单条指令周期微基准 (诊断): 把 op 写到 WRAM 0x100100, 单步执行, 返回周期增量
u64 kof98_dbg_step(uint16_t op, uint32_t d0, uint32_t d1) {
    mem_write16(0x100100, op);
    mem_write16(0x100102, 0x4E75);   // RTS (占位, 防扩展误读)
    mem_write16(0x100104, 0x4E75);
    cpu.D[0] = d0;
    cpu.D[1] = d1;
    cpu.pc = 0x100100;
    u64 c0 = cpu.cyc;
    cpu_interp_step();
    return cpu.cyc - c0;
}

// dump: 16 字节桶, 按周期排序写出前 200 个 (含指令数与平均周期) + 总计
int kof98_prof_dump(const char *path) {
    if (!g_prof) return -1;
    FILE *f = fopen(path, "w");
    if (!f) return -2;
    u64 total = 0, total_n = 0;
    static u32 cyc[1 << 20], nin[1 << 20];
    memcpy(cyc, g_prof, sizeof(cyc));
    memcpy(nin, g_prof_n, sizeof(nin));
    for (int i = 0; i < (1 << 20); i++) { total += cyc[i]; total_n += nin[i]; }
    fprintf(f, "total_cycles=%llu total_instr=%llu avg=%.2f\n",
            (unsigned long long)total, (unsigned long long)total_n,
            total_n ? (double)total / total_n : 0.0);
    for (int n = 0; n < 200; n++) {
        int best = -1;
        for (int i = 0; i < (1 << 20); i++)
            if (cyc[i] && (best < 0 || cyc[i] > cyc[best])) best = i;
        if (best < 0 || !cyc[best]) break;
        fprintf(f, "pc=%06x cycles=%u n=%u cpi=%.1f\n", best << 4, cyc[best], nin[best],
                nin[best] ? (double)cyc[best] / nin[best] : 0.0);
        cyc[best] = 0;
    }
    fclose(f);
    return 0;
}
