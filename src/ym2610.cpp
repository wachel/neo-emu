// YM2610 sound chip glue, built on the ymfm library (BSD-3-Clause,
// copyright Aaron Giles -- see src/ymfm/LICENSE.txt).
// Chip runs at 8MHz (= 68k * 2/3 cycles); engine output at clock/144 (55.5kHz),
// resampled to 44.1kHz stereo for the platform audio ring.
#include "rt.h"
#include "ymfm/ymfm_opn.h"
#include <vector>

extern u8 *g_vrom;
void z80_set_irq(int on);

// ---- audio ring buffer (stereo s16, producer = emu, consumer = win32 audio) ----
#define RING_BITS 17
#define RING_SIZE (1 << RING_BITS)
#define RING_MASK (RING_SIZE - 1)
static s16 g_ring[RING_SIZE * 2];
static volatile u32 g_ring_w, g_ring_r;
u64 g_audio_nonzero, g_audio_total, g_audio_clip;
void audio_ring_push(s16 l, s16 r) {
    u32 w = g_ring_w;
    g_audio_total++;
    if (l || r) g_audio_nonzero++;
    if (w - g_ring_r >= RING_SIZE) return;      // overrun: drop
    g_ring[(w & RING_MASK) * 2] = l;
    g_ring[(w & RING_MASK) * 2 + 1] = r;
    g_ring_w = w + 1;
}
u32 audio_ring_avail() { return g_ring_w - g_ring_r; }
u32 audio_ring_pop(s16 *dst, u32 n) {
    u32 a = audio_ring_avail();
    if (n > a) n = a;
    for (u32 i = 0; i < n; i++) {
        u32 idx = (g_ring_r & RING_MASK) * 2;
        dst[i * 2] = g_ring[idx]; dst[i * 2 + 1] = g_ring[idx + 1];
        g_ring_r++;
    }
    return n;
}
void audio_ring_clear() { g_ring_r = g_ring_w = 0; }

// ---- ymfm interface glue ----
class YmGlue : public ymfm::ymfm_interface {
public:
    void ymfm_set_timer(u32 tnum, s32 duration) override {
        if (tnum > 1) return;
        if (duration < 0) timer_exp[tnum] = ~(u64)0;
        else timer_exp[tnum] = cur_cyc + (u64)duration;
    }
    void ymfm_set_busy_end(u32 clocks) override { busy_end = cur_cyc + clocks; }
    bool ymfm_is_busy() override { return cur_cyc < busy_end; }
    void ymfm_update_irq(bool asserted) override {
    // KOF98_NO_ZINT: keep the z80 timer IRQ masked (sound engine never ticks,
    // z80 idles in its wait loop). Command/NMI replies still work.
    static int no_zint = -1;
    if (no_zint < 0) no_zint = getenv("KOF98_NO_ZINT") ? 1 : 0;
    if (!no_zint) z80_set_irq(asserted ? 1 : 0);
}
    u8 ymfm_external_read(ymfm::access_class type, u32 address) override {
        (void)type;
        return g_vrom[address & 0xFFFFFF];
    }
    void fire_timer(int t) { m_engine->engine_timer_expired((u32)t); }
    u64 cur_cyc = 0;
    u64 timer_exp[2] = { ~(u64)0, ~(u64)0 };
    u64 busy_end = 0;
};

static YmGlue g_intf;
static ymfm::ym2610 *g_chip;

u8 ym2610_read(u16 port) { return g_chip->read(port & 3); }
void ym2610_write(u16 port, u8 data) {
    static u8 log_addr[2];
    {
        int half = (port >> 1) & 1;
        if (!(port & 1)) log_addr[half] = data;
        else if (getenv("KOF98_YMLOG"))
            fprintf(stderr, "YM p%d r%02x=%02x t=%.3f\n", half, log_addr[half], data,
                    (double)g_intf.cur_cyc / 8000000.0);
        // trace trigger: first write of addr 0xA6 on port 0 after 4s
        static int traced = 0;
        if (!traced && half == 0 && !(port & 1) && data == 0xA6 &&
            g_intf.cur_cyc > 4ULL * 8000000 && getenv("KOF98_ZTRACE")) {
            traced = 1;
            void z80_trace_dump(FILE *f);
            z80_trace_dump(stderr);
        }
    }
    if (getenv("KOF98_FORCEPAN") && (port & 1)) {
        int half = (port >> 1) & 1;
        if (log_addr[half] >= 0xB4 && log_addr[half] <= 0xB6)
            data |= 0xC0;    // experiment: never let the driver mute L/R
    }
    g_chip->write(port & 3, data);
}

void ym2610_reset() {
    if (!g_chip) {
        g_chip = new ymfm::ym2610(g_intf);
        g_chip->set_fidelity(ymfm::OPN_FIDELITY_MED);   // engine output = clock/144
    }
    g_chip->reset();
    g_intf.cur_cyc = 0;
    g_intf.timer_exp[0] = g_intf.timer_exp[1] = ~(u64)0;
    g_intf.busy_end = 0;
    audio_ring_clear();
}

