// Shared interpreter internals (used by cpu_interp.cpp / cpu_interp2.cpp)
#pragma once
#include "rt.h"

extern u32 g_usp, g_ssp;

inline void set_S(int s) {
    if (s != cpu.S) {
        if (cpu.S) g_ssp = cpu.A[7]; else g_usp = cpu.A[7];
        cpu.A[7] = s ? g_ssp : g_usp;
        cpu.S = (u8)s;
    }
}
inline void cpu_after_sr_change() {
    set_S(cpu.S);
    // iml may have dropped below a pending level: stop translated code ASAP
    if (g_irq_level && (g_irq_level > cpu.iml || g_irq_level == 7)) g_stop_cyc = 0;
}

inline void cpu_exception(int vec, u32 push_pc) {
    if (getenv("KOF98_EXLOG"))
        fprintf(stderr, "EXC vec=%d pc_push=%06x a7=%06x cyc=%llu\n", vec, push_pc, cpu.A[7], (unsigned long long)cpu.cyc);
    u16 sr = cpu_get_sr();
    set_S(1); cpu.T = 0;
    mem_write32(cpu.A[7] - 4, push_pc);
    mem_write16(cpu.A[7] - 6, sr);
    cpu.A[7] -= 6;
    cpu.pc = mem_read32(vec * 4);
    cpu.stopped = 0;
}

inline u16 nextw() { u16 v = mem_read16(cpu.pc); cpu.pc += 2; return v; }
inline u32 nextl() { u32 v = mem_read32(cpu.pc); cpu.pc += 4; return v; }

inline u32 msk(int sz) { return sz == 0 ? 0xFFu : sz == 1 ? 0xFFFFu : 0xFFFFFFFFu; }
inline u32 sgnb(int sz) { return sz == 0 ? 0x80u : sz == 1 ? 0x8000u : 0x80000000u; }
inline u32 sext_sz(u32 v, int sz) {
    if (sz == 0) return (u32)(s32)(s8)v;
    if (sz == 1) return (u32)(s32)(s16)v;
    return v;
}

// ---- EA ----
template<int SZ>
inline u32 ea_read(int mode, int reg, int &cyc) {
    u32 a;
    switch (mode) {
    case 0: return cpu.D[reg] & msk(SZ);
    case 1: return cpu.A[reg] & msk(SZ);
    case 2: a = cpu.A[reg]; cyc += (SZ == 2 ? 8 : 4);
        return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
    case 3: {
        a = cpu.A[reg];
        cpu.A[reg] += (reg == 7 && SZ == 0) ? 2 : (1 << SZ);
        cyc += (SZ == 2 ? 8 : 4);
        return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
    }
    case 4: {
        cpu.A[reg] -= (reg == 7 && SZ == 0) ? 2 : (1 << SZ);
        a = cpu.A[reg]; cyc += (SZ == 2 ? 10 : 6);
        return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
    }
    case 5: a = cpu.A[reg] + (u32)(s32)(s16)nextw(); cyc += (SZ == 2 ? 12 : 8);
        return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
    case 6: {
        u16 ext = nextw();
        u32 xn = (ext & 0x8000) ? cpu.A[(ext >> 12) & 7] : cpu.D[(ext >> 12) & 7];
        if (!(ext & 0x800)) xn = (u32)(s32)(s16)xn;
        a = cpu.A[reg] + (u32)(s32)(s8)ext + xn; cyc += (SZ == 2 ? 14 : 10);
        return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
    }
    case 7:
        switch (reg) {
        case 0: a = (u32)(s32)(s16)nextw(); cyc += (SZ == 2 ? 12 : 8);
            return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
        case 1: a = nextl(); cyc += (SZ == 2 ? 16 : 12);
            return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
        case 2: { u32 base = cpu.pc; a = base + (u32)(s32)(s16)nextw(); cyc += (SZ == 2 ? 12 : 8);
            return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a); }
        case 3: {
            u32 base = cpu.pc;
            u16 ext = nextw();
            u32 xn = (ext & 0x8000) ? cpu.A[(ext >> 12) & 7] : cpu.D[(ext >> 12) & 7];
            if (!(ext & 0x800)) xn = (u32)(s32)(s16)xn;
            a = base + (u32)(s32)(s8)ext + xn; cyc += (SZ == 2 ? 14 : 10);
            return SZ == 0 ? mem_read8(a) : SZ == 1 ? mem_read16(a) : mem_read32(a);
        }
        case 4: cyc += (SZ == 2 ? 8 : 4);
            return SZ == 0 ? nextw() & 0xFF : SZ == 1 ? nextw() : nextl();
        }
    }
    return 0;
}

