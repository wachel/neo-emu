// Z80 audio CPU, built on the chips/z80.h cycle-stepped core
// (ISC license, (c) 2021 Andre Weissflog -- see src/chips/z80.h header).
#include "rt.h"
#include <stdio.h>
#define CHIPS_IMPL
#include "chips/z80.h"

// memory/io provided by emu.cpp
u8 z80_mem_read(u16 addr);
void z80_mem_write(u16 addr, u8 data);
u8 z80_io_read(u16 port);
void z80_io_write(u16 port, u8 data);

u64 g_z80_cyc;
static z80_t g_cpu;
static u64 g_pins;
static int g_irq_line;
static int g_nmi_pending;

// PC history for crash debugging
static u16 g_pcring[512];
static u32 g_pcring_pos;

void z80_reset() {
    g_pins = z80_reset(&g_cpu);
    g_z80_cyc = 0;
    g_irq_line = 0;
    g_nmi_pending = 0;
    g_pcring_pos = 0;
}

void z80_nmi() { g_nmi_pending = 1; }
void z80_set_irq(int on) { g_irq_line = on; }
u32 z80_get_pc() { return g_cpu.pc; }
u8 z80_get_a() { return g_cpu.a; }

extern int g_z80_nmi_enabled;
static int g_zparked;   // z80 parked in an idle loop by the slice skipper
void z80_run_until(u64 target) {
    if (g_zparked) {
        // whole-call skip: the z80 is frozen in a no-exit idle loop; only an
        // interrupt can wake it (loop flags can't change while parked).
        // parking is only engaged once the z80 has armed its NMI, so a
        // pending sound command always shows up as g_nmi_pending here.
        if (!g_z80_nmi_enabled || g_irq_line || g_nmi_pending) g_zparked = 0;
        else { g_z80_cyc = target; return; }
    }
    while (g_z80_cyc < target) {
        if (g_irq_line) g_pins |= Z80_INT; else g_pins &= ~Z80_INT;
        if (g_nmi_pending) { g_pins |= Z80_NMI; g_nmi_pending = 0; }
        if ((g_pins & Z80_HALT) && !(g_pins & (Z80_INT | Z80_NMI))) {
            // halted with no wake source within this call: nothing observable
            // happens until the next IRQ, so skip the idle ticking entirely
            g_z80_cyc = target;
            break;
        }
        g_pins = z80_tick(&g_cpu, g_pins);
        g_z80_cyc++;
        g_pins &= ~Z80_NMI;             // NMI is edge-triggered: pulse once
        if (g_pins & Z80_MREQ) {
            u16 addr = Z80_GET_ADDR(g_pins);
            if (g_pins & Z80_RD) {
                if (g_pins & Z80_M1) {
#ifdef KOF98_DIAG
                    g_pcring[g_pcring_pos++ & 511] = addr;
                    // 非法音轨命令: jp(hl) 跳到表外 ($1d5e-$1d86 但非 $1d68 合法项).
                    // 特征: 目标在该区间且前一条 M1 是 0x1c76 (jp (hl) 本身).
                    static int badjmp_dumped = 0;
                    if (!badjmp_dumped && addr >= 0x1d5e && addr <= 0x1d86 && addr != 0x1d68 &&
                        g_pcring[(g_pcring_pos - 2) & 511] == 0x1c76 && getenv("KOF98_ZTRACE")) {
                        badjmp_dumped = 1;
                        u16 ix = g_cpu.ix, iy = g_cpu.iy;
                        u8 z80_mem_read(u16);
                        fprintf(stderr, "BADJMP to %04x at zt=%.3f ix=%04x iy=%04x hl=%04x\n",
                                addr, (double)g_z80_cyc / 4000000.0, ix, iy, (u16)g_cpu.hl);
                        for (int k = 0; k < 0x27; k++)
                            fprintf(stderr, "%02x%c", z80_mem_read((u16)(ix + k)),
                                    k % 16 == 15 ? '\n' : ' ');
                        u16 sp = (u16)(z80_mem_read((u16)(ix + 0x0a)) | (z80_mem_read((u16)(ix + 0x0b)) << 8));
                        u16 lp = (u16)(z80_mem_read((u16)(ix + 0x0c)) | (z80_mem_read((u16)(ix + 0x0d)) << 8));
                        fprintf(stderr, "\nstream=%04x loop=%04x 流前后:", sp, lp);
                        for (int k = -4; k < 12; k++)
                            fprintf(stderr, " %02x", z80_mem_read((u16)(sp + k)));
                        fprintf(stderr, "\n");
                        void z80_trace_dump(FILE *f);
                        z80_trace_dump(stderr);
                    }
#endif
                    // KOF98 sound-driver idle loop at 0x012b-0x0144: spins
                    // until a timer-IRQ bumps a RAM flag. With no INT/NMI
                    // pending the flags can't change within this call, so the
                    // loop cannot exit -- skip the whole idle slice.
                    // (side note: the loop's free-running counter at 0xFDCA
                    // stops advancing during skips; only used as noise seed)
                    static int zspin = -1;
                    if (zspin < 0) zspin = getenv("KOF98_NOZSPIN") ? 0 : 1;
                    if (zspin && g_z80_nmi_enabled && !(g_pins & (Z80_INT | Z80_NMI)) && (
                            (addr == 0x012b
                             && z80_mem_read(0xFD66) == z80_mem_read(0xFD67)
                             && z80_mem_read(0xFDDB) == z80_mem_read(0xFDDC)) ||
                            (addr == 0xFFFD
                             && z80_mem_read(0xFFFD) == 0x18
                             && z80_mem_read(0xFFFE) == 0xFE))) {
                        // idle spin with no exit until an interrupt: complete
                        // the in-flight fetch, then park for the rest of the
                        // slice and subsequent calls while no IRQ is pending
                        Z80_SET_DATA(g_pins, z80_mem_read(addr));
                        g_zparked = 1;
                        g_z80_cyc = target;
                        return;
                    }
                }
                Z80_SET_DATA(g_pins, z80_mem_read(addr));
            } else if (g_pins & Z80_WR) {
                u8 dv = Z80_GET_DATA(g_pins);
#ifdef KOF98_DIAG
                static int wlog = -1;
                if (wlog < 0) wlog = getenv("KOF98_FD17LOG") ? 1 : 0;
                if (wlog && (addr == 0xFD17 || (addr >= 0xF9D0 && addr < 0xFA00) ||
                             (addr >= 0xFAC0 && addr < 0xFAE7)))
                    fprintf(stderr, "ZWR %04x=%02x pc=%04x zt=%.3f\n",
                            addr, dv, g_cpu.pc, (double)g_z80_cyc / 4000000.0);
#endif
                z80_mem_write(addr, dv);
            }
        } else if (g_pins & Z80_IORQ) {
            u16 port = Z80_GET_ADDR(g_pins);
            if (g_pins & Z80_M1) {
                Z80_SET_DATA(g_pins, 0xFF);     // int ack vector (unused in IM1)
            } else if (g_pins & Z80_RD) {
                Z80_SET_DATA(g_pins, z80_io_read(port));
            } else if (g_pins & Z80_WR) {
                z80_io_write(port, Z80_GET_DATA(g_pins));
            }
        }
    }
}

void z80_trace_dump(FILE *f); // fwd

// ---- save-state ----
struct Z80State {
    z80_t cpu;
    u64 pins, cyc;
    s32 irq_line, nmi_pending, parked;
};

int z80_state_size() { return (int)sizeof(Z80State); }

void z80_state_save(u8 *buf) {
    Z80State s;
    s.cpu = g_cpu; s.pins = g_pins; s.cyc = g_z80_cyc;
    s.irq_line = g_irq_line; s.nmi_pending = g_nmi_pending; s.parked = g_zparked;
    memcpy(buf, &s, sizeof(s));
}

void z80_state_load(const u8 *buf) {
    Z80State s;
    memcpy(&s, buf, sizeof(s));
    g_cpu = s.cpu; g_pins = s.pins; g_z80_cyc = s.cyc;
    g_irq_line = s.irq_line; g_nmi_pending = s.nmi_pending; g_zparked = s.parked;
}

#ifdef KOF98_DIAG
void z80_trace_dump(FILE *f) {
    for (int k = 512; k >= 1; k--)
        fprintf(f, "%04x\n", g_pcring[(g_pcring_pos - k) & 511]);
    fflush(f);
}
#endif
