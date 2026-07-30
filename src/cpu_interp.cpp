// 68000 interpreter, part 1: engine + opcodes 0x0..0x7
#include "cpu_internal.h"

u32 g_usp, g_ssp;

void cpu_check_irq() {
    int lv = g_irq_level;
    if (lv > cpu.iml || lv == 7) {
        if (getenv("KOF98_EXLOG"))
            fprintf(stderr, "IRQ lv=%d pc=%06x cyc=%llu\n", lv, cpu.pc, (unsigned long long)cpu.cyc);
        u16 sr = cpu_get_sr();
        set_S(1); cpu.T = 0;
        mem_write32(cpu.A[7] - 4, cpu.pc);
        mem_write16(cpu.A[7] - 6, sr);
        cpu.A[7] -= 6;
        cpu.iml = (u8)lv;
        cpu.stopped = 0;
        cpu.pc = mem_read32((24 + lv) * 4);
        cpu.cyc += 44;
    }
}

// KOF98 main-loop vblank waits: TST.B ($2785,a5) + BEQ self. The flag is only
// written by the IRQ1 handler, so when no IRQ is pending the loop is a pure
// idle: safe to fast-forward to the next scheduling point (line end / irq2 /
// watchdog). Disable with KOF98_NOSPIN=1.
// CRITICAL: only skip while the flag is CLEAR. Once the IRQ sets it, the
// loop's TST/BEQ must actually execute to observe it and exit -- skipping
// unconditionally would strand the game in the wait loop forever.
static int g_spin_opt = -1;
static inline int spin_ff(u64 target) {
    if (g_spin_opt < 0) g_spin_opt = getenv("KOF98_NOSPIN") ? 0 : 1;
    if (!g_spin_opt || g_irq_level) return 0;
    switch (cpu.pc) {
    case 0x9f6e: case 0x9fe6: case 0xa142: case 0xa180: case 0xa1b0:
        if (mem_read8((cpu.A[5] + 0x2785) & 0xFFFFFF) != 0) return 0;
        cpu.cyc = target;
        return 1;
    }
    return 0;
}

// 68k PC 周期剖析 (KOF98_PROF api): g_prof[pc>>4] 累计周期, g_prof_n 累计指令数
u32 *g_prof, *g_prof_n;
u64 g_prof_instr;
int g_prof_on;

void cpu_run_until(u64 target) {
    u64 prev = ~(u64)0; int stagnant = 0;
    while (cpu.cyc < target) {
        g_ntrace = 10;
        if (g_irq_level) cpu_check_irq();
        if (cpu.stopped) { cpu.cyc = target; return; }
        if (spin_ff(target)) return;
        g_ntrace = 11;
        if (g_prof_on) {
            u32 ppc = cpu.pc;
            u64 c0 = cpu.cyc;
            cpu_interp_step();
            g_prof[(ppc >> 4) & 0xFFFFF] += (u32)(cpu.cyc - c0);
            g_prof_n[(ppc >> 4) & 0xFFFFF]++;
            g_prof_instr++;
        } else {
            cpu_interp_step();
        }
        g_ntrace = 1;
        if (cpu.cyc == prev) {          // native infinite-loop guard
            if (++stagnant > 1000) {
                fprintf(stderr, "STALL pc=%06x op=%04x cyc=%llu\n", cpu.pc,
                        mem_read16(cpu.pc), (unsigned long long)cpu.cyc);
                exit(4);
            }
        } else stagnant = 0;
        prev = cpu.cyc;
    }
}

