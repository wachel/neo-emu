// KOF98 native emulator -- pure C interface for RL training.
// Cross-platform: builds as kof98.dll (Windows), libkof98.so (Linux),
// libkof98.dylib (macOS). No window/audio/timer dependencies: the caller
// drives frame stepping.
#ifndef KOF98_API_H
#define KOF98_API_H

#ifdef _WIN32
#define KOF98_EXPORT extern "C" __declspec(dllexport)
#else
#define KOF98_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#include <stdint.h>

// boot flags for kof98_boot
#define KOF98F_VIDEO 1   // render the 320x224 framebuffer each frame (for vision RL)
#define KOF98F_AUDIO 2   // full YM2610 waveform synthesis (default: timers only)
#define KOF98F_ZINT  4   // z80 timer IRQ enabled (default: masked; game-logic identical)

// button bits for kof98_set_input (1 = pressed)
#define KOF98_UP     0x01
#define KOF98_DOWN   0x02
#define KOF98_LEFT   0x04
#define KOF98_RIGHT  0x08
#define KOF98_A      0x10
#define KOF98_B      0x20
#define KOF98_C      0x40
#define KOF98_D      0x80

// Load ROMs from roms_dir (needs kof98.zip + neogeo.zip) and power on.
// flags: default 0 = fastest RL config (no video render, lite audio, z80 zint off).
// Returns 0 on success, nonzero on ROM load failure.
KOF98_EXPORT int kof98_boot(const char *roms_dir, unsigned flags);

// Run exactly one frame (about 1/60s of emulated time).
KOF98_EXPORT void kof98_step_frame(void);

// Set controller state. p1/p2: KOF98_* button bits (1 = pressed).
// start/coin: 1 = pressed.
KOF98_EXPORT void kof98_set_input(uint8_t p1, uint8_t p2, uint8_t start, uint8_t coin);

// Byte/word/dword read from the 68k address space (observations: WRAM at
// 0x100000..0x10FFFF, I/O at 0x300000.., ROM at 0x0..).
KOF98_EXPORT uint8_t  kof98_peek8(uint32_t addr);
KOF98_EXPORT uint16_t kof98_peek16(uint32_t addr);
KOF98_EXPORT uint32_t kof98_peek32(uint32_t addr);

// Framebuffer: 320x224, u32 pixels as 0x00RRGGBB (little-endian bytes B,G,R,0
// = "BGRA" for SDL/pygame). Only valid with KOF98F_VIDEO.
KOF98_EXPORT const uint32_t *kof98_framebuffer(int *width, int *height);

// Audio output: 44100 Hz stereo s16 interleaved. Samples are only produced
// with KOF98F_AUDIO; otherwise the buffer stays empty.
KOF98_EXPORT uint32_t kof98_audio_rate(void);
// Drain up to max_frames stereo sample pairs into dst (dst must hold
// max_frames*2 int16). Returns the number of sample pairs actually written.
KOF98_EXPORT uint32_t kof98_audio_drain(int16_t *dst, uint32_t max_frames);

// Save-state: size of one snapshot in bytes (constant after boot).
KOF98_EXPORT int kof98_state_size(void);
// Snapshot all mutable machine state into buf (must be >= kof98_state_size()).
KOF98_EXPORT void kof98_state_save(void *buf);
// Restore a snapshot taken with kof98_state_save.
KOF98_EXPORT void kof98_state_load(const void *buf);

#endif
