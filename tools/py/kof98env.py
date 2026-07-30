# KOF98 RL interface -- Python wrapper over the C API shared library.
#
# Usage (single instance):
#   env = Kof98("path/to/roms")
#   env.run(60, p1=Kof98.A | Kof98.RIGHT)
#   snap = env.save()
#   ...
#   env.load(snap)
#
# Multi-instance in one process (vector envs): the DLL holds one global
# machine, so logical instances are multiplexed via state swapping --
# see Kof98VecEnv. For multi-core, run one process per core (multiprocessing),
# each with its own Kof98VecEnv.
import ctypes, os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def _default_lib():
    import platform
    sysname = platform.system()
    if sysname == "Windows":
        return os.path.join(ROOT, "build", "lib", "kof98.dll")
    if sysname == "Darwin":
        arm = platform.machine() in ("arm64", "aarch64")
        name = "libkof98_arm64.dylib" if arm else "libkof98.dylib"
        return os.path.join(ROOT, "build", "lib", name)
    return os.path.join(ROOT, "build", "lib", "libkof98.so")

# buttons (1 = pressed), mirror of kof98_api.h
UP, DOWN, LEFT, RIGHT = 0x01, 0x02, 0x04, 0x08
A, B, C, D = 0x10, 0x20, 0x40, 0x80

# boot flags
F_VIDEO, F_AUDIO, F_ZINT = 1, 2, 4

class Kof98:
    def __init__(self, roms_dir, flags=0, lib_path=None):
        lib_path = lib_path or _default_lib()
        self.lib = ctypes.CDLL(lib_path)
        L = self.lib
        L.kof98_boot.argtypes = [ctypes.c_char_p, ctypes.c_uint]
        L.kof98_boot.restype = ctypes.c_int
        L.kof98_set_input.argtypes = [ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8]
        L.kof98_state_size.restype = ctypes.c_int
        L.kof98_state_save.argtypes = [ctypes.c_void_p]
        L.kof98_state_load.argtypes = [ctypes.c_void_p]
        L.kof98_peek8.argtypes = [ctypes.c_uint32]; L.kof98_peek8.restype = ctypes.c_uint8
        L.kof98_peek16.argtypes = [ctypes.c_uint32]; L.kof98_peek16.restype = ctypes.c_uint16
        L.kof98_peek32.argtypes = [ctypes.c_uint32]; L.kof98_peek32.restype = ctypes.c_uint32
        rc = L.kof98_boot(os.fsencode(roms_dir), flags)
        if rc != 0:
            raise RuntimeError(f"kof98_boot failed (rc={rc}), roms_dir={roms_dir}")
        self._size = L.kof98_state_size()
        self._p1 = self._p2 = self._start = self._coin = 0

    @property
    def state_size(self):
        return self._size

    def run(self, frames=1):
        L = self.lib
        L.kof98_set_input(self._p1, self._p2, self._start, self._coin)
        for _ in range(frames):
            L.kof98_step_frame()

    def set_input(self, p1=0, p2=0, start=0, coin=0):
        """Set buttons held for subsequent run() calls. 1 = pressed."""
        self._p1, self._p2, self._start, self._coin = p1, p2, start, coin

    def save(self):
        buf = ctypes.create_string_buffer(self._size)
        self.lib.kof98_state_save(buf)
        return buf

    def load(self, buf):
        self.lib.kof98_state_load(buf)

    def peek8(self, addr):  return self.lib.kof98_peek8(addr)
    def peek16(self, addr): return self.lib.kof98_peek16(addr)
    def peek32(self, addr): return self.lib.kof98_peek32(addr)

    # WRAM observation helpers (68k address space: WRAM at 0x100000..0x10FFFF)
    def wram8(self, off):  return self.peek8(0x100000 + off)
    def wram16(self, off): return self.peek16(0x100000 + off)
    def wram32(self, off): return self.peek32(0x100000 + off)

    def framebuffer(self):
        """320x224 pixels, u32 0x00RRGGBB each (bytes B,G,R,0 = pygame "BGRA").
        Valid only with F_VIDEO."""
        w = ctypes.c_int(); h = ctypes.c_int()
        self.lib.kof98_framebuffer.restype = ctypes.POINTER(ctypes.c_uint32)
        self.lib.kof98_framebuffer.argtypes = [ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int)]
        p = self.lib.kof98_framebuffer(ctypes.byref(w), ctypes.byref(h))
        return p, w.value, h.value

    def audio_drain(self, ptr, max_frames):
        """Drain up to max_frames stereo s16 sample pairs into ptr.
        Returns sample pairs written. Only with F_AUDIO (44100 Hz)."""
        self.lib.kof98_audio_drain.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        self.lib.kof98_audio_drain.restype = ctypes.c_uint32
        return self.lib.kof98_audio_drain(ptr, max_frames)

class Kof98VecEnv:
    """N logical instances multiplexed over one global machine via state
    swapping. Each step: load env i's state -> step -> save it back."""
    def __init__(self, roms_dir, n, flags=0, lib_path=None):
        self.core = Kof98(roms_dir, flags, lib_path)
        self.states = [self.core.save() for _ in range(n)]
        self.n = n

    def step_env(self, i, frames=1):
        self.core.load(self.states[i])
        self.core.run(frames)
        self.states[i] = self.core.save()

    def set_input(self, i, **kw):
        # inputs live inside the saved state, so apply then save via step
        self.core.load(self.states[i])
        self.core.set_input(**kw)
        self.states[i] = self.core.save()

if __name__ == "__main__":
    import time
    roms = os.path.join(ROOT, "roms")
    env = Kof98(roms)
    print(f"state size: {env.state_size/1024:.1f} KB")

    # attract-mode benchmark through the Python call path
    t0 = time.perf_counter()
    env.run(600)
    dt = time.perf_counter() - t0
    print(f"600 frames in {dt*1000:.0f} ms = {600/dt:.0f} fps (via ctypes)")

    # save/load round trip
    snap = env.save()
    env.run(120)
    env.load(snap)
    env.run(120)
    print("save/load ok")