static void op_imm_sr(int opc, int sz, u32 imm, u32 pc0, int &cyc) {
    // ORI/ANDI/EORI to CCR (byte) or SR (word)
    if (sz == 0) {
        if (opc == 0) cpu_set_ccr((u8)(cpu_get_sr() | imm));
        else if (opc == 1) cpu_set_ccr((u8)(cpu_get_sr() & imm));
        else cpu_set_ccr((u8)(cpu_get_sr() ^ imm));
        cyc += 20;
    } else {
        if (!cpu.S) { cpu_exception(8, pc0); cyc += 34; return; }
        u16 sr = cpu_get_sr();
        if (opc == 0) sr |= (u16)imm;
        else if (opc == 1) sr &= (u16)imm;
        else sr ^= (u16)imm;
        cpu_set_sr(sr); cpu_after_sr_change();
        cyc += 20;
    }
}

u32 g_pcring[1024];
u32 g_pcring_pos;
void cpu_interp_step() {
    u32 pc0 = cpu.pc;
    g_pcring[g_pcring_pos++ & 1023] = pc0;
    if (g_cov_enabled) cov_mark(pc0);
    // lightweight PC watchpoints: KOF98_WATCH=c11c5c,c11c66,... logs each hit
    static u32 watch[16]; static int nwatch = -1;
    if (nwatch < 0) {
        nwatch = 0;
        const char *w = getenv("KOF98_WATCH");
        if (w) {
            char buf[256]; strncpy(buf, w, 255); buf[255] = 0;
            for (char *t = strtok(buf, ","); t && nwatch < 16; t = strtok(NULL, ","))
                watch[nwatch++] = (u32)strtoul(t, NULL, 16);
        }
    }
    for (int i = 0; i < nwatch; i++)
        if (pc0 == watch[i])
            fprintf(stderr, "WATCH %06x cyc=%llu d6=%08x a6=%06x\n", pc0, (unsigned long long)cpu.cyc, cpu.D[6], cpu.A[6]);
    static int trace_cond = -1, trace_left = 0, ring_on = -1;
    static u32 ring[8192]; static int ring_pos;
    if (trace_cond < 0) {
        const char *e = getenv("KOF98_TRACE_COND");
        trace_cond = e ? (e[0] == 'a' ? 2 : e[0] == 'p' ? 3 : 1) : 0;
    }
    if (trace_cond) {
        ring[ring_pos & 8191] = pc0; ring_pos++;
        int hit = trace_cond == 2 ? (cpu.A[7] < 0x10f2d0)
                : trace_cond == 3 ? (pc0 == (u32)strtoul(getenv("KOF98_TRACE_COND") + 1, NULL, 16))
                : (pc0 < 0x100000 && pc0 != 0 && pc0 != 4);
        if (hit && !trace_left) {
            trace_left = getenv("KOF98_TRACE_LONG") ? 60000 : 2000;
            for (int k = 0; k < 8192 && k < ring_pos; k++) {
                u32 p = ring[(ring_pos - 8192 + k) & 8191];
                if (ring_pos < 8192) p = ring[k];
                fprintf(g_trace, "HIST %06x\n", ring_pos < 8192 ? ring[k] : ring[(ring_pos - 8192 + k) & 8191]);
            }
            fprintf(g_trace, "TRIG ----\n");
        }
    }
    if (g_trace && (!trace_cond || trace_left > 0)) {
        fprintf(g_trace, "%06x D:%08x %08x %08x %08x %08x %08x %08x %08x A:%08x %08x %08x %08x %08x %08x %08x %08x SR:%04x\n",
                pc0, cpu.D[0], cpu.D[1], cpu.D[2], cpu.D[3], cpu.D[4], cpu.D[5], cpu.D[6], cpu.D[7],
                cpu.A[0], cpu.A[1], cpu.A[2], cpu.A[3], cpu.A[4], cpu.A[5], cpu.A[6], cpu.A[7], cpu_get_sr());
        if (trace_cond) trace_left--;
    }
    u16 op = nextw();
    int cyc = 0;
    int top = op >> 12;

    if (top >= 0x8) { cpu_interp_hi(op, pc0, cyc); cpu.cyc += (cyc > 0 ? cyc : 4); return; }

    switch (top) {
    case 0x0: {
        if ((op & 0x0138) == 0x0108) {      // MOVEP
            int dr = (op >> 9) & 7, sz = (op >> 6) & 1, dir = (op >> 7) & 1, ar = op & 7;
            u32 a = cpu.A[ar] + (u32)(s32)(s16)nextw();
            if (dir == 0) {
                if (sz == 0) cpu.D[dr] = (cpu.D[dr] & 0x0000FFFF) | ((u32)mem_read8(a) << 24) | ((u32)mem_read8(a + 2) << 16);
                else cpu.D[dr] = ((u32)mem_read8(a) << 24) | ((u32)mem_read8(a + 2) << 16) | ((u32)mem_read8(a + 4) << 8) | mem_read8(a + 6);
            } else {
                if (sz == 0) { mem_write8(a, (u8)(cpu.D[dr] >> 8)); mem_write8(a + 2, (u8)cpu.D[dr]); }
                else {
                    mem_write8(a, (u8)(cpu.D[dr] >> 24)); mem_write8(a + 2, (u8)(cpu.D[dr] >> 16));
                    mem_write8(a + 4, (u8)(cpu.D[dr] >> 8)); mem_write8(a + 6, (u8)cpu.D[dr]);
                }
            }
            cyc += sz ? 24 : 16;
            break;
        }
        if ((op & 0x0F00) == 0x0800) {      // static bit ops #imm
            int which = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
            u32 bitnum = nextw();
            if (mode == 0) {
                bitnum &= 31;
                u32 d = cpu.D[reg];
                cpu.Z = (d & (1u << bitnum)) == 0;
                if (which == 1) d ^= (1u << bitnum);
                else if (which == 2) d &= ~(1u << bitnum);
                else if (which == 3) d |= (1u << bitnum);
                cpu.D[reg] = d;
                cyc += (which == 0) ? 10 : 12;
            } else {
                int c2 = 0; EA e = ea_addr<0>(mode, reg, c2);
                u32 d = mem_read8(e.addr);
                bitnum &= 7;
                cpu.Z = (d & (1u << bitnum)) == 0;
                if (which == 1) d ^= (1u << bitnum);
                else if (which == 2) d &= ~(1u << bitnum);
                else if (which == 3) d |= (1u << bitnum);
                if (which) mem_write8(e.addr, (u8)d);
                cyc += c2 + (which == 0 ? 8 : 12);
            }
            break;
        }
        if (op & 0x0100) {                  // dynamic bit ops
            int which = (op >> 6) & 3, mode = (op >> 3) & 7, reg = op & 7;
            u32 bitnum = cpu.D[(op >> 9) & 7];
            if (mode == 0) {
                bitnum &= 31;
                u32 d = cpu.D[reg];
                cpu.Z = (d & (1u << bitnum)) == 0;
                if (which == 1) d ^= (1u << bitnum);
                else if (which == 2) d &= ~(1u << bitnum);
                else if (which == 3) d |= (1u << bitnum);
                cpu.D[reg] = d;
                cyc += (which == 0) ? 6 : 8;
            } else {
                int c2 = 0; EA e = ea_addr<0>(mode, reg, c2);
                u32 d = mem_read8(e.addr);
                bitnum &= 7;
                cpu.Z = (d & (1u << bitnum)) == 0;
                if (which == 1) d ^= (1u << bitnum);
                else if (which == 2) d &= ~(1u << bitnum);
                else if (which == 3) d |= (1u << bitnum);
                if (which) mem_write8(e.addr, (u8)d);
                cyc += c2 + (which == 0 ? 4 : 8);
            }
            break;
        }
        int opc = (op >> 9) & 7, sz = (op >> 6) & 3;
        int mode = (op >> 3) & 7, reg = op & 7;
        if (sz == 3 || opc == 4 || opc == 7) { cpu_exception(4, pc0); cyc += 34; break; }
        u32 imm = sz == 0 ? (nextw() & 0xFF) : sz == 1 ? nextw() : nextl();
        if ((op & 0x3F) == 0x3C && (opc == 0 || opc == 1 || opc == 5)) { op_imm_sr(opc, sz, imm, pc0, cyc); break; }
        int c2 = 0;
        if (opc == 6) {                     // CMPI
            if (sz == 0) { u32 d = ea_read<0>(mode, reg, c2); alu_cmp<0>(imm, d); }
            else if (sz == 1) { u32 d = ea_read<1>(mode, reg, c2); alu_cmp<1>(imm, d); }
            else { u32 d = ea_read<2>(mode, reg, c2); alu_cmp<2>(imm, d); }
            cyc += (mode == 0 ? (sz == 2 ? 14 : 8) : (sz == 2 ? 12 : 8)) + c2;
        } else {
            if (sz == 0) {
                EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e);
                if (opc == 0) ea_write<0>(e, alu_logic<0>(d | imm));
                else if (opc == 1) ea_write<0>(e, alu_logic<0>(d & imm));
                else if (opc == 2) ea_write<0>(e, alu_sub<0>(imm, d));
                else if (opc == 3) ea_write<0>(e, alu_add<0>(imm, d));
                else ea_write<0>(e, alu_logic<0>(d ^ imm));
            } else if (sz == 1) {
                EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e);
                if (opc == 0) ea_write<1>(e, alu_logic<1>(d | imm));
                else if (opc == 1) ea_write<1>(e, alu_logic<1>(d & imm));
                else if (opc == 2) ea_write<1>(e, alu_sub<1>(imm, d));
                else if (opc == 3) ea_write<1>(e, alu_add<1>(imm, d));
                else ea_write<1>(e, alu_logic<1>(d ^ imm));
            } else {
                EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e);
                if (opc == 0) ea_write<2>(e, alu_logic<2>(d | imm));
                else if (opc == 1) ea_write<2>(e, alu_logic<2>(d & imm));
                else if (opc == 2) ea_write<2>(e, alu_sub<2>(imm, d));
                else if (opc == 3) ea_write<2>(e, alu_add<2>(imm, d));
                else ea_write<2>(e, alu_logic<2>(d ^ imm));
            }
            cyc += (mode == 0 ? (sz == 2 ? 16 : 8) : (sz == 2 ? 20 : 12)) + c2;
        }
        break;
    }

    case 0x1: case 0x2: case 0x3: {         // MOVE / MOVEA
        int sz = top == 1 ? 0 : top == 3 ? 1 : 2;
        int dstm = (op >> 6) & 7, dstr = (op >> 9) & 7;
        int srcm = (op >> 3) & 7, srcr = op & 7;
        int c1 = 0, c2 = 0;
        if (sz == 0) {
            u32 v = ea_read<0>(srcm, srcr, c1);
            EA e = ea_addr<0>(dstm, dstr, c2);
            ea_write<0>(e, alu_logic<0>(v));
        } else if (sz == 1) {
            u32 v = ea_read<1>(srcm, srcr, c1);
            if (dstm == 1) cpu.A[dstr] = sext_sz(v, 1);
            else { EA e = ea_addr<1>(dstm, dstr, c2); ea_write<1>(e, alu_logic<1>(v)); }
        } else {
            u32 v = ea_read<2>(srcm, srcr, c1);
            if (dstm == 1) cpu.A[dstr] = v;
            else { EA e = ea_addr<2>(dstm, dstr, c2); ea_write<2>(e, alu_logic<2>(v)); }
        }
        cyc += 4 + c1 + c2;
        break;
    }

    case 0x4: {
        int mode = (op >> 3) & 7, reg = op & 7;
        u16 m = op & 0xFFC0;
        if ((op & 0xFFF0) == 0x4E40) { cpu_exception(32 + (op & 0xF), cpu.pc); cyc += 34; break; } // TRAP
        if ((op & 0xFFF8) == 0x4E70) {
            if (op == 0x4E71) { cyc += 4; break; }                          // NOP
            if (!cpu.S) { cpu_exception(8, pc0); cyc += 34; break; }
            if (op == 0x4E70) { cyc += 4; break; }                          // RESET
            if (op == 0x4E72) { u16 nsr = nextw(); cpu_set_sr(nsr); cpu_after_sr_change(); cpu.stopped = 1; cyc += 4; break; } // STOP
            if (op == 0x4E73) {                                             // RTE
                u16 sr = mem_read16(cpu.A[7]); u32 npc = mem_read32(cpu.A[7] + 2); cpu.A[7] += 6;
                cpu_set_sr(sr); cpu_after_sr_change(); cpu.pc = npc; cyc += 20; break;
            }
        }
        if (op == 0x4E75) { cpu.pc = mem_read32(cpu.A[7]); cpu.A[7] += 4; cyc += 16; break; } // RTS
        if (op == 0x4E76) { if (cpu.V) { cpu_exception(7, cpu.pc); cyc += 34; } else cyc += 4; break; } // TRAPV
        if (op == 0x4E77) {                                                 // RTR
            u16 sr = mem_read16(cpu.A[7]); u32 npc = mem_read32(cpu.A[7] + 2); cpu.A[7] += 6;
            cpu_set_ccr((u8)sr); cpu.pc = npc; cyc += 20; break;
        }
        if ((op & 0xFFF8) == 0x4E50) {                                      // LINK
            int r = op & 7; s32 disp = (s32)(s16)nextw();
            mem_write32(cpu.A[7] - 4, cpu.A[r]); cpu.A[7] -= 4;
            cpu.A[r] = cpu.A[7]; cpu.A[7] += disp; cyc += 16; break;
        }
        if ((op & 0xFFF8) == 0x4E58) {                                      // UNLK
            int r = op & 7; cpu.A[7] = cpu.A[r]; cpu.A[r] = mem_read32(cpu.A[7]); cpu.A[7] += 4; cyc += 12; break;
        }
        if ((op & 0xFFF0) == 0x4E60) {                                      // MOVE USP
            if (!cpu.S) { cpu_exception(8, pc0); cyc += 34; break; }
            int r = op & 7;
            if (op & 8) g_usp = cpu.A[r]; else cpu.A[r] = g_usp;
            cyc += 4; break;
        }
        if (m == 0x4E80) { int c2 = 0; EA e = ea_addr<0>(mode, reg, c2);    // JSR
            mem_write32(cpu.A[7] - 4, cpu.pc); cpu.A[7] -= 4; cpu.pc = e.addr; cyc += c2 + 8; break; }
        if (m == 0x4EC0) { int c2 = 0; EA e = ea_addr<0>(mode, reg, c2); cpu.pc = e.addr; cyc += c2 + 4; break; } // JMP
        if ((op & 0xFF00) == 0x4000 && (op & 0xC0) != 0xC0) {               // NEGX
            int sz = (op >> 6) & 3; int c2 = 0;
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); u32 r = (0 - d - cpu.X) & 0xFF; cpu.C = cpu.X = ((d | r) & 0xFF) != 0; cpu.V = (d & r & 0x80) != 0; cpu.N = (r & 0x80) != 0; cpu.Z &= (r == 0); ea_write<0>(e, r); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); u32 r = (0 - d - cpu.X) & 0xFFFF; cpu.C = cpu.X = ((d | r) & 0xFFFF) != 0; cpu.V = (d & r & 0x8000) != 0; cpu.N = (r & 0x8000) != 0; cpu.Z &= (r == 0); ea_write<1>(e, r); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); u32 r = (0u - d - cpu.X); cpu.C = cpu.X = ((d | r) != 0); cpu.V = (d & r & 0x80000000u) != 0; cpu.N = (r & 0x80000000u) != 0; cpu.Z &= (r == 0); ea_write<2>(e, r); }
            cyc += (mode == 0 ? ((op >> 6 & 3) == 2 ? 6 : 4) : 12) + c2; break;
        }
        if (m == 0x40C0) { int c2 = 0; EA e = ea_addr<1>(mode, reg, c2); ea_write<1>(e, cpu_get_sr()); cyc += (mode == 0 ? 6 : 8) + c2; break; } // MOVE SR,<ea>
        if ((op & 0xFF00) == 0x4200 && (op & 0xC0) != 0xC0) {               // CLR
            int sz = (op >> 6) & 3; int c2 = 0;
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); ea_write<0>(e, 0); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); ea_write<1>(e, 0); }
            else { EA e = ea_addr<2>(mode, reg, c2); ea_write<2>(e, 0); }
            cpu.N = 0; cpu.Z = 1; cpu.V = 0; cpu.C = 0;
            cyc += (mode == 0 ? ((op >> 6 & 3) == 2 ? 6 : 4) : 12) + c2; break;
        }
        if ((op & 0xFF00) == 0x4400 && (op & 0xC0) != 0xC0) {               // NEG
            int sz = (op >> 6) & 3; int c2 = 0;
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); ea_write<0>(e, alu_sub<0>(d, 0)); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); ea_write<1>(e, alu_sub<1>(d, 0)); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); ea_write<2>(e, alu_sub<2>(d, 0)); }
            cyc += (mode == 0 ? ((op >> 6 & 3) == 2 ? 6 : 4) : 12) + c2; break;
        }
        if (m == 0x44C0) { int c2 = 0; u32 v = ea_read<1>(mode, reg, c2); cpu_set_ccr((u8)v); cyc += 12 + c2; break; } // MOVE <ea>,CCR
        if ((op & 0xFF00) == 0x4600 && (op & 0xC0) != 0xC0) {               // NOT
            int sz = (op >> 6) & 3; int c2 = 0;
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); ea_write<0>(e, alu_logic<0>(~d)); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); ea_write<1>(e, alu_logic<1>(~d)); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); ea_write<2>(e, alu_logic<2>(~d)); }
            cyc += (mode == 0 ? ((op >> 6 & 3) == 2 ? 6 : 4) : 12) + c2; break;
        }
        if (m == 0x46C0) {                                                  // MOVE <ea>,SR
            if (!cpu.S) { cpu_exception(8, pc0); cyc += 34; break; }
            int c2 = 0; u32 v = ea_read<1>(mode, reg, c2); cpu_set_sr((u16)v); cpu_after_sr_change(); cyc += 12 + c2; break;
        }
        if (m == 0x4800) {                                                  // NBCD
            int c2 = 0; EA e = ea_addr<0>(mode, reg, c2);
            u32 d = ea_value<0>(e);
            u32 r = bcd_sub(d, 0);
            cpu.C = cpu.X = (r != 0);
            ea_write<0>(e, r);
            cyc += (mode == 0 ? 6 : 12) + c2; break;
        }
        if ((op & 0xFFF8) == 0x4840) {                                      // SWAP Dn
            u32 d = cpu.D[reg]; d = (d >> 16) | (d << 16); cpu.D[reg] = d;
            fl_nz<2>(d); cpu.V = 0; cpu.C = 0; cyc += 4; break;
        }
        if (m == 0x4840) { int c2 = 0; EA e = ea_addr<0>(mode, reg, c2);    // PEA
            mem_write32(cpu.A[7] - 4, e.addr); cpu.A[7] -= 4; cyc += c2 + 8; break; }
        if ((op & 0xFFF8) == 0x4880) {                                      // EXT.W Dn
            u32 d = (cpu.D[reg] & 0x80) ? (cpu.D[reg] | 0xFF00) : (cpu.D[reg] & 0xFF);
            cpu.D[reg] = (cpu.D[reg] & 0xFFFF0000) | (d & 0xFFFF);
            fl_nz<1>(d & 0xFFFF); cpu.V = 0; cpu.C = 0; cyc += 4; break;
        }
        if ((op & 0xFFF8) == 0x48C0) {                                      // EXT.L Dn
            u32 d = (u32)(s32)(s16)cpu.D[reg]; cpu.D[reg] = d;
            fl_nz<2>(d); cpu.V = 0; cpu.C = 0; cyc += 4; break;
        }
        if ((op & 0xFB80) == 0x4880) {                                      // MOVEM
            int dir = (op >> 10) & 1, sz = (op >> 6) & 1;
            u16 msk16 = nextw();
            int c2 = 0;
            // NOTE: modes 3/4 ((An)+ / -(An)) must NOT auto-inc/dec via ea_addr;
            // the movem sequence performs the pointer updates itself.
            u32 a;
            if (mode == 4 || mode == 3) { a = cpu.A[reg]; c2 = 4; }
            else { EA e = ea_addr<0>(mode, reg, c2); a = e.addr; }
            int n = 0;
            if (dir == 0) {
                if (mode == 4) {
                    for (int r = 15; r >= 0; r--) if (msk16 & (1 << (15 - r))) {
                        u32 v = r < 8 ? cpu.D[r] : cpu.A[r - 8];
                        if (sz) { a -= 4; mem_write32(a, v); } else { a -= 2; mem_write16(a, (u16)v); }
                        n++;
                    }
                    cpu.A[reg] = a;
                } else {
                    for (int r = 0; r < 16; r++) if (msk16 & (1 << r)) {
                        u32 v = r < 8 ? cpu.D[r] : cpu.A[r - 8];
                        if (sz) { mem_write32(a, v); a += 4; } else { mem_write16(a, (u16)v); a += 2; }
                        n++;
                    }
                    if (mode == 3) cpu.A[reg] = a;
                }
            } else {
                for (int r = 0; r < 16; r++) if (msk16 & (1 << r)) {
                    u32 v;
                    if (sz) { v = mem_read32(a); a += 4; } else { v = (u32)(s32)(s16)mem_read16(a); a += 2; }
                    if (r < 8) cpu.D[r] = v; else cpu.A[r - 8] = v;
                    n++;
                }
                if (mode == 3) cpu.A[reg] = a;
            }
            cyc += c2 + 8 + n * (sz ? 8 : 4);
            break;
        }
        if ((op & 0xF1C0) == 0x4180) {                                      // CHK
            int c2 = 0; u32 bound = ea_read<1>(mode, reg, c2);
            s32 v = (s32)(s16)cpu.D[(op >> 9) & 7];
            s32 b = (s32)(s16)bound;
            if (v < 0) { cpu.N = 1; cpu_exception(6, pc0); cyc += 40; }
            else if (v > b) { cpu.N = 0; cpu_exception(6, pc0); cyc += 40; }
            else cyc += 10 + c2;
            break;
        }
        if ((op & 0xF1C0) == 0x41C0) { int c2 = 0; EA e = ea_addr<0>(mode, reg, c2); cpu.A[(op >> 9) & 7] = e.addr; cyc += c2 + 4; break; } // LEA
        if ((op & 0xFF00) == 0x4A00 && (op & 0xC0) != 0xC0) {               // TST
            int sz = (op >> 6) & 3; int c2 = 0;
            if (sz == 0) { u32 d = ea_read<0>(mode, reg, c2); fl_nz<0>(d); }
            else if (sz == 1) { u32 d = ea_read<1>(mode, reg, c2); fl_nz<1>(d); }
            else { u32 d = ea_read<2>(mode, reg, c2); fl_nz<2>(d); }
            cpu.V = 0; cpu.C = 0;
            cyc += 4 + c2; break;
        }
        if (m == 0x4AC0) {                                                  // TAS
            int c2 = 0; EA e = ea_addr<0>(mode, reg, c2);
            u32 d = ea_value<0>(e);
            fl_nz<0>(d); cpu.V = 0; cpu.C = 0;
            ea_write<0>(e, d | 0x80);
            cyc += (mode == 0 ? 4 : 14) + c2; break;
        }
        cpu_exception(4, pc0); cyc += 34;
        break;
    }

    case 0x5: {
        if ((op & 0xC0) == 0xC0) {
            int cc = (op >> 8) & 0xF;
            if (op & 0x0100) {                                              // DBcc (invalid encodings fall here too)
                if ((op & 0x38) != 0x08) { cpu_exception(4, pc0); cyc += 34; break; }
                int reg = op & 7;
                u32 base = cpu.pc;          // base = displacement word address
                s16 disp = (s16)nextw();
                if (!cond_true(cc)) {
                    u16 w = (u16)(cpu.D[reg] - 1);
                    cpu.D[reg] = (cpu.D[reg] & 0xFFFF0000) | w;
                    if (w != 0xFFFF) { cpu.pc = base + disp; cyc += 10; }
                    else cyc += 14;
                } else cyc += 12;
            } else {                                                        // Scc
                int c2 = 0; EA e = ea_addr<0>((op >> 3) & 7, op & 7, c2);
                int t = cond_true(cc);
                ea_write<0>(e, t ? 0xFF : 0);
                cyc += c2 + (e.is_reg == 0 ? (t ? 6 : 4) : 8);
            }
            break;
        }
        int sz = (op >> 6) & 3;
        if (sz == 3) { cpu_exception(4, pc0); cyc += 34; break; }
        int mode = (op >> 3) & 7, reg = op & 7;
        u32 q = (op >> 9) & 7; if (q == 0) q = 8;
        int b8 = (op >> 8) & 1;
        int c2 = 0;
        if (mode == 1) {
            if (b8) cpu.A[reg] -= q; else cpu.A[reg] += q;
            cyc += 8;
        } else if (sz == 0) {
            EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e);
            u8 x = cpu.X; u32 r = b8 ? alu_sub<0>(q, d) : alu_add<0>(q, d); cpu.X = x;
            ea_write<0>(e, r);
            cyc += (mode == 0 ? 4 : 8) + c2;
        } else if (sz == 1) {
            EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e);
            u8 x = cpu.X; u32 r = b8 ? alu_sub<1>(q, d) : alu_add<1>(q, d); cpu.X = x;
            ea_write<1>(e, r);
            cyc += (mode == 0 ? 4 : 8) + c2;
        } else {
            EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e);
            u8 x = cpu.X; u32 r = b8 ? alu_sub<2>(q, d) : alu_add<2>(q, d); cpu.X = x;
            ea_write<2>(e, r);
            cyc += (mode == 0 ? 8 : 12) + c2;
        }
        break;
    }

    case 0x6: {
        int cc = (op >> 8) & 0xF;
        u32 base = cpu.pc;              // base = extension word address (instr+2)
        s32 disp = (s8)(op & 0xFF);
        if ((op & 0xFF) == 0) disp = (s16)nextw();
        if (cc == 0) { cpu.pc = base + disp; cyc += 10; }
        else if (cc == 1) { mem_write32(cpu.A[7] - 4, cpu.pc); cpu.A[7] -= 4; cpu.pc = base + disp; cyc += 18; }
        else if (cond_true(cc)) { cpu.pc = base + disp; cyc += 10; }
        else cyc += (op & 0xFF) == 0 ? 12 : 8;
        break;
    }

    case 0x7: {
        int r = (op >> 9) & 7;
        u32 v = (u32)(s32)(s8)(op & 0xFF);
        cpu.D[r] = v;
        fl_nz<2>(v); cpu.V = 0; cpu.C = 0;
        cyc += 4;
        break;
    }
    }

    cpu.cyc += (cyc > 0 ? cyc : 4);
}
