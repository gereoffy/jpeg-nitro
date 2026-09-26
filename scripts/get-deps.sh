#!/bin/bash
# Downloads and builds the optional dependencies into third_party/:
#   - libjpeg-turbo 3.1.2 (static, with SIMD): fallback decoder for nitroview,
#     reference for the tests and benchmarks
#   - nasm (needed to build libjpeg-turbo's SIMD code) and cmake (via pip), locally
#   - stb_image.h and wuffs (only for the decoder comparison in bench/bench)
# Nothing is installed system-wide.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party && cd third_party
TP=$PWD
LJT_VER=3.1.2
NASM_VER=2.16.03
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)

if [ ! -f stb_image.h ]; then
  curl -fsSL -o stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
fi
if [ ! -f wuffs.c ]; then
  curl -fsSL -o wuffs.c https://raw.githubusercontent.com/google/wuffs/main/release/c/wuffs-unsupported-snapshot.c
fi

if [ ! -x tools/nasm/bin/nasm ]; then
  curl -fsSL -o nasm.tar.xz https://www.nasm.us/pub/nasm/releasebuilds/$NASM_VER/nasm-$NASM_VER.tar.xz
  tar xf nasm.tar.xz
  (cd nasm-$NASM_VER && ./configure -q --prefix="$TP/tools/nasm" && make -j"$JOBS" -s && make install -s)
fi

CMAKE=$(command -v cmake || true)
if [ -z "$CMAKE" ]; then
  [ -x tools/py/bin/cmake ] || python3 -m pip install -q --target tools/py cmake
  CMAKE=$(find "$TP/tools/py" -path '*data/bin/cmake' -type f | head -1)
fi

if [ ! -f ljt/lib/libturbojpeg.a ]; then
  curl -fsSL -o ljt.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LJT_VER/libjpeg-turbo-$LJT_VER.tar.gz
  tar xf ljt.tar.gz
  cd libjpeg-turbo-$LJT_VER
  CPU=$([ "$(uname -m)" = x86_64 ] && echo "-march=native" || echo "-mcpu=native")
  "$CMAKE" -S . -B build -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF \
    -DCMAKE_ASM_NASM_COMPILER="$TP/tools/nasm/bin/nasm" -DCMAKE_INSTALL_PREFIX="$TP/ljt" \
    -DCMAKE_C_FLAGS="-O3 $CPU" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
  "$CMAKE" --build build -j"$JOBS" >/dev/null
  "$CMAKE" --install build >/dev/null
  cd ..
fi
echo "dependencies ready in third_party/ (libjpeg-turbo $LJT_VER, stb_image, wuffs)"
