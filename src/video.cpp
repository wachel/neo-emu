// LSPC video: sprite + fix layer rendering, faithfully following MAME neogeo_spr.cpp
#include "rt.h"

extern u8 *g_crom, *g_sfix, *g_s1, *g_zoomy;
extern u32 *g_pens;
extern u16 g_vram_offset;
extern u8 g_auto_anim_counter, g_auto_anim_disabled;

static u8 *g_spr8;          // pre-decoded 8bpp sprite graphics (2x crom size)
static u32 g_spr_mask;      // address mask in decoded space

static const u16 zoom_x_tables[16] = {
    0x0080, 0x0880, 0x0888, 0x2888, 0x288a, 0x2a8a, 0x2aaa, 0xaaaa,
    0xaaea, 0xbaea, 0xbaeb, 0xbbeb, 0xbbef, 0xfbef, 0xfbff, 0xffff
};

void video_init() {
    // decode sprite gfx exactly like MAME neosprite_optimized_device::optimize_helper:
    // each 0x80-byte block (one 16x16 4bpp tile in the c1/c2,c3/c4... interleaved
    // region) -> 0x100 bytes 8bpp; pixel x reads BIT x of the byte (LSB = leftmost).
    size_t crom_size = 0x4000000;   // 64 MiB for kof98
    size_t dec_size = crom_size * 2;
    g_spr8 = alloc_guarded("spr8", dec_size);
    const u8 *src = g_crom;
    u8 *dst = g_spr8;
    for (size_t i = 0; i < crom_size; i += 0x80, src += 0x80) {
        for (int y = 0; y < 0x10; y++) {
            for (int x = 0; x < 8; x++) {
                *dst++ = (u8)((((src[0x43 | (y << 2)] >> x) & 1) << 3) |
                              (((src[0x41 | (y << 2)] >> x) & 1) << 2) |
                              (((src[0x42 | (y << 2)] >> x) & 1) << 1) |
                              (((src[0x40 | (y << 2)] >> x) & 1) << 0));
            }
            for (int x = 0; x < 8; x++) {
                *dst++ = (u8)((((src[0x03 | (y << 2)] >> x) & 1) << 3) |
                              (((src[0x01 | (y << 2)] >> x) & 1) << 2) |
                              (((src[0x02 | (y << 2)] >> x) & 1) << 1) |
                              (((src[0x00 | (y << 2)] >> x) & 1) << 0));
            }
        }
    }
    g_spr_mask = (u32)(dec_size - 1);
}

static inline int sprite_on_scanline(int scanline, int y, int rows) {
    return (rows == 0) || (rows >= 0x20) || (((scanline - y) & 0x1FF) < (rows * 0x10));
}

