# Validate kof98.dll: boot, step, save/load determinism, input, peek.
import ctypes, os, sys, hashlib

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DLL = os.path.join(ROOT, "build", "lib", "kof98.dll")
ROMS = os.path.join(ROOT, "roms")

lib = ctypes.CDLL(DLL)
lib.kof98_boot.argtypes = [ctypes.c_char_p, ctypes.c_uint]
lib.kof98_boot.restype = ctypes.c_int
lib.kof98_state_size.restype = ctypes.c_int
lib.kof98_state_save.argtypes = [ctypes.c_void_p]
lib.kof98_state_load.argtypes = [ctypes.c_void_p]
lib.kof98_set_input.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
lib.kof98_peek8.argtypes = [ctypes.c_uint32]
lib.kof98_peek8.restype = ctypes.c_uint8
lib.kof98_peek16.argtypes = [ctypes.c_uint32]
lib.kof98_peek16.restype = ctypes.c_uint16
lib.kof98_peek32.argtypes = [ctypes.c_uint32]
lib.kof98_peek32.restype = ctypes.c_uint32

rc = lib.kof98_boot(ROMS.encode(), 0)
assert rc == 0, f"boot failed rc={rc}"
print("boot ok")

sz = lib.kof98_state_size()
print(f"state size: {sz} bytes ({sz/1024:.1f} KB)")

def run(frames, p1=0, p2=0, start=0, coin=0):
    lib.kof98_set_input(p1, p2, start, coin)
    for _ in range(frames):
        lib.kof98_step_frame()

def wram_hash():
    h = hashlib.sha256()
    for a in range(0x100000, 0x110000, 4):
        h.update(lib.kof98_peek32(a).to_bytes(4, "little"))
    return h.hexdigest()[:16]

# boot to title, then coin+start like the earlier headless runs
run(300, coin=1)
run(8, coin=1)
run(112)
run(8, start=1)
run(100)
print("pc=%06x" % lib.kof98_peek32(4))

snap = ctypes.create_string_buffer(sz)
lib.kof98_state_save(snap)
print("state saved")

# run 300 frames with a button mashing pattern, record state
run(300, p1=0x10 | 0x08)  # A + right
h1 = wram_hash()
pc1 = lib.kof98_peek32(4)

# restore and replay the same inputs -> must be identical
lib.kof98_state_load(snap)
run(300, p1=0x10 | 0x08)
h2 = wram_hash()
pc2 = lib.kof98_peek32(4)

print(f"pass1: pc={pc1:08x} wram={h1}")
print(f"pass2: pc={pc2:08x} wram={h2}")
assert h1 == h2 and pc1 == pc2, "MISMATCH: save/load not deterministic"
print("PASS: save/load deterministic")

# different inputs after load -> should diverge (sanity check that inputs matter)
lib.kof98_state_load(snap)
run(300, p1=0)
h3 = wram_hash()
print(f"pass3 (no input): wram={h3}  (should differ: {h3 != h1})")
print("ALL OK")