// ---- save-state ----
struct YmStateHdr {
    u64 cur_cyc, timer_exp[2], busy_end;
    u32 chip_size;
};

int ym_state_size() {
    static int sz = -1;
    if (sz < 0) {
        std::vector<uint8_t> tmp;
        ymfm::ymfm_saved_state ss(tmp, true);
        g_chip->save_restore(ss);
        sz = (int)(sizeof(YmStateHdr) + tmp.size());
    }
    return sz;
}

void ym_state_save(u8 *buf) {
    YmStateHdr h;
    h.cur_cyc = g_intf.cur_cyc;
    h.timer_exp[0] = g_intf.timer_exp[0]; h.timer_exp[1] = g_intf.timer_exp[1];
    h.busy_end = g_intf.busy_end;
    std::vector<uint8_t> tmp;
    ymfm::ymfm_saved_state ss(tmp, true);
    g_chip->save_restore(ss);
    h.chip_size = (u32)tmp.size();
    memcpy(buf, &h, sizeof(h)); buf += sizeof(h);
    memcpy(buf, tmp.data(), tmp.size());
}

void ym_state_load(const u8 *buf) {
    YmStateHdr h;
    memcpy(&h, buf, sizeof(h)); buf += sizeof(h);
    g_intf.cur_cyc = h.cur_cyc;
    g_intf.timer_exp[0] = h.timer_exp[0]; g_intf.timer_exp[1] = h.timer_exp[1];
    g_intf.busy_end = h.busy_end;
    std::vector<uint8_t> tmp(h.chip_size);
    memcpy(tmp.data(), buf, h.chip_size);
    ymfm::ymfm_saved_state ss(tmp, false);
    g_chip->save_restore(ss);
}

// ---- clocking: 144 master cycles per engine output sample (55.5kHz) ----
void ym2610_run_until(u64 target) {
    // KOF98_LITE_AUDIO: advance timers/busy only, skip waveform synthesis.
    // Z80 keeps running (sound replies intact) but FM/ADPCM mixing cost is gone.
    // Note: ADPCM playback state won't complete under this mode.
    static int lite = -1;
    if (lite < 0) lite = getenv("KOF98_LITE_AUDIO") ? 1 : 0;
    if (lite) {
        // event-driven: jump straight to the next timer expiry instead of
        // stepping every clock (8MHz busy loop otherwise)
        while (g_intf.cur_cyc < target) {
            u64 next = target;
            for (int t = 0; t < 2; t++)
                if (g_intf.timer_exp[t] < next) next = g_intf.timer_exp[t];
            g_intf.cur_cyc = next;
            for (int t = 0; t < 2; t++)
                if (g_intf.cur_cyc >= g_intf.timer_exp[t]) {
                    g_intf.timer_exp[t] = ~(u64)0;
                    g_intf.fire_timer(t);
                }
        }
        return;
    }
    static u32 gen_div;
    static double in_index, out_pos;    // resampler positions (input-sample units)
    static int prev_l, prev_r;
    static FILE *wav = NULL;
    static int wav_init = 0;
    const double in_rate = 8000000.0 / 144.0;       // 55.5kHz engine output
    const double step = in_rate / 44100.0;          // input units per output sample
    if (!wav_init) {
        wav_init = 1;
        const char *wp = getenv("KOF98_WAVDUMP");
        if (wp) wav = fopen(wp, "wb");
    }
    while (g_intf.cur_cyc < target) {
        g_intf.cur_cyc++;
        for (int t = 0; t < 2; t++)
            if (g_intf.cur_cyc >= g_intf.timer_exp[t]) {
                g_intf.timer_exp[t] = ~(u64)0;
                g_intf.fire_timer(t);
            }
        if (++gen_div < 144) continue;
        gen_div = 0;
        ymfm::ym2610::output_data out;
        g_chip->generate(&out);
        // out.data[0]=FM+ADPCM L, [1]=R, [2]=SSG mono (mix to both)
        int ol = out.data[0] + out.data[2] / 2;
        int or_ = out.data[1] + out.data[2] / 2;
        // linear downsample 55.5k -> 44.1k: emit when output time falls in [prev, cur]
        in_index += 1.0;
        if (out_pos <= in_index) {
            double f = out_pos - (in_index - 1.0);  // 0..1 position between prev and cur
            if (f < 0.0) f = 0.0;
            int l = prev_l + (int)((ol - prev_l) * f);
            int r = prev_r + (int)((or_ - prev_r) * f);
            if (l > 32767) { l = 32767; g_audio_clip++; }
            if (l < -32768) { l = -32768; g_audio_clip++; }
            if (r > 32767) { r = 32767; g_audio_clip++; }
            if (r < -32768) { r = -32768; g_audio_clip++; }
            audio_ring_push((s16)l, (s16)r);
            if (wav) { s16 b[2] = { (s16)l, (s16)r }; fwrite(b, 2, 2, wav); }
            out_pos += step;
        }
        prev_l = ol;
        prev_r = or_;
    }
}