static void draw_sprites_line(int line, const u32 *pens) {
    const u16 *vram = g_vram;
    u16 *list = &g_vram[(line & 1) ? 0x8680 : 0x8600];
    // parse: build active sprite list (max 96 per line)
    int count = 0;
    {
        int y = 0, rows = 0;
        for (int n = 0; n < 381; n++) {
            u16 yc = vram[0x8200 | n];
            if (!(yc & 0x40)) { y = 0x200 - (yc >> 7); rows = yc & 0x3F; }
            if (rows == 0) continue;
            if (!sprite_on_scanline(line, y, rows)) continue;
            list[count++] = (u16)n;
            if (count == 96) break;
        }
    }
    list[count] = 0;

    int x = 0, y = 0, rows = 0, zoom_x = 0, zoom_y = 0;
    u32 *fb = g_fb + line * SCREEN_W;
    for (int i = 0; i < count; i++) {
        int n = list[i] & 0x1FF;
        u16 yc = vram[0x8200 | n];
        u16 zc = vram[0x8000 | n];
        if (yc & 0x40) {    // chained
            x = (x + zoom_x + 1) & 0x1FF;
            zoom_x = (zc >> 8) & 0x0F;
        } else {
            y = 0x200 - (yc >> 7);
            x = vram[0x8400 | n] >> 7;
            zoom_y = zc & 0xFF;
            zoom_x = (zc >> 8) & 0x0F;
            rows = yc & 0x3F;
        }
        if (x >= 0x140 && x <= 0x1F0) continue;
        if (!sprite_on_scanline(line, y, rows)) continue;

        int sprite_line = (line - y) & 0x1FF;
        int zoom_line = sprite_line & 0xFF;
        int invert = (sprite_line >> 8) & 1;
        if (invert) zoom_line ^= 0xFF;
        if (rows > 0x20) {
            zoom_line %= ((zoom_y + 1) << 1);
            if (zoom_line > zoom_y) {
                zoom_line = ((zoom_y + 1) << 1) - 1 - zoom_line;
                invert = !invert;
            }
        }
        u8 b = g_zoomy[(zoom_y << 8) | zoom_line];
        int sy = b & 0x0F, tile = b >> 4;
        if (invert) { sy ^= 0x0F; tile ^= 0x1F; }

        u32 attr_offs = ((u32)n << 6) | ((u32)tile << 1);
        u16 attr = vram[attr_offs + 1];
        u32 code = (((u32)attr << 12) & 0xF0000) | vram[attr_offs];
        if (!g_auto_anim_disabled) {
            if (attr & 0x08) code = (code & ~7u) | (g_auto_anim_counter & 7);
            else if (attr & 0x04) code = (code & ~3u) | (g_auto_anim_counter & 3);
        }
        if (attr & 0x02) sy ^= 0x0F;                    // vflip
        u16 zxt = zoom_x_tables[zoom_x];
        u32 gfx = ((code << 8) | ((u32)sy << 4)) & g_spr_mask;
        const u32 *line_pens = pens + ((attr >> 8) << 4);
        int x_inc;
        if (attr & 0x01) { gfx += 0x0F; x_inc = -1; } else x_inc = 1;  // hflip

        if (x <= 0x1F0) {
            u32 *px = fb + x;
            u32 *px_end = fb + SCREEN_W;    // clip: MAME draws into a 384-wide bitmap
            for (int i2 = 0; i2 < 0x10; i2++) {
                if (zxt & 0x8000) {
                    u8 p = g_spr8[gfx];
                    if (p && px < px_end) *px = line_pens[p];
                    px++;
                }
                zxt <<= 1;
                if (!zxt) break;
                gfx += x_inc;
            }
        } else {    // wrap-around
            int xs = x;
            u32 *px = fb;
            for (int i2 = 0; i2 < 0x10; i2++) {
                if (zxt & 0x8000) {
                    if (x >= 0x200) {
                        u8 p = g_spr8[gfx];
                        if (p) *px = line_pens[p];
                        px++;
                    }
                    x++;
                }
                zxt <<= 1;
                if (!zxt) break;
                gfx += x_inc;
            }
            x = xs;
        }
    }
}

static void draw_fixed_line(int line, const u32 *pens) {
    const u16 *vram = g_vram;
    const u8 *gfx_base = g_use_cart_audio ? g_s1 : g_sfix;
    const u32 addr_mask = 0x1FFFF;
    const u16 *video_data = &vram[0x7000 | (line >> 3)];
    u32 *px = g_fb + line * SCREEN_W;
    static const int pix_offsets[4] = {0x10, 0x18, 0x00, 0x08};
    for (int col = 0; col < 40; col++) {
        u16 entry = *video_data;
        u32 code = entry & 0x0FFF;
        const u32 *char_pens = pens + ((entry >> 12) << 4);
        int go = (int)(((code << 5) | (line & 7)) & addr_mask);
        for (int i = 0; i < 4; i++) {
            u8 data = gfx_base[go + pix_offsets[i]];
            if (data & 0x0F) *px = char_pens[data & 0x0F];
            px++;
            if (data & 0xF0) *px = char_pens[data >> 4];
            px++;
        }
        video_data += 0x20;
    }
}

void video_line(int line) {
    g_ntrace = 3;
    const u32 *pens = g_pens + g_palette_bank + (g_screen_shadow ? 0x2000 : 0);
    u32 bg = pens[0xFFF];
    u32 *fb = g_fb + line * SCREEN_W;
    for (int x = 0; x < SCREEN_W; x++) fb[x] = bg;
    g_ntrace = 4;
    if (!getenv("KOF98_NOSPR")) draw_sprites_line(line, pens);
    g_ntrace = 5;
    if (!getenv("KOF98_NOFIX")) draw_fixed_line(line, pens);
    g_ntrace = 0;
}
