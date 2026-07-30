# KOF98 training-result viewer: drives the RL library with video on and
# shows the framebuffer in a window. Cross-platform (needs: pip install pygame).
#
# Modes:
#   python play_agent.py                -- play with keyboard
#   python play_agent.py --fast         -- uncapped speed
#   python play_agent.py --snap fight.bin --frames 600
#                                       -- load snapshot, run N frames idle
#
# To watch YOUR AGENT: subclass or edit agent_input() below -- it is called
# every frame and returns (p1, p2, start, coin) button bits.
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from kof98env import Kof98, F_VIDEO, F_AUDIO, F_ZINT, UP, DOWN, LEFT, RIGHT, A, B, C, D

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

def agent_input(env, frame):
    """Override point for a trained policy. Return (p1, p2, start, coin).
    Example: read game state via env.wram8/16/32, feed to your model."""
    return None     # None = use keyboard

class SdlAudioOut:
    """Continuous s16-stereo playback driven by SDL's audio thread (the same
    pull-model as the exe's waveOut buffers): SDL calls _cb at the hardware
    rate, we keep it fed from a circular byte buffer. Frame-time jitter is
    absorbed by the buffer instead of crackling."""
    def __init__(self, rate=44100, seconds=2):
        import threading
        from pygame._sdl2.audio import AudioDevice, AUDIO_S16LSB, AUDIO_S16MSB, get_audio_device_names
        self.rate = rate
        self.cap = rate * 4 * seconds        # bytes (s16 stereo = 4 B/frame)
        self.buf = bytearray(self.cap)
        self.r = self.w = 0
        self.lock = threading.Lock()
        self.underruns = self.drops = 0      # smoothness stats (want zeros)
        fmt = AUDIO_S16LSB if sys.byteorder == "little" else AUDIO_S16MSB
        self.dev = AudioDevice(get_audio_device_names(False)[0], False,
                               rate, fmt, 2, 1024, 0, self._cb)
        # stays paused until start(): caller primes the buffer first

    def start(self):
        self.dev.pause(0)

    def avail_bytes(self):
        return self.w - self.r

    def push(self, data):
        with self.lock:
            n = len(data)
            room = self.cap - self.avail_bytes()
            if n > room:                     # way ahead of playback: drop tail
                self.drops += n - room
                n = room
            if n <= 0:
                return
            i = self.w % self.cap
            k = min(n, self.cap - i)
            self.buf[i:i+k] = data[:k]
            self.buf[:n-k] = data[k:]
            self.w += n

    def _cb(self, dev, mv):                  # called on SDL's audio thread
        with self.lock:
            n = len(mv)
            a = min(n, self.avail_bytes())
            if a:
                i = self.r % self.cap
                k = min(a, self.cap - i)
                mv[:k] = self.buf[i:i+k]
                mv[k:a] = self.buf[:a-k]
                self.r += a
            if a < n:                        # underrun: pad silence
                mv[a:] = b"\x00" * (n - a)
                self.underruns += 1

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--roms", default=os.path.join(ROOT, "roms"))
    ap.add_argument("--snap", help="state file to load at start (raw 215KB blob)")
    ap.add_argument("--frames", type=int, default=0, help="exit after N frames")
    ap.add_argument("--fast", action="store_true", help="no 60fps cap")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--mute", action="store_true", help="no audio (also faster)")
    args = ap.parse_args()

    import pygame
    import numpy as np
    flags = F_VIDEO if args.mute else (F_VIDEO | F_AUDIO | F_ZINT)
    env = Kof98(args.roms, flags=flags)
    if args.snap:
        snap = open(args.snap, "rb").read()
        buf = __import__("ctypes").create_string_buffer(snap, len(snap))
        env.load(buf)
        print(f"loaded snapshot {args.snap}")

    if not args.mute:
        pygame.mixer.pre_init(44100, -16, 2, 1024)
    pygame.init()
    W, H = 320, 224
    screen = pygame.display.set_mode((W * args.scale, H * args.scale))
    pygame.display.set_caption("KOF98 RL viewer")
    clock = pygame.time.Clock()

    # audio output. Preferred: SDL2 callback device fed from a circular buffer
    # (same pull-model as the exe's waveOut buffers). Fallback: mixer chunks.
    AU_RATE = 44100
    au_kind = None
    au_sdl = au_chan = au_acc = au_pending = None
    if not args.mute:
        au_buf = np.empty((4096, 2), dtype=np.int16)
        try:
            au_sdl = SdlAudioOut(AU_RATE)
            au_kind = "sdl2"
        except Exception:
            from collections import deque
            au_chan = pygame.mixer.Channel(0)
            au_acc = np.empty((0, 2), dtype=np.int16)
            au_pending = deque()
            au_kind = "mixer"

        # prime ~130ms of audio while the device is still paused
        for _ in range(8):
            env.run(1)
            n = env.audio_drain(au_buf.ctypes.data, len(au_buf))
            if n and au_kind == "sdl2":
                au_sdl.push(au_buf[:n].tobytes())
        if au_kind == "sdl2":
            au_sdl.start()
        print(f"audio backend: {au_kind}")

    KEYMAP = {  # pygame key -> (button bit, player)
        pygame.K_UP: (UP, 0), pygame.K_DOWN: (DOWN, 0),
        pygame.K_LEFT: (LEFT, 0), pygame.K_RIGHT: (RIGHT, 0),
        pygame.K_z: (A, 0), pygame.K_x: (B, 0), pygame.K_c: (C, 0), pygame.K_v: (D, 0),
        pygame.K_i: (UP, 1), pygame.K_k: (DOWN, 1),
        pygame.K_j: (LEFT, 1), pygame.K_l: (RIGHT, 1),
        pygame.K_KP1: (A, 1), pygame.K_KP2: (B, 1), pygame.K_KP3: (C, 1), pygame.K_KP4: (D, 1),
    }

    frame = 0
    t0 = time.perf_counter()
    while True:
        for e in pygame.event.get():
            if e.type == pygame.QUIT or (e.type == pygame.KEYDOWN and e.key == pygame.K_ESCAPE):
                pygame.quit(); return

        inp = agent_input(env, frame)
        if inp is None:
            keys = pygame.key.get_pressed()
            p1 = p2 = 0
            for k, (bit, pl) in KEYMAP.items():
                if keys[k]:
                    if pl == 0: p1 |= bit
                    else: p2 |= bit
            start = (1 if keys[pygame.K_1] else 0) | (2 if keys[pygame.K_2] else 0)
            coin = 1 if keys[pygame.K_5] else 0
        else:
            p1, p2, start, coin = inp

        env.set_input(p1=p1, p2=p2, start=start, coin=coin)
        env.run(1)
        frame += 1

        fb, w, h = env.framebuffer()
        raw = __import__("ctypes").string_at(fb, w * h * 4)
        surf = pygame.image.frombuffer(raw, (w, h), "BGRA")  # pixels are 0x00RRGGBB
        screen.blit(pygame.transform.scale(surf, (w * args.scale, h * args.scale)), (0, 0))
        pygame.display.flip()

        if au_kind == "sdl2":
            n = env.audio_drain(au_buf.ctypes.data, len(au_buf))
            if n:
                au_sdl.push(au_buf[:n].tobytes())
        elif au_kind == "mixer":
            n = env.audio_drain(au_buf.ctypes.data, len(au_buf))
            if n:
                au_acc = np.concatenate([au_acc, au_buf[:n]])
            while len(au_acc) >= 2822 and len(au_pending) < 4:   # ~64ms chunks
                au_pending.append(pygame.sndarray.make_sound(au_acc[:2822].copy()))
                au_acc = au_acc[2822:]
            if not au_chan.get_busy() and au_pending:
                au_chan.play(au_pending.popleft())
            if au_chan.get_queue() is None and au_pending:
                au_chan.queue(au_pending.popleft())

        if args.frames and frame >= args.frames: break
        if au_kind == "sdl2":
            # Pace to the playback clock: the Neo Geo runs at ~59.19 fps, not
            # 60, so a fixed 60 fps cap makes audio production outrun playback
            # until the buffer overruns and drops -- the periodic little skip.
            # Holding the buffer at ~120ms makes drops/underruns impossible.
            while au_sdl.avail_bytes() > AU_RATE * 4 * 120 // 1000:
                time.sleep(0.001)
        elif not args.fast:
            clock.tick_busy_loop(60)   # tick() has 15ms granularity on Windows

    dt = time.perf_counter() - t0
    print(f"{frame} frames in {dt:.1f}s = {frame/dt:.0f} fps")
    if au_sdl is not None:
        print(f"audio: underruns={au_sdl.underruns} drops={au_sdl.drops} (both should be 0)")
    pygame.quit()

if __name__ == "__main__":
    main()
