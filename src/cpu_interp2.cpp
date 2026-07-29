// 68000 interpreter, part 2: opcodes 0x8..0xE
#include "cpu_internal.h"

void cpu_interp_hi(u16 op, u32 pc0, int &cyc) {
    int top = op >> 12;
    int mode = (op >> 3) & 7, reg = op & 7;

    switch (top) {
    case 0x8: {
        int dr = (op >> 9) & 7, b8 = (op >> 8) & 1, sz = (op >> 6) & 3;
        if (sz == 3) {                      // DIVU / DIVS
            int c2 = 0;
            if (b8 == 0) {
                u32 s = ea_read<1>(mode, reg, c2);
                if (s == 0) { cpu_exception(5, pc0); cyc += 38; return; }
                u32 dvd = cpu.D[dr];
                u32 q = dvd / s, rem = dvd % s;
                if (q > 0xFFFF) { cpu.V = 1; cpu.C = 0; cyc += 140 + c2; }
                else { cpu.D[dr] = (rem << 16) | q; fl_nz<1>(q); cpu.V = 0; cpu.C = 0; cyc += 133 + c2; }
            } else {
                u32 sv = ea_read<1>(mode, reg, c2);
                s32 s = (s32)(s16)sv;
                if (s == 0) { cpu_exception(5, pc0); cyc += 38; return; }
                s32 dvd = (s32)cpu.D[dr];
                s32 q = dvd / s, rem = dvd % s;
                if (q > 32767 || q < -32768) { cpu.V = 1; cpu.C = 0; cyc += 158 + c2; }
                else { cpu.D[dr] = ((u32)(rem & 0xFFFF) << 16) | ((u32)q & 0xFFFF); fl_nz<1>((u32)q & 0xFFFF); cpu.V = 0; cpu.C = 0; cyc += 150 + c2; }
            }
            return;
        }
        if (b8 == 1 && (op & 0x00F8) == 0x0000) {   // SBCD reg
            u32 s = cpu.D[reg] & 0xFF, d = cpu.D[dr] & 0xFF;
            cpu.D[dr] = (cpu.D[dr] & ~0xFFu) | bcd_sub(s, d);
            cyc += 6; return;
        }
        if (b8 == 1 && (op & 0x00F8) == 0x0008) {   // SBCD mem
            cpu.A[reg] -= (reg == 7) ? 2 : 1; u32 s = mem_read8(cpu.A[reg]);
            cpu.A[dr] -= (dr == 7) ? 2 : 1; u32 d = mem_read8(cpu.A[dr]);
            mem_write8(cpu.A[dr], (u8)bcd_sub(s, d));
            cyc += 18; return;
        }
        int c2 = 0;
        if (b8 == 0) {                      // OR <ea>,Dn
            if (sz == 0) { u32 s = ea_read<0>(mode, reg, c2); cpu.D[dr] = (cpu.D[dr] & ~0xFFu) | alu_logic<0>(s | (cpu.D[dr] & 0xFF)); }
            else if (sz == 1) { u32 s = ea_read<1>(mode, reg, c2); cpu.D[dr] = (cpu.D[dr] & ~0xFFFFu) | alu_logic<1>(s | (cpu.D[dr] & 0xFFFF)); }
            else { u32 s = ea_read<2>(mode, reg, c2); cpu.D[dr] = alu_logic<2>(s | cpu.D[dr]); }
            cyc += (sz == 2 ? 6 : 4) + c2;
        } else {                            // OR Dn,<ea>
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); ea_write<0>(e, alu_logic<0>(d | (cpu.D[dr] & 0xFF))); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); ea_write<1>(e, alu_logic<1>(d | (cpu.D[dr] & 0xFFFF))); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); ea_write<2>(e, alu_logic<2>(d | cpu.D[dr])); }
            cyc += (sz == 2 ? 12 : 8) + c2;
        }
        return;
    }

    case 0x9: case 0xD: {
        int is_add = top == 0xD;
        int dr = (op >> 9) & 7, b8 = (op >> 8) & 1, sz = (op >> 6) & 3;
        if (sz == 3) {                      // ADDA / SUBA
            int c2 = 0; u32 v;
            if (b8 == 0) v = sext_sz(ea_read<1>(mode, reg, c2), 1);
            else v = ea_read<2>(mode, reg, c2);
            if (is_add) cpu.A[dr] += v; else cpu.A[dr] -= v;
            cyc += 8 + c2;
            return;
        }
        if (b8 == 1 && (mode == 0 || mode == 4)) {  // ADDX / SUBX
            if (mode == 0) {
                if (sz == 0) { u32 s = cpu.D[reg] & 0xFF, d = cpu.D[dr] & 0xFF; u32 r = is_add ? alu_addx<0>(s, d) : alu_subx<0>(s, d); cpu.D[dr] = (cpu.D[dr] & ~0xFFu) | r; }
                else if (sz == 1) { u32 s = cpu.D[reg] & 0xFFFF, d = cpu.D[dr] & 0xFFFF; u32 r = is_add ? alu_addx<1>(s, d) : alu_subx<1>(s, d); cpu.D[dr] = (cpu.D[dr] & ~0xFFFFu) | r; }
                else { cpu.D[dr] = is_add ? alu_addx<2>(cpu.D[reg], cpu.D[dr]) : alu_subx<2>(cpu.D[reg], cpu.D[dr]); }
                cyc += (sz == 2 ? 8 : 4);
            } else {
                cpu.A[reg] -= (reg == 7 && sz == 0) ? 2 : (1 << sz);
                cpu.A[dr] -= (dr == 7 && sz == 0) ? 2 : (1 << sz);
                u32 a_s = cpu.A[reg], a_d = cpu.A[dr];
                if (sz == 0) { u32 s = mem_read8(a_s), d = mem_read8(a_d); mem_write8(a_d, (u8)(is_add ? alu_addx<0>(s, d) : alu_subx<0>(s, d))); }
                else if (sz == 1) { u32 s = mem_read16(a_s), d = mem_read16(a_d); mem_write16(a_d, (u16)(is_add ? alu_addx<1>(s, d) : alu_subx<1>(s, d))); }
                else { u32 s = mem_read32(a_s), d = mem_read32(a_d); mem_write32(a_d, is_add ? alu_addx<2>(s, d) : alu_subx<2>(s, d)); }
                cyc += (sz == 2 ? 30 : 18);
            }
            return;
        }
        int c2 = 0;
        if (b8 == 0) {
            if (sz == 0) { u32 s = ea_read<0>(mode, reg, c2); u32 d = cpu.D[dr] & 0xFF; u32 r = is_add ? alu_add<0>(s, d) : alu_sub<0>(s, d); cpu.D[dr] = (cpu.D[dr] & ~0xFFu) | r; }
            else if (sz == 1) { u32 s = ea_read<1>(mode, reg, c2); u32 d = cpu.D[dr] & 0xFFFF; u32 r = is_add ? alu_add<1>(s, d) : alu_sub<1>(s, d); cpu.D[dr] = (cpu.D[dr] & ~0xFFFFu) | r; }
            else { u32 s = ea_read<2>(mode, reg, c2); cpu.D[dr] = is_add ? alu_add<2>(s, cpu.D[dr]) : alu_sub<2>(s, cpu.D[dr]); }
            cyc += (sz == 2 ? 6 : 4) + c2;
        } else {
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); ea_write<0>(e, is_add ? alu_add<0>(cpu.D[dr] & 0xFF, d) : alu_sub<0>(cpu.D[dr] & 0xFF, d)); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); ea_write<1>(e, is_add ? alu_add<1>(cpu.D[dr] & 0xFFFF, d) : alu_sub<1>(cpu.D[dr] & 0xFFFF, d)); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); ea_write<2>(e, is_add ? alu_add<2>(cpu.D[dr], d) : alu_sub<2>(cpu.D[dr], d)); }
            cyc += (sz == 2 ? 12 : 8) + c2;
        }
        return;
    }

    case 0xA: cpu_exception(10, pc0); cyc += 34; return;
    case 0xF: cpu_exception(11, pc0); cyc += 34; return;

    case 0xB: {
        int dr = (op >> 9) & 7, b8 = (op >> 8) & 1, sz = (op >> 6) & 3;
        if (sz == 3) {                      // CMPA
            int c2 = 0; u32 v = b8 == 0 ? sext_sz(ea_read<1>(mode, reg, c2), 1) : ea_read<2>(mode, reg, c2);
            alu_cmp<2>(v, cpu.A[dr]);
            cyc += 6 + c2;
            return;
        }
        if (b8 == 1 && mode == 1) {         // CMPM
            u32 a_s = cpu.A[reg], a_d = cpu.A[dr];
            cpu.A[reg] += (reg == 7 && sz == 0) ? 2 : (1 << sz);
            cpu.A[dr] += (dr == 7 && sz == 0) ? 2 : (1 << sz);
            if (sz == 0) { u32 s = mem_read8(a_s), d = mem_read8(a_d); alu_cmp<0>(s, d); }
            else if (sz == 1) { u32 s = mem_read16(a_s), d = mem_read16(a_d); alu_cmp<1>(s, d); }
            else { u32 s = mem_read32(a_s), d = mem_read32(a_d); alu_cmp<2>(s, d); }
            cyc += (sz == 2 ? 20 : 12);
            return;
        }
        if (b8 == 1) {                      // EOR
            int c2 = 0;
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); ea_write<0>(e, alu_logic<0>(d ^ (cpu.D[dr] & 0xFF))); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); ea_write<1>(e, alu_logic<1>(d ^ (cpu.D[dr] & 0xFFFF))); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); ea_write<2>(e, alu_logic<2>(d ^ cpu.D[dr])); }
            cyc += (sz == 2 ? 12 : 8) + c2;
            return;
        }
        int c2 = 0;                         // CMP
        if (sz == 0) { u32 s = ea_read<0>(mode, reg, c2); alu_cmp<0>(s, cpu.D[dr] & 0xFF); }
        else if (sz == 1) { u32 s = ea_read<1>(mode, reg, c2); alu_cmp<1>(s, cpu.D[dr] & 0xFFFF); }
        else { u32 s = ea_read<2>(mode, reg, c2); alu_cmp<2>(s, cpu.D[dr]); }
        cyc += (sz == 2 ? 6 : 4) + c2;
        return;
    }

    case 0xC: {
        int dr = (op >> 9) & 7, b8 = (op >> 8) & 1, sz = (op >> 6) & 3;
        if (sz == 3) {                      // MULU / MULS
            int c2 = 0; u32 s = ea_read<1>(mode, reg, c2);
            if (b8 == 0) {
                u32 r = (s & 0xFFFF) * (cpu.D[dr] & 0xFFFF);
                cpu.D[dr] = r; fl_nz<2>(r); cpu.V = 0; cpu.C = 0;
                cyc += 38 + 2 * popcount16(s) + c2;
            } else {
                s32 r = (s32)(s16)s * (s32)(s16)cpu.D[dr];
                cpu.D[dr] = (u32)r; fl_nz<2>((u32)r); cpu.V = 0; cpu.C = 0;
                cyc += 38 + 2 * popcount16(s ^ (s << 1)) + c2;
            }
            return;
        }
        if (b8 == 1) {
            if ((op & 0xF1F8) == 0xC100) {  // ABCD reg
                u32 s = cpu.D[reg] & 0xFF, d = cpu.D[dr] & 0xFF;
                cpu.D[dr] = (cpu.D[dr] & ~0xFFu) | bcd_add(s, d);
                cyc += 6; return;
            }
            if ((op & 0xF1F8) == 0xC108) {  // ABCD mem
                cpu.A[reg] -= (reg == 7) ? 2 : 1; u32 s = mem_read8(cpu.A[reg]);
                cpu.A[dr] -= (dr == 7) ? 2 : 1; u32 d = mem_read8(cpu.A[dr]);
                mem_write8(cpu.A[dr], (u8)bcd_add(s, d));
                cyc += 18; return;
            }
            if ((op & 0xF1F8) == 0xC140) { u32 t = cpu.D[dr]; cpu.D[dr] = cpu.D[reg]; cpu.D[reg] = t; cyc += 6; return; } // EXG D,D
            if ((op & 0xF1F8) == 0xC148) { u32 t = cpu.A[dr]; cpu.A[dr] = cpu.A[reg]; cpu.A[reg] = t; cyc += 6; return; } // EXG A,A
            if ((op & 0xF1F8) == 0xC188) { u32 t = cpu.D[dr]; cpu.D[dr] = cpu.A[reg]; cpu.A[reg] = t; cyc += 6; return; } // EXG D,A
        }
        int c2 = 0;                         // AND
        if (b8 == 0) {
            if (sz == 0) { u32 s = ea_read<0>(mode, reg, c2); cpu.D[dr] = (cpu.D[dr] & ~0xFFu) | alu_logic<0>(s & (cpu.D[dr] & 0xFF)); }
            else if (sz == 1) { u32 s = ea_read<1>(mode, reg, c2); cpu.D[dr] = (cpu.D[dr] & ~0xFFFFu) | alu_logic<1>(s & (cpu.D[dr] & 0xFFFF)); }
            else { u32 s = ea_read<2>(mode, reg, c2); cpu.D[dr] = alu_logic<2>(s & cpu.D[dr]); }
            cyc += (sz == 2 ? 6 : 4) + c2;
        } else {
            if (sz == 0) { EA e = ea_addr<0>(mode, reg, c2); u32 d = ea_value<0>(e); ea_write<0>(e, alu_logic<0>(d & (cpu.D[dr] & 0xFF))); }
            else if (sz == 1) { EA e = ea_addr<1>(mode, reg, c2); u32 d = ea_value<1>(e); ea_write<1>(e, alu_logic<1>(d & (cpu.D[dr] & 0xFFFF))); }
            else { EA e = ea_addr<2>(mode, reg, c2); u32 d = ea_value<2>(e); ea_write<2>(e, alu_logic<2>(d & cpu.D[dr])); }
            cyc += (sz == 2 ? 12 : 8) + c2;
        }
        return;
    }

    case 0xE: {
        if ((op & 0xC0) == 0xC0) {          // memory shifts (word)
            int kind = (op >> 9) & 3, dir = (op >> 8) & 1;
            int c2 = 0; EA e = ea_addr<1>(mode, reg, c2);
            u32 d = ea_value<1>(e), r = d;
            cpu.V = 0;
            if (kind == 0) {
                if (dir) { cpu.C = cpu.X = (d >> 15) & 1; r = (d << 1) & 0xFFFF; if ((d ^ r) & 0x8000) cpu.V = 1; }
                else { cpu.C = cpu.X = d & 1; r = (d >> 1) | (d & 0x8000); }
            } else if (kind == 1) {
                if (dir) { cpu.C = cpu.X = (d >> 15) & 1; r = (d << 1) & 0xFFFF; }
                else { cpu.C = cpu.X = d & 1; r = d >> 1; }
            } else if (kind == 2) {
                if (dir) { u32 ox = cpu.X; cpu.C = cpu.X = (d >> 15) & 1; r = ((d << 1) | ox) & 0xFFFF; }
                else { u32 ox = cpu.X; cpu.C = cpu.X = d & 1; r = (d >> 1) | (ox << 15); }
            } else {
                if (dir) { cpu.C = (d >> 15) & 1; r = ((d << 1) | cpu.C) & 0xFFFF; }
                else { cpu.C = d & 1; r = (d >> 1) | ((u32)cpu.C << 15); }
            }
            fl_nz<1>(r);
            ea_write<1>(e, r);
            cyc += c2 + 8;
            return;
        }
        // register shifts
        int sz = (op >> 6) & 3;
        if (sz == 3) { cpu_exception(4, pc0); cyc += 34; return; }
        int dir = (op >> 8) & 1, kind = (op >> 3) & 3, r2 = op & 7;
        u32 count = (op & 0x20) ? (cpu.D[(op >> 9) & 7] & 63) : (((op >> 9) & 7) == 0 ? 8 : ((op >> 9) & 7));
        u32 m = msk(sz), sg = sgnb(sz);
        u32 r = cpu.D[r2] & m;
        cpu.V = 0;
        if (count == 0) {
            cpu.C = 0;
            if (sz == 0) fl_nz<0>(r); else if (sz == 1) fl_nz<1>(r); else fl_nz<2>(r);
            cyc += 6;
            return;
        }
        if (kind == 0) {                    // ASL / ASR
            if (dir) {
                u32 orig = r & sg; int v = 0; u32 t = r;
                for (u32 i = 0; i < count; i++) { cpu.C = cpu.X = (u8)((r & sg) ? 1 : 0); r = (r << 1) & m; }
                for (u32 i = 0; i < count; i++) { t = (t << 1) & m; if ((t & sg) != orig) { v = 1; break; } }
                cpu.V = (u8)v;
            } else {
                for (u32 i = 0; i < count; i++) { cpu.C = cpu.X = (u8)(r & 1); r = (r >> 1) | (r & sg); }
                r &= m;
            }
        } else if (kind == 1) {             // LSL / LSR
            if (dir) { for (u32 i = 0; i < count; i++) { cpu.C = cpu.X = (u8)((r & sg) ? 1 : 0); r = (r << 1) & m; } }
            else { for (u32 i = 0; i < count; i++) { cpu.C = cpu.X = (u8)(r & 1); r >>= 1; } r &= m; }
        } else if (kind == 2) {             // ROXL / ROXR
            if (dir) { for (u32 i = 0; i < count; i++) { u32 ox = cpu.X; cpu.C = cpu.X = (u8)((r & sg) ? 1 : 0); r = ((r << 1) | ox) & m; } }
            else { for (u32 i = 0; i < count; i++) { u32 ox = cpu.X; cpu.C = cpu.X = (u8)(r & 1); r = (r >> 1) | (ox ? sg : 0); } }
            r &= m;
        } else {                            // ROL / ROR
            u32 bits = 8u << sz;
            count %= bits;
            if (dir) { for (u32 i = 0; i < count; i++) { u32 o = (r & sg) ? 1 : 0; cpu.C = (u8)o; r = ((r << 1) | o) & m; } }
            else { for (u32 i = 0; i < count; i++) { u32 o = r & 1; cpu.C = (u8)o; r = (r >> 1) | (o ? sg : 0); } }
            r &= m;
        }
        if (sz == 0) { fl_nz<0>(r); cpu.D[r2] = (cpu.D[r2] & ~0xFFu) | r; }
        else if (sz == 1) { fl_nz<1>(r); cpu.D[r2] = (cpu.D[r2] & ~0xFFFFu) | r; }
        else { fl_nz<2>(r); cpu.D[r2] = r; }
        cyc += 6 + 2 * count;
        return;
    }
    }
}
