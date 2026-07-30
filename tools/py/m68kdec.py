# -*- coding: utf-8 -*-
"""
Motorola 68000 decoder: given a byte buffer and address, decode one instruction.
Returns an Instr with structured operands (for disassembly and C++ codegen).
"""
from dataclasses import dataclass, field

# EA modes
EA_DN, EA_AN, EA_IND, EA_POST, EA_PRE, EA_D16, EA_IDX, EA_EXT = range(8)
# sub-modes for EA_EXT.reg
X_ABSW, X_ABSL, X_PCD16, X_PCIDX, X_IMM = range(5)

SZ_B, SZ_W, SZ_L = 0, 1, 2
SZC = "bwl"

# flow types
F_FALL, F_BRA, F_BSR, F_BCC, F_DBCC, F_JMP, F_JSR, F_RTS, F_RTE, F_RTR, F_TRAP, F_STOP, F_ILL = range(13)

CC_NAMES = ["t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
            "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"]


@dataclass
class EA:
    mode: int
    reg: int
    sz: int
    ext: int = 0        # extension word (index modes)
    val: int = 0        # abs addr / displacement / immediate
    def text(self, pc_after_ext):
        m, r = self.mode, self.reg
        if m == EA_DN: return "d%d" % r
        if m == EA_AN: return "a%d" % r
        if m == EA_IND: return "(a%d)" % r
        if m == EA_POST: return "(a%d)+" % r
        if m == EA_PRE: return "-(a%d)" % r
        if m == EA_D16: return "($%04x,a%d)" % (self.val & 0xFFFF, r)
        if m == EA_IDX:
            xn = "a%d" % ((self.ext >> 12) & 7) if self.ext & 0x8000 else "d%d" % ((self.ext >> 12) & 7)
            wl = "l" if self.ext & 0x800 else "w"
            return "($%02x,a%d,%s.%s)" % (self.ext & 0xFF, r, xn, wl)
        if m == EA_EXT:
            if r == X_ABSW: return "($%04x).w" % (self.val & 0xFFFF)
            if r == X_ABSL: return "($%06x).l" % self.val
            if r == X_PCD16: return "($%04x,pc) ; ->$%06x" % (self.val & 0xFFFF, (pc_after_ext - 2 + s16(self.val)) & 0xFFFFFF)
            if r == X_PCIDX:
                xn = "a%d" % ((self.ext >> 12) & 7) if self.ext & 0x8000 else "d%d" % ((self.ext >> 12) & 7)
                wl = "l" if self.ext & 0x800 else "w"
                return "($%02x,pc,%s.%s)" % (self.ext & 0xFF, xn, wl)
            if r == X_IMM:
                if self.sz == SZ_L: return "#$%08x" % self.val
                return "#$%04x" % (self.val & 0xFFFF)
        return "?"


def s16(v): return v - 0x10000 if v & 0x8000 else v
def s8(v): return v - 0x100 if v & 0x80 else v


@dataclass
class Instr:
    addr: int
    op: int
    size: int               # bytes total (incl. extensions)
    mnem: str
    eas: list = field(default_factory=list)
    flow: int = F_FALL
    target: int = -1        # static branch target if known
    cond: int = -1          # condition code for bcc/dbcc/scc
    sz: int = -1
    text: str = ""
    ea_cycles: int = 0      # approx ea cycles (for codegen parity)
    base_cycles: int = 4

    def dis(self):
        if self.text: return self.text
        ops = ", ".join(e.text(self.addr + self.size) for e in self.eas)
        return "%s %s" % (self.mnem, ops) if ops else self.mnem


class Reader:
    def __init__(self, mem, addr):
        self.mem = mem
        self.addr = addr
        self.start = addr
    def w(self):
        v = (self.mem[self.addr] << 8) | self.mem[self.addr + 1]
        self.addr += 2
        return v
    def l(self):
        v = (self.w() << 16) | self.w()
        return v


