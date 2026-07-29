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

void z80_run_until(u64 target) {
    while (g_z80_cyc < target) {
        if (g_irq_line) g_pins |= Z80_INT; else g_pins &= ~Z80_INT;
        if (g_nmi_pending) { g_pins |= Z80_NMI; g_nmi_pending = 0; }
        g_pins = z80_tick(&g_cpu, g_pins);
        g_z80_cyc++;
        g_pins &= ~Z80_NMI;             // NMI is edge-triggered: pulse once
        if (g_pins & Z80_MREQ) {
            u16 addr = Z80_GET_ADDR(g_pins);
            if (g_pins & Z80_RD) {
                if (g_pins & Z80_M1) g_pcring[g_pcring_pos++ & 511] = addr;
                Z80_SET_DATA(g_pins, z80_mem_read(addr));
            } else if (g_pins & Z80_WR) {
                z80_mem_write(addr, Z80_GET_DATA(g_pins));
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

void z80_trace_dump(FILE *f) {
    for (int k = 200; k >= 1; k--)
        fprintf(f, "%04x\n", g_pcring[(g_pcring_pos - k) & 511]);
    fflush(f);
}
