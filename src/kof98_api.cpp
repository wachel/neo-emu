// Pure C RL interface. Platform-free: no window/audio/timer.
#include "kof98_api.h"
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void machine_init();
void machine_frame();
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
    g_in_start = start ? 1 : 0;
    g_in_coin = coin ? (u8)0xFE : (u8)0xFF;   // coin1 = bit0 low
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
}