struct EA { int is_reg; int reg; u32 addr; };

template<int SZ>
inline EA ea_addr(int mode, int reg, int &cyc) {
    EA e; u16 ext; u32 xn;
    switch (mode) {
    case 0: e.is_reg = 0; e.reg = reg; e.addr = 0; return e;
    case 1: e.is_reg = 1; e.reg = reg; e.addr = 0; return e;
    case 2: e.is_reg = 2; e.addr = cpu.A[reg]; cyc += (SZ == 2 ? 8 : 4); return e;
    case 3:
        e.is_reg = 2; e.addr = cpu.A[reg];
        cpu.A[reg] += (reg == 7 && SZ == 0) ? 2 : (1 << SZ);
        cyc += (SZ == 2 ? 8 : 4); return e;
    case 4:
        cpu.A[reg] -= (reg == 7 && SZ == 0) ? 2 : (1 << SZ);
        e.is_reg = 2; e.addr = cpu.A[reg]; cyc += (SZ == 2 ? 10 : 6); return e;
    case 5:
        e.is_reg = 2; e.addr = cpu.A[reg] + (u32)(s32)(s16)nextw(); cyc += (SZ == 2 ? 12 : 8); return e;
    case 6:
        ext = nextw();
        xn = (ext & 0x8000) ? cpu.A[(ext >> 12) & 7] : cpu.D[(ext >> 12) & 7];
        if (!(ext & 0x800)) xn = (u32)(s32)(s16)xn;
        e.is_reg = 2; e.addr = cpu.A[reg] + (u32)(s32)(s8)ext + xn; cyc += (SZ == 2 ? 14 : 10); return e;
    case 7:
        e.is_reg = 2;
        switch (reg) {
        case 0: e.addr = (u32)(s32)(s16)nextw(); cyc += (SZ == 2 ? 12 : 8); return e;
        case 1: e.addr = nextl(); cyc += (SZ == 2 ? 16 : 12); return e;
        case 2: { u32 base = cpu.pc; e.addr = base + (u32)(s32)(s16)nextw(); cyc += (SZ == 2 ? 12 : 8); return e; }
        case 3: {
            u32 base = cpu.pc;
            ext = nextw();
            xn = (ext & 0x8000) ? cpu.A[(ext >> 12) & 7] : cpu.D[(ext >> 12) & 7];
            if (!(ext & 0x800)) xn = (u32)(s32)(s16)xn;
            e.addr = base + (u32)(s32)(s8)ext + xn; cyc += (SZ == 2 ? 14 : 10); return e;
        }
        }
    }
    e.is_reg = 2; e.addr = 0; return e;
}

template<int SZ>
inline u32 ea_value(const EA &e) {
    if (e.is_reg == 0) return cpu.D[e.reg] & msk(SZ);
    if (e.is_reg == 1) return cpu.A[e.reg] & msk(SZ);
    return SZ == 0 ? mem_read8(e.addr) : SZ == 1 ? mem_read16(e.addr) : mem_read32(e.addr);
}

template<int SZ>
inline void ea_write(const EA &e, u32 v) {
    v &= msk(SZ);
    if (e.is_reg == 0) {
        if (SZ == 0) cpu.D[e.reg] = (cpu.D[e.reg] & ~0xFFu) | v;
        else if (SZ == 1) cpu.D[e.reg] = (cpu.D[e.reg] & ~0xFFFFu) | v;
        else cpu.D[e.reg] = v;
        return;
    }
    if (e.is_reg == 1) { cpu.A[e.reg] = v; return; }
    if (SZ == 0) mem_write8(e.addr, (u8)v);
    else if (SZ == 1) mem_write16(e.addr, (u16)v);
    else mem_write32(e.addr, v);
}

