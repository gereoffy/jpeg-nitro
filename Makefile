# jpeg-nitro: nitroview (fast image viewer for macOS) + nitrojpeg (parallel JPEG decoder)
#
#   make                 nitroview; uses libjpeg-turbo as fallback decoder if it is present
#                        in third_party/ljt (see scripts/get-deps.sh), standalone otherwise
#   make TURBOJPEG=0     no libjpeg-turbo: JPEGs nitrojpeg can't decode go to Apple ImageIO
#   make WUFFS=0         no Wuffs: PNG is decoded by Apple ImageIO (~1.8x slower)
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

WUFFS      ?= $(if $(wildcard third_party/wuffs.c),1,0)

ifeq ($(TURBOJPEG),1)
VIEWER_DEFS += -DNV_TURBOJPEG -I$(LJT)/include
VIEWER_LIBS += $(LJT)/lib/libturbojpeg.a -Wl,-w
endif
ifeq ($(WUFFS),1)
VIEWER_DEFS += -DNV_WUFFS
VIEWER_OBJS += build/png_wuffs.o
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

build/nitropsd.o: src/nitropsd.c src/nitropsd.h src/nitropng.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/nitropng.o: src/nitropng.c src/nitropng.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# optional Wuffs PNG decoder (third_party/wuffs.c, Apache-2.0), compiled once
build/png_wuffs.o: src/png_wuffs.c src/png_wuffs.h
	@mkdir -p build
	$(CC) -O3 $(CPUFLAGS) -w -c $< -o $@

# rebuild the viewer when the optional parts change
CONFIG := tj$(TURBOJPEG)-wuffs$(WUFFS)
build/.config-$(CONFIG):
	@mkdir -p build
	@rm -f build/.config-* nitroview
	@touch $@

nitroview: src/nitroview.m build/nitrojpeg.o build/nitropng.o build/nitropsd.o $(VIEWER_OBJS) build/.config-$(CONFIG) Makefile
	$(CC) $(CFLAGS) $(VIEWER_DEFS) -fobjc-arc src/nitroview.m build/nitrojpeg.o build/nitropng.o build/nitropsd.o $(VIEWER_OBJS) $(VIEWER_LIBS) \
	  $(FRAMEWORKS) -framework ImageIO -o $@
	@echo "built nitroview (libjpeg-turbo fallback: $(if $(filter 1,$(TURBOJPEG)),yes,no), Wuffs PNG: $(if $(filter 1,$(WUFFS)),yes,no), ImageIO: always)"

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

bench/pngbench: bench/pngbench.m | deps
	$(CC) -O3 $(CPUFLAGS) -w -fobjc-arc $< -framework Foundation -framework ImageIO -framework CoreGraphics -o $@

bench/pngverify: bench/pngverify.c build/nitropng.o
	$(CC) -O3 $(CPUFLAGS) -w $^ -lz -o $@

# nitropng robustness test on damaged PNGs, with AddressSanitizer + UBSan
bench/pngrobust: bench/pngrobust.c src/nitropng.c src/nitropng.h
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer bench/pngrobust.c src/nitropng.c -o $@

bench/psdverify: bench/psdverify.c build/nitropsd.o build/nitropng.o
	$(CC) -O3 $(CPUFLAGS) -w $^ -o $@

# nitropsd robustness test on damaged PSDs, with AddressSanitizer + UBSan
bench/psdrobust: bench/psdrobust.c src/nitropsd.c src/nitropsd.h src/nitropng.c
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer bench/psdrobust.c src/nitropsd.c src/nitropng.c -o $@

bench/pngsplit: bench/pngsplit.c
	$(CC) -O3 $(CPUFLAGS) $< -lz -o $@

bench/freqprobe: bench/freqprobe.c
	$(CC) -O2 $< -o $@

bench/sustain: bench/sustain.c build/nitrojpeg.o
	$(CC) -O2 $^ -o $@

tools: bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust \
       bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust

clean:
	rm -rf build nitroview bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit \
	  bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust

.PHONY: all tools clean deps