def dec_ea(r, mode, reg, sz):
    """decode one EA, consuming extension words"""
    if mode <= 4:
        return EA(mode, reg, sz)
    if mode == 5:
        return EA(EA_D16, reg, sz, val=r.w())
    if mode == 6:
        return EA(EA_IDX, reg, sz, ext=r.w())
    if mode == 7:
        if reg == 0: return EA(EA_EXT, X_ABSW, sz, val=s16(r.w()) & 0xFFFFFFFF if False else r.w())
        if reg == 1: return EA(EA_EXT, X_ABSL, sz, val=r.l())
        if reg == 2: return EA(EA_EXT, X_PCD16, sz, val=r.w())
        if reg == 3: return EA(EA_EXT, X_PCIDX, sz, ext=r.w())
        if reg == 4:
            if sz == SZ_B: return EA(EA_EXT, X_IMM, sz, val=r.w() & 0xFF)
            if sz == SZ_W: return EA(EA_EXT, X_IMM, sz, val=r.w())
            return EA(EA_EXT, X_IMM, sz, val=r.l())
    return EA(EA_EXT, X_ABSW, sz, val=0)   # invalid EA placeholder


def decode(mem, addr):
    r = Reader(mem, addr)
    op = r.w()
    ins = Instr(addr, op, 0, "???")
    top = op >> 12
    mode, reg = (op >> 3) & 7, op & 7

    def fin():
        ins.size = r.addr - addr
        return ins

    if top in (0x1, 0x2, 0x3):
        sz = SZ_B if top == 1 else SZ_W if top == 3 else SZ_L
        dstm, dstr = (op >> 6) & 7, (op >> 9) & 7
        se = dec_ea(r, mode, reg, sz)
        de = dec_ea(r, dstm, dstr, sz)
        ins.sz = sz
        ins.mnem = "movea." + SZC[sz] if dstm == 1 else "move." + SZC[sz]
        ins.eas = [se, de]
        return fin()

    if top == 0x0:
        if (op & 0x0138) == 0x0108:     # MOVEP
            dr = (op >> 9) & 7
            szl = (op >> 6) & 1
            dire = (op >> 7) & 1
            disp = r.w()
            ins.mnem = "movep." + ("l" if szl else "w")
            ea = EA(EA_D16, reg, SZ_L, val=disp)
            dn = EA(EA_DN, dr, SZ_L)
            ins.eas = [dn, ea] if dire else [ea, dn]
            return fin()
        if (op & 0x0F00) == 0x0800:     # static bit
            which = (op >> 6) & 3
            imm = r.w()
            ins.mnem = ["btst", "bchg", "bclr", "bset"][which]
            ins.eas = [EA(EA_EXT, X_IMM, SZ_W, val=imm), dec_ea(r, mode, reg, SZ_B)]
            return fin()
        if op & 0x0100:                 # dynamic bit
            which = (op >> 6) & 3
            ins.mnem = ["btst", "bchg", "bclr", "bset"][which]
            ins.eas = [EA(EA_DN, (op >> 9) & 7, SZ_L), dec_ea(r, mode, reg, SZ_B)]
            return fin()
        opc = (op >> 9) & 7
        sz = (op >> 6) & 3
        names = {0: "ori", 1: "andi", 2: "subi", 3: "addi", 5: "eori", 6: "cmpi"}
        if sz == 3 or opc not in names:
            ins.mnem = "illegal"; ins.flow = F_ILL
            return fin()
        if sz == SZ_B: imm = r.w() & 0xFF
        elif sz == SZ_W: imm = r.w()
        else: imm = r.l()
        if (op & 0x3F) == 0x3C and opc in (0, 1, 5):
            ins.mnem = names[opc] + (".w" if sz else ".b")
            ins.eas = [EA(EA_EXT, X_IMM, sz, val=imm)]
            ins.text = "%s #$%x, %s" % (ins.mnem, imm, "sr" if sz else "ccr")
            ins.mnem = names[opc] + "_" + ("sr" if sz else "ccr")
            ins.eas = []
            return fin()
        ins.mnem = names[opc] + "." + SZC[sz]
        ins.sz = sz
        ins.eas = [EA(EA_EXT, X_IMM, sz, val=imm), dec_ea(r, mode, reg, sz)]
        return fin()

    if top == 0x4:
        if (op & 0xFFF0) == 0x4E40:
            ins.mnem = "trap"; ins.flow = F_TRAP; ins.text = "trap #%d" % (op & 0xF)
            return fin()
        exact = {0x4E70: "reset", 0x4E71: "nop", 0x4E72: "stop", 0x4E73: "rte",
                 0x4E75: "rts", 0x4E76: "trapv", 0x4E77: "rtr"}
        if op in exact:
            ins.mnem = exact[op]
            if op == 0x4E72: ins.flow = F_STOP; ins.eas = [EA(EA_EXT, X_IMM, SZ_W, val=r.w())]
            if op == 0x4E73: ins.flow = F_RTE
            if op == 0x4E75: ins.flow = F_RTS
            if op == 0x4E77: ins.flow = F_RTR
            if op == 0x4E76: ins.flow = F_TRAP
            return fin()
        if (op & 0xFFF8) == 0x4E50:
            disp = r.w()
            ins.mnem = "link"; ins.eas = [EA(EA_AN, reg, SZ_L), EA(EA_EXT, X_IMM, SZ_W, val=disp)]
            return fin()
        if (op & 0xFFF8) == 0x4E58:
            ins.mnem = "unlk"; ins.eas = [EA(EA_AN, reg, SZ_L)]
            return fin()
        if (op & 0xFFF0) == 0x4E60:
            ins.mnem = "move usp" + ("->a" if (op & 8) else "a->")
            return fin()
        m = op & 0xFFC0
        if m == 0x4E80 or m == 0x4EC0:
            ea = dec_ea(r, mode, reg, SZ_W)
            ins.mnem = "jsr" if m == 0x4E80 else "jmp"
            ins.flow = F_JSR if m == 0x4E80 else F_JMP
            ins.eas = [ea]
            if ea.mode == EA_EXT and ea.reg == X_ABSW: ins.target = s16(ea.val) & 0xFFFFFF if False else ea.val
            if ea.mode == EA_EXT and ea.reg == X_ABSL: ins.target = ea.val
            return fin()
        m8 = op & 0xFF00
        simple = {0x4000: "negx", 0x4200: "clr", 0x4400: "neg", 0x4600: "not", 0x4A00: "tst"}
        if m8 in simple and (op & 0xC0) != 0xC0:
            sz = (op >> 6) & 3
            ins.mnem = simple[m8] + "." + SZC[sz]
            ins.sz = sz
            ins.eas = [dec_ea(r, mode, reg, sz)]
            return fin()
        if m == 0x40C0:
            ins.mnem = "move sr->ea"; ins.eas = [dec_ea(r, mode, reg, SZ_W)]; return fin()
        if m == 0x44C0:
            ins.mnem = "move ea->ccr"; ins.eas = [dec_ea(r, mode, reg, SZ_W)]; return fin()
        if m == 0x46C0:
            ins.mnem = "move ea->sr"; ins.eas = [dec_ea(r, mode, reg, SZ_W)]; return fin()
        if m == 0x4800:
            ins.mnem = "nbcd"; ins.eas = [dec_ea(r, mode, reg, SZ_B)]; return fin()
        if (op & 0xFFF8) == 0x4840:
            ins.mnem = "swap"; ins.eas = [EA(EA_DN, reg, SZ_L)]; return fin()
        if m == 0x4840:
            ins.mnem = "pea"; ins.eas = [dec_ea(r, mode, reg, SZ_L)]
            return fin()
        if (op & 0xFFF8) == 0x4880:
            ins.mnem = "ext.w"; ins.eas = [EA(EA_DN, reg, SZ_W)]; return fin()
        if (op & 0xFFF8) == 0x48C0:
            ins.mnem = "ext.l"; ins.eas = [EA(EA_DN, reg, SZ_L)]; return fin()
        if (op & 0xFB80) == 0x4880:         # MOVEM
            dire = (op >> 10) & 1
            szl = (op >> 6) & 1
            msk = r.w()
            ins.mnem = "movem." + ("l" if szl else "w")
            ins.sz = szl
            ea = dec_ea(r, mode, reg, SZ_W)
            regs = [i for i in range(16) if msk & (1 << i)]
            ins.eas = [ea]
            ins.target = msk   # reuse: register mask
            ins.cond = dire
            ins.text = "%s $%04x, %s" % (ins.mnem, msk, ea.text(0))
            return fin()
        if (op & 0xF1C0) == 0x4180:
            ins.mnem = "chk"; ins.eas = [dec_ea(r, mode, reg, SZ_W), EA(EA_DN, (op >> 9) & 7, SZ_W)]
            return fin()
        if (op & 0xF1C0) == 0x41C0:
            ins.mnem = "lea"; ins.eas = [dec_ea(r, mode, reg, SZ_L), EA(EA_AN, (op >> 9) & 7, SZ_L)]
            return fin()
        if m == 0x4AC0:
            ins.mnem = "tas"; ins.eas = [dec_ea(r, mode, reg, SZ_B)]; return fin()
        ins.mnem = "illegal"; ins.flow = F_ILL
        return fin()

    if top == 0x5:
        if (op & 0xC0) == 0xC0:
            cc = (op >> 8) & 0xF
            ins.cond = cc
            if op & 0x0100:                 # DBcc
                disp = r.w()
                ins.mnem = "db" + CC_NAMES[cc]
                ins.eas = [EA(EA_DN, reg, SZ_W)]
                ins.flow = F_DBCC
                ins.target = (addr + 2 + s16(disp)) & 0xFFFFFF
            else:
                ins.mnem = "s" + CC_NAMES[cc]
                ins.eas = [dec_ea(r, mode, reg, SZ_B)]
            return fin()
        sz = (op >> 6) & 3
        if sz == 3:
            ins.mnem = "illegal"; ins.flow = F_ILL
            return fin()
        q = (op >> 9) & 7 or 8
        ins.mnem = ("subq." if (op >> 8) & 1 else "addq.") + SZC[sz]
        ins.sz = sz
        ins.eas = [EA(EA_EXT, X_IMM, sz, val=q), dec_ea(r, mode, reg, sz)]
        return fin()

    if top == 0x6:
        cc = (op >> 8) & 0xF
        if (op & 0xFF) == 0:
            disp = s16(r.w())
        else:
            disp = s8(op & 0xFF)
        tgt = (addr + 2 + disp) & 0xFFFFFF
        ins.cond = cc
        if cc == 0:
            ins.mnem = "bra"; ins.flow = F_BRA
        elif cc == 1:
            ins.mnem = "bsr"; ins.flow = F_BSR
        else:
            ins.mnem = "b" + CC_NAMES[cc]; ins.flow = F_BCC
        ins.target = tgt
        ins.eas = []
        ins.text = "%s $%06x" % (ins.mnem, tgt)
        return fin()

    if top == 0x7:
        v = op & 0xFF
        ins.mnem = "moveq"
        ins.eas = [EA(EA_EXT, X_IMM, SZ_L, val=v), EA(EA_DN, (op >> 9) & 7, SZ_L)]
        ins.text = "moveq #$%02x, d%d" % (v, (op >> 9) & 7)
        return fin()

    if top in (0x8, 0x9, 0xB, 0xC, 0xD):
        dr = (op >> 9) & 7
        b8 = (op >> 8) & 1
        sz = (op >> 6) & 3
        if top == 0x8:
            if sz == 3:
                ins.mnem = "divs" if b8 else "divu"
                ins.eas = [dec_ea(r, mode, reg, SZ_W), EA(EA_DN, dr, SZ_L)]
                return fin()
            if b8 and (op & 0x00F8) in (0x0000, 0x0008):
                ins.mnem = "sbcd"
                mem = (op & 8) != 0
                ins.eas = [EA(EA_PRE if mem else EA_DN, reg, SZ_B), EA(EA_PRE if mem else EA_DN, dr, SZ_B)]
                return fin()
            ins.mnem = "or." + SZC[sz]; ins.sz = sz
            ins.eas = [dec_ea(r, mode, reg, sz), EA(EA_DN, dr, sz)] if not b8 else [EA(EA_DN, dr, sz), dec_ea(r, mode, reg, sz)]
            return fin()
        if top == 0xC:
            if sz == 3:
                ins.mnem = "muls" if b8 else "mulu"
                ins.eas = [dec_ea(r, mode, reg, SZ_W), EA(EA_DN, dr, SZ_L)]
                return fin()
            if b8:
                if (op & 0xF1F8) == 0xC100:
                    ins.mnem = "abcd"; ins.eas = [EA(EA_DN, reg, SZ_B), EA(EA_DN, dr, SZ_B)]
                    return fin()
                if (op & 0xF1F8) == 0xC108:
                    ins.mnem = "abcd"; ins.eas = [EA(EA_PRE, reg, SZ_B), EA(EA_PRE, dr, SZ_B)]
                    return fin()
                if (op & 0xF1F8) in (0xC140, 0xC148, 0xC188):
                    ins.mnem = "exg"
                    kind = op & 0xF1F8
                    if kind == 0xC140: ins.eas = [EA(EA_DN, dr, SZ_L), EA(EA_DN, reg, SZ_L)]
                    elif kind == 0xC148: ins.eas = [EA(EA_AN, dr, SZ_L), EA(EA_AN, reg, SZ_L)]
                    else: ins.eas = [EA(EA_DN, dr, SZ_L), EA(EA_AN, reg, SZ_L)]
                    return fin()
            ins.mnem = "and." + SZC[sz]; ins.sz = sz
            ins.eas = [dec_ea(r, mode, reg, sz), EA(EA_DN, dr, sz)] if not b8 else [EA(EA_DN, dr, sz), dec_ea(r, mode, reg, sz)]
            return fin()
        # 0x9 sub / 0xD add / 0xB cmp
        base = {0x9: "sub", 0xD: "add", 0xB: "cmp"}[top]
        if sz == 3:
            ins.mnem = base + "a." + ("w" if not b8 else "l")
            ins.sz = SZ_W if not b8 else SZ_L
            ins.eas = [dec_ea(r, mode, reg, ins.sz), EA(EA_AN, dr, SZ_L)]
            return fin()
        if top in (0x9, 0xD) and b8 and (mode == 0 or mode == 4):
            ins.mnem = base + "x." + SZC[sz]; ins.sz = sz
            if mode == 0:
                ins.eas = [EA(EA_DN, reg, sz), EA(EA_DN, dr, sz)]
            else:
                ins.eas = [EA(EA_PRE, reg, sz), EA(EA_PRE, dr, sz)]
            return fin()
        if top == 0xB and b8 and mode == 1:
            ins.mnem = "cmpm." + SZC[sz]; ins.sz = sz
            ins.eas = [EA(EA_POST, reg, sz), EA(EA_POST, dr, sz)]
            return fin()
        if top == 0xB and b8:
            ins.mnem = "eor." + SZC[sz]; ins.sz = sz
            ins.eas = [EA(EA_DN, dr, sz), dec_ea(r, mode, reg, sz)]
            return fin()
        ins.mnem = base + "." + SZC[sz]; ins.sz = sz
        ins.eas = [dec_ea(r, mode, reg, sz), EA(EA_DN, dr, sz)] if not b8 else [EA(EA_DN, dr, sz), dec_ea(r, mode, reg, sz)]
        return fin()

    if top == 0xE:
        if (op & 0xC0) == 0xC0:
            kind = (op >> 9) & 3
            dire = (op >> 8) & 1
            ins.mnem = ["as", "ls", "rox", "ro"][kind] + ("l" if dire else "r") + ".w"
            ins.eas = [dec_ea(r, mode, reg, SZ_W)]
            return fin()
        sz = (op >> 6) & 3
        if sz == 3:
            ins.mnem = "illegal"; ins.flow = F_ILL
            return fin()
        dire = (op >> 8) & 1
        kind = (op >> 3) & 3
        byreg = (op >> 5) & 1
        cnt = (op >> 9) & 7
        ins.mnem = ["as", "ls", "rox", "ro"][kind] + ("l" if dire else "r") + "." + SZC[sz]
        ins.sz = sz
        src = EA(EA_DN, cnt, SZ_L) if byreg else EA(EA_EXT, X_IMM, SZ_B, val=cnt or 8)
        ins.eas = [src, EA(EA_DN, reg, sz)]
        ins.cond = byreg
        return fin()

    if top == 0xA or top == 0xF:
        ins.mnem = "line_%x" % top; ins.flow = F_TRAP
        return fin()

    ins.mnem = "illegal"; ins.flow = F_ILL
    return fin()
