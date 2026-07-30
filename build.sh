#!/bin/sh
# KOF98 Native -- Linux/macOS one-click build.
# Builds the RL interface shared library into build/lib/.
# Uses the system C++ compiler (clang++ or g++); no zig required.
# (The playable windowed app is currently Windows-only; on Linux/macOS the
#  RL shared library + Python viewer is the way to go.)
set -e
cd "$(dirname "$0")"

CXX="${CXX:-c++}"

# CPU baseline: AVX2 on x86_64 (2013+), generic on ARM (Apple Silicon)
ARCH="$(uname -m)"
if [ "$ARCH" = "x86_64" ]; then
    MARCH="-march=x86-64-v3"
else
    MARCH=""
fi

OS="$(uname -s)"
case "$OS" in
    Darwin) SHARED="-dynamiclib"; OUT="libkof98.dylib" ;;
    *)      SHARED="-shared";     OUT="libkof98.so" ;;
esac

mkdir -p build/lib

SRCS="src/emu.cpp src/video.cpp src/cpu_interp.cpp src/cpu_interp2.cpp \
src/z80.cpp src/ym2610.cpp src/romload.cpp src/kof98_api.cpp \
src/ymfm/ymfm_adpcm.cpp src/ymfm/ymfm_opn.cpp src/ymfm/ymfm_ssg.cpp"

echo "compiler: $CXX   target: $OS/$ARCH"
$CXX -O3 -std=c++17 -fPIC $MARCH -Isrc $SHARED -o "build/lib/$OUT" $SRCS

echo "OK: build/lib/$OUT"
echo "python wrapper: tools/py/kof98env.py (needs roms/kof98.zip + roms/neogeo.zip)"
