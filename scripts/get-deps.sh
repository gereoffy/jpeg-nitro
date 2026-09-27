#!/bin/bash
# Downloads and builds the optional dependencies into third_party/:
#   - libjpeg-turbo 3.1.2 (static, with SIMD): fallback decoder for nitroview,
#     reference for the tests and benchmarks (macOS: x86_64 + arm64 in one library)
#   - nasm (needed to build libjpeg-turbo's SIMD code) and cmake (via pip), locally
#   - stb_image.h and wuffs (only for the decoder comparison in bench/bench)
# Nothing is installed system-wide.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party && cd third_party
TP=$PWD
LJT_VER=3.1.2
NASM_VER=2.16.03
JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 8)

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

# libjpeg-turbo for one architecture into $TP/$2 (static, with its SIMD code)
build_ljt() {   # arch prefix
  local A=$1 P=$2 CPU PROC
  case $A in
    x86_64) PROC=x86_64; CPU=$([ "$(uname -s)" = Darwin ] && echo "-march=x86-64-v3" || echo "-march=native") ;;
    arm64|aarch64) PROC=aarch64; CPU=$([ "$(uname -s)" = Darwin ] && echo "-mcpu=apple-m1" || echo "-mcpu=native") ;;
    *) PROC=$A; CPU="" ;;
  esac
  local MAC=()
  [ "$(uname -s)" = Darwin ] && MAC=(-DCMAKE_OSX_ARCHITECTURES="$A" -DCMAKE_SYSTEM_PROCESSOR="$PROC" -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0)
  [ -d libjpeg-turbo-$LJT_VER ] || {
    curl -fsSL -o ljt.tar.gz https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$LJT_VER/libjpeg-turbo-$LJT_VER.tar.gz
    tar xf ljt.tar.gz
  }
  (cd libjpeg-turbo-$LJT_VER &&
   "$CMAKE" -S . -B build-$A -DCMAKE_BUILD_TYPE=Release -DENABLE_SHARED=OFF "${MAC[@]}" \
     -DCMAKE_ASM_NASM_COMPILER="$TP/tools/nasm/bin/nasm" -DCMAKE_INSTALL_PREFIX="$TP/$P" -DCMAKE_INSTALL_LIBDIR=lib \
     -DCMAKE_C_FLAGS="-O3 $CPU" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null &&
   "$CMAKE" --build build-$A -j"$JOBS" >/dev/null &&
   "$CMAKE" --install build-$A >/dev/null)
}

if [ "$(uname -s)" = Darwin ]; then
  # macOS: both architectures in one (universal) library, for the universal nitroview.app
  if [ ! -f ljt/.universal-macos12 ]; then   # (older versions built only the host architecture)
    build_ljt x86_64 ljt-x86_64
    build_ljt arm64 ljt-arm64
    rm -rf ljt && cp -R ljt-x86_64 ljt
    for L in libturbojpeg.a libjpeg.a; do
      lipo -create ljt-x86_64/lib/$L ljt-arm64/lib/$L -output ljt/lib/$L
    done
    touch ljt/.universal-macos12
  fi
elif [ ! -f ljt/lib/libturbojpeg.a ]; then
  build_ljt "$(uname -m)" ljt
fi
echo "dependencies ready in third_party/ (libjpeg-turbo $LJT_VER, stb_image, wuffs)"
