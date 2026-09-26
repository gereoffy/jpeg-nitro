# jpeg-nitro: nitroview (fast image viewer for macOS) + nitrojpeg (parallel JPEG decoder)
#
#   make                 nitroview; uses libjpeg-turbo as fallback decoder if it is present
#                        in third_party/ljt (see scripts/get-deps.sh), standalone otherwise
#   make TURBOJPEG=0     force a fully standalone viewer (no libjpeg-turbo): files nitrojpeg
#                        can't decode (progressive, CMYK, damaged ...) show "CANNOT DECODE"
#   make tools           benchmarks and tests (need scripts/get-deps.sh first)

LJT        := third_party/ljt
TURBOJPEG  ?= $(if $(wildcard $(LJT)/lib/libturbojpeg.a),1,0)
CC         := clang
ARCH       := $(shell uname -m)
ifeq ($(ARCH),x86_64)
CPUFLAGS   := -march=native
else
CPUFLAGS   := -mcpu=native
endif
CFLAGS     := -O3 $(CPUFLAGS) -Wall -Wextra -Wno-unused-parameter
FRAMEWORKS := -framework Cocoa -framework Metal -framework MetalKit -framework QuartzCore

ifeq ($(TURBOJPEG),1)
VIEWER_DEFS := -DNV_TURBOJPEG -I$(LJT)/include
VIEWER_LIBS := $(LJT)/lib/libturbojpeg.a -Wl,-w
endif

REF_CFLAGS := $(CFLAGS) -DNJ_REFERENCE -I$(LJT)/include
REF_LIBS   := $(LJT)/lib/libturbojpeg.a -Wl,-w

all: nitroview

# production decoder: no dependencies
build/nitrojpeg.o: src/nitrojpeg.c src/nitrojpeg.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# reference build: + old libjpeg-turbo engine and BGRX output (tests, benchmarks)
build/nitrojpeg_ref.o: src/nitrojpeg.c src/nitrojpeg.h | deps
	@mkdir -p build
	$(CC) $(REF_CFLAGS) -c $< -o $@

# rebuild the viewer when TURBOJPEG changes
build/.turbojpeg-$(TURBOJPEG):
	@mkdir -p build
	@rm -f build/.turbojpeg-* nitroview
	@touch $@

nitroview: src/nitroview.m build/nitrojpeg.o build/.turbojpeg-$(TURBOJPEG) Makefile
	$(CC) $(CFLAGS) $(VIEWER_DEFS) -fobjc-arc src/nitroview.m build/nitrojpeg.o $(VIEWER_LIBS) $(FRAMEWORKS) -o $@
	@echo "built nitroview (libjpeg-turbo fallback: $(if $(filter 1,$(TURBOJPEG)),yes,no))"

deps:
	@test -f $(LJT)/lib/libturbojpeg.a -a -f third_party/stb_image.h -a -f third_party/wuffs.c || \
	  { echo "missing dependencies: run scripts/get-deps.sh first"; exit 1; }

bench/bench: bench/bench.m build/nitrojpeg_ref.o | deps
	$(CC) $(REF_CFLAGS) -w -fobjc-arc $^ $(REF_LIBS) -framework Foundation -framework ImageIO \
	  -framework CoreGraphics -framework VideoToolbox -framework CoreMedia -framework CoreVideo -o $@

bench/verify: bench/verify.c build/nitrojpeg_ref.o | deps
	$(CC) $(REF_CFLAGS) -w $^ $(REF_LIBS) -o $@

# robustness test on damaged files (production decoder + TurboJPEG fallback, like
# the viewer), with AddressSanitizer + UBSan
bench/robust: bench/robust.c src/nitrojpeg.c src/nitrojpeg.h | deps
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I$(LJT)/include \
	  bench/robust.c src/nitrojpeg.c $(REF_LIBS) -o $@

bench/freqprobe: bench/freqprobe.c
	$(CC) -O2 $< -o $@

bench/sustain: bench/sustain.c build/nitrojpeg.o
	$(CC) -O2 $^ -o $@

tools: bench/bench bench/verify bench/robust bench/freqprobe bench/sustain

clean:
	rm -rf build nitroview bench/bench bench/verify bench/robust bench/freqprobe bench/sustain

.PHONY: all tools clean deps