// ---- standalone FM self-test (KOF98_FMTEST): key on ch0, dump engine output ----
void ym2610_selftest(const char *path) {
    YmGlue intf;
    ymfm::ym2610 chip(intf);
    chip.set_fidelity(ymfm::OPN_FIDELITY_MED);
    chip.reset();
    u8 g_vrom_dummy[16] = {0};
    if (!g_vrom) g_vrom = g_vrom_dummy;
    // simple loud instrument on FM ch1 (port 0; YM2610 uses opna ch 1,2,4,5)
    static const u8 init[][2] = {
        {0x31, 0x01}, {0x35, 0x01}, {0x39, 0x01}, {0x3D, 0x01},   // MUL=1
        {0x41, 0x00}, {0x45, 0x00}, {0x49, 0x00}, {0x4D, 0x00},   // TL=0
        {0x51, 0x1F}, {0x55, 0x1F}, {0x59, 0x1F}, {0x5D, 0x1F},   // AR=31
        {0x61, 0x00}, {0x65, 0x00}, {0x69, 0x00}, {0x6D, 0x00},
        {0x71, 0x00}, {0x75, 0x00}, {0x79, 0x00}, {0x7D, 0x00},
        {0x81, 0x00}, {0x85, 0x00}, {0x89, 0x00}, {0x8D, 0x00},
        {0xB1, 0x07},   // algo 7
        {0xB5, 0xC0},   // L+R on
        {0xA5, 0x22}, {0xA1, 0x00},   // block 4, fnum 0x400
        {0x28, 0xF1},   // key on ch1, all ops
    };
    for (unsigned i = 0; i < sizeof(init) / sizeof(init[0]); i++) {
        chip.write(0, init[i][0]);
        chip.write(1, init[i][1]);
    }
    FILE *f = fopen(path, "wb");
    int peak = 0, nz = 0;
    for (int i = 0; i < 55556; i++) {
        ymfm::ym2610::output_data out;
        chip.generate(&out);
        int v = out.data[0];
        if (v) nz++;
        if (v > peak) peak = v;
        if (-v > peak) peak = -v;
        if (f) { s16 b[2] = { (s16)v, (s16)v }; fwrite(b, 2, 2, f); }
    }
    if (f) fclose(f);
    printf("FMTEST ch1: nonzero=%d/55556 peak=%d\n", nz, peak);

    // replay the driver's exact select-music keyon sequence on ch5 (port 1)
    chip.reset();
    static const u8 seq[][2] = {
        {0xB3, 0x11}, {0x4E, 0x26}, {0xA6, 0xB8}, {0xA2, 0x8B}
    };
    for (unsigned i = 0; i < 4; i++) { chip.write(2, seq[i][0]); chip.write(3, seq[i][1]); }
    chip.write(0, 0x28); chip.write(1, 0xF6);
    peak = 0; nz = 0;
    for (int i = 0; i < 55556; i++) {
        ymfm::ym2610::output_data out;
        chip.generate(&out);
        int v = out.data[0];
        if (v) nz++;
        if (v > peak) peak = v;
        if (-v > peak) peak = -v;
    }
    printf("FMTEST driver-seq ch5 (reset state): nonzero=%d peak=%d\n", nz, peak);

    // same but with AR=31 + MUL=1 + algo 7 on ch5
    chip.reset();
    for (int op = 0; op < 16; op += 4) {
        chip.write(2, 0x32 + op); chip.write(3, 0x01);   // MUL=1
        chip.write(2, 0x52 + op); chip.write(3, 0x1F);   // AR=31
        chip.write(2, 0x82 + op); chip.write(3, 0x00);   // SL/RR
        chip.write(2, 0x42 + op); chip.write(3, 0x00);   // TL=0
    }
    chip.write(2, 0xB2); chip.write(3, 0x07);            // algo 7
    chip.write(2, 0xB6); chip.write(3, 0xC0);            // pan L+R
    for (unsigned i = 0; i < 4; i++) { chip.write(2, seq[i][0]); chip.write(3, seq[i][1]); }
    chip.write(0, 0x28); chip.write(1, 0xF6);
    peak = 0; nz = 0;
    for (int i = 0; i < 55556; i++) {
        ymfm::ym2610::output_data out;
        chip.generate(&out);
        int v = out.data[0];
        if (v) nz++;
        if (v > peak) peak = v;
        if (-v > peak) peak = -v;
    }
    printf("FMTEST ch5 full-instrument: nonzero=%d peak=%d\n", nz, peak);
}