// ---- flag/ALU semantics ----
template<int SZ> inline void fl_nz(u32 r) {
    r &= msk(SZ); cpu.N = (r & sgnb(SZ)) != 0; cpu.Z = (r == 0);
}
template<int SZ> inline u32 alu_add(u32 s, u32 d) {
    u64 full = (u64)s + (d & msk(SZ));
    u32 r = (u32)full & msk(SZ);
    cpu.C = cpu.X = (u8)((full >> (8 << SZ)) & 1);
    cpu.V = ((s ^ r) & (d ^ r) & sgnb(SZ)) != 0;
    fl_nz<SZ>(r);
    return r;
}
template<int SZ> inline u32 alu_addx(u32 s, u32 d) {
    u64 full = (u64)s + (d & msk(SZ)) + cpu.X;
    u32 r = (u32)full & msk(SZ);
    cpu.C = cpu.X = (u8)((full >> (8 << SZ)) & 1);
    cpu.V = ((s ^ r) & (d ^ r) & sgnb(SZ)) != 0;
    cpu.N = (r & sgnb(SZ)) != 0; cpu.Z &= (r == 0);
    return r;
}
template<int SZ> inline u32 alu_sub(u32 s, u32 d) {
    s &= msk(SZ); d &= msk(SZ);
    u32 r = (d - s) & msk(SZ);
    cpu.C = cpu.X = s > d;
    cpu.V = ((d ^ s) & (d ^ r) & sgnb(SZ)) != 0;
    fl_nz<SZ>(r);
    return r;
}
template<int SZ> inline u32 alu_subx(u32 s, u32 d) {
    s &= msk(SZ); d &= msk(SZ);
    u32 sx = s + cpu.X;
    u32 r = (d - sx) & msk(SZ);
    cpu.C = cpu.X = sx > d;
    cpu.V = ((d ^ s) & (d ^ r) & sgnb(SZ)) != 0;
    cpu.N = (r & sgnb(SZ)) != 0; cpu.Z &= (r == 0);
    return r;
}
template<int SZ> inline u32 alu_cmp(u32 s, u32 d) {
    u8 x = cpu.X; u32 r = alu_sub<SZ>(s, d); cpu.X = x; return r;
}
template<int SZ> inline u32 alu_logic(u32 r) {
    r &= msk(SZ); cpu.V = 0; cpu.C = 0; fl_nz<SZ>(r); return r;
}

inline u32 bcd_add(u32 s, u32 d) {
    u32 r = (s & 0x0F) + (d & 0x0F) + cpu.X;
    if (r > 9) r += 6;
    r += (s & 0xF0) + (d & 0xF0);
    cpu.V = (~(d ^ s) & (d ^ r) & 0x80) != 0;
    if (r > 0x99) { r += 0x60; cpu.C = cpu.X = 1; } else cpu.C = cpu.X = 0;
    r &= 0xFF;
    cpu.N = (r & 0x80) != 0; cpu.Z &= (r == 0);
    return r;
}
inline u32 bcd_sub(u32 s, u32 d) {
    u32 r = (d & 0xFF) - (s & 0xFF) - cpu.X;
    cpu.V = ((d ^ s) & (d ^ r) & 0x80) != 0;
    if ((d ^ s ^ r) & 0x10) r -= 6;
    if (r & 0x100) { r -= 0x60; cpu.C = cpu.X = 1; } else cpu.C = cpu.X = 0;
    r &= 0xFF;
    cpu.N = (r & 0x80) != 0; cpu.Z &= (r == 0);
    return r;
}

inline int cond_true(int cc) {
    switch (cc) {
    case 0: return 1; case 1: return 0;
    case 2: return !cpu.C && !cpu.Z;
    case 3: return cpu.C || cpu.Z;
    case 4: return !cpu.C; case 5: return cpu.C;
    case 6: return !cpu.Z; case 7: return cpu.Z;
    case 8: return !cpu.V; case 9: return cpu.V;
    case 10: return !cpu.N; case 11: return cpu.N;
    case 12: return cpu.N == cpu.V; case 13: return cpu.N != cpu.V;
    case 14: return (cpu.N == cpu.V) && !cpu.Z;
    case 15: return (cpu.N != cpu.V) || cpu.Z;
    }
    return 0;
}

inline int popcount16(u32 v) {
    v &= 0xFFFF; v = v - ((v >> 1) & 0x5555); v = (v & 0x3333) + ((v >> 2) & 0x3333);
    return (int)(((v + (v >> 4)) & 0x0F0F) * 0x0101 >> 8);
}

void cpu_interp_hi(u16 op, u32 pc0, int &cyc);   // cases 0x8..0xE (cpu_interp2.cpp)
