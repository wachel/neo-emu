# -*- coding: utf-8 -*-
"""Quick disassembler CLI: python disasm.py <rom.bin> [--base 0x0] --start 0x.. --count N"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m68kdec import decode, F_BRA, F_BSR, F_BCC, F_DBCC, F_JMP, F_RTS, F_RTE, F_RTR, F_TRAP, F_STOP


def main():
    args = sys.argv[1:]
    path = args[0]
    base = 0
    start = None
    count = 40
    i = 1
    while i < len(args):
        if args[i] == "--base": base = int(args[i + 1], 0); i += 2
        elif args[i] == "--start": start = int(args[i + 1], 0); i += 2
        elif args[i] == "--count": count = int(args[i + 1], 0); i += 2
        else: i += 1
    mem = open(path, "rb").read()
    if start is None: start = base
    a = start
    for _ in range(count):
        if a - base < 0 or a - base + 1 >= len(mem):
            print("%06x: <out of range>" % a)
            break
        ins = decode(mem, a - base)
        raw = " ".join("%02x%02x" % (mem[a - base + k], mem[a - base + k + 1]) for k in range(0, ins.size, 2))
        print("%06x: %-24s %s" % (a, raw, ins.dis()))
        a += ins.size


if __name__ == "__main__":
    main()
