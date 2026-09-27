# jpeg-nitro: nitroview (fast image viewer for macOS) + nitrojpeg (parallel JPEG decoder)
#
#   make                 nitroview; uses libjpeg-turbo as fallback decoder if it is present
#                        in third_party/ljt (see scripts/get-deps.sh), standalone otherwise
#   make TURBOJPEG=0     no libjpeg-turbo: JPEGs nitrojpeg can't decode go to Apple ImageIO
#   make WUFFS=0         no Wuffs: PNG is decoded by Apple ImageIO (~1.8x slower)
#   make app             nitroview.app bundle, so the Finder can open images with it
#   make tools           benchmarks and tests (need scripts/get-deps.sh first)
#   make linux           Linux (also the default there): the portable tools - nbench,
#                        pngverify, psdverify, the ASan robustness tests, and verify / robust
#                        once scripts/get-deps.sh has built libjpeg-turbo. Needs clang.
#   make windows         cross-compile nitroview.exe (the viewer), bench/nbench.exe and
#                        bench/psdverify.exe for Windows (x64, static, AVX2) with llvm-mingw:
#                        WINCC=path/to/x86_64-w64-mingw32-clang

LJT        := third_party/ljt
TURBOJPEG  ?= $(if $(wildcard $(LJT)/lib/libturbojpeg.a),1,0)
CC         := clang
ARCH       := $(shell uname -m)
ifeq ($(ARCH),x86_64)
CPUFLAGS   := -march=native
else
CPUFLAGS   := -mcpu=native
endif
UNAME_S    ?= $(shell uname -s)
COMMA      := ,
ifeq ($(UNAME_S),Darwin)
PORTFLAGS  :=
THREADLIBS :=
else   # the decoders' loop bodies are blocks (see src/nitro_os.h): clang -fblocks
PORTFLAGS  := -fblocks
THREADLIBS := -lpthread
endif
CFLAGS     := -O3 $(CPUFLAGS) $(PORTFLAGS) -Wall -Wextra -Wno-unused-parameter
ASAN       := -O1 -g $(PORTFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer
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
LDQUIET    := $(if $(filter Darwin,$(UNAME_S)),-Wl$(COMMA)-w)   # Apple ld: no duplicate-library warnings
REF_LIBS   := $(LJT)/lib/libturbojpeg.a $(LDQUIET)

ifeq ($(UNAME_S),Darwin)
all: nitroview
else
all: linux
endif

# production decoder: no dependencies
build/nitrojpeg.o: src/nitrojpeg.c src/nitrojpeg.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# reference build: + old libjpeg-turbo engine and BGRX output (tests, benchmarks)
build/nitrojpeg_ref.o: src/nitrojpeg.c src/nitrojpeg.h src/nitro_os.h | ljt
	@mkdir -p build
	$(CC) $(REF_CFLAGS) -c $< -o $@

build/nitropsd.o: src/nitropsd.c src/nitropsd.h src/nitropng.h src/nitro_os.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/nitropng.o: src/nitropng.c src/nitropng.h src/nitro_os.h
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

# the Metal shader source, embedded as a byte array
build/shaders.inc: src/shaders.metal
	@mkdir -p build
	cd src && xxd -i shaders.metal > ../$@

nitroview: src/nitroview.m build/shaders.inc build/nitrojpeg.o build/nitropng.o build/nitropsd.o $(VIEWER_OBJS) build/.config-$(CONFIG) Makefile
	$(CC) $(CFLAGS) $(VIEWER_DEFS) -Ibuild -fobjc-arc src/nitroview.m build/nitrojpeg.o build/nitropng.o build/nitropsd.o $(VIEWER_OBJS) $(VIEWER_LIBS) \
	  $(FRAMEWORKS) -framework ImageIO -o $@
	@echo "built nitroview (libjpeg-turbo fallback: $(if $(filter 1,$(TURBOJPEG)),yes,no), Wuffs PNG: $(if $(filter 1,$(WUFFS)),yes,no), ImageIO: always)"

# Dock / Finder icon: every iconset size scaled from packaging/icon.png
build/nitroview.icns: packaging/icon.png
	@mkdir -p build
	rm -rf build/nitroview.iconset && mkdir build/nitroview.iconset
	for s in 16 32 128 256 512; do \
	  sips -z $$s $$s $< --out build/nitroview.iconset/icon_$${s}x$${s}.png >/dev/null; \
	  sips -z $$((s*2)) $$((s*2)) $< --out build/nitroview.iconset/icon_$${s}x$${s}@2x.png >/dev/null; \
	done
	iconutil -c icns build/nitroview.iconset -o $@

# macOS app bundle (for the Finder: "Open With" / "Change All"), ad-hoc signed
app: nitroview packaging/Info.plist build/nitroview.icns
	rm -rf nitroview.app
	mkdir -p nitroview.app/Contents/MacOS nitroview.app/Contents/Resources
	cp nitroview nitroview.app/Contents/MacOS/
	cp packaging/Info.plist nitroview.app/Contents/
	cp build/nitroview.icns nitroview.app/Contents/Resources/
	codesign --force --sign - nitroview.app
	@echo "built nitroview.app (copy it to /Applications, then Finder: Get Info > Open with > nitroview > Change All)"

deps:
	@test -f $(LJT)/lib/libturbojpeg.a -a -f third_party/stb_image.h -a -f third_party/wuffs.c || \
	  { echo "missing dependencies: run scripts/get-deps.sh first"; exit 1; }
ljt:
	@test -f $(LJT)/lib/libturbojpeg.a || { echo "missing libjpeg-turbo: run scripts/get-deps.sh first"; exit 1; }

bench/bench: bench/bench.m build/nitrojpeg_ref.o | deps
	$(CC) $(REF_CFLAGS) -w -fobjc-arc $^ $(REF_LIBS) -framework Foundation -framework ImageIO \
	  -framework CoreGraphics -framework VideoToolbox -framework CoreMedia -framework CoreVideo -o $@

bench/verify: bench/verify.c build/nitrojpeg_ref.o | ljt
	$(CC) $(REF_CFLAGS) -w $^ $(REF_LIBS) $(THREADLIBS) -o $@

# robustness test on damaged files (production decoder + TurboJPEG fallback, like
# the viewer), with AddressSanitizer + UBSan
bench/robust: bench/robust.c src/nitrojpeg.c src/nitrojpeg.h | ljt
	$(CC) $(ASAN) -I$(LJT)/include bench/robust.c src/nitrojpeg.c $(REF_LIBS) $(THREADLIBS) -o $@

bench/pngbench: bench/pngbench.m build/nitropng.o | deps
	$(CC) -O3 $(CPUFLAGS) -w -fobjc-arc $^ -framework Foundation -framework ImageIO -framework CoreGraphics -o $@

bench/pngverify: bench/pngverify.c build/nitropng.o
	$(CC) -O3 $(CPUFLAGS) $(PORTFLAGS) -w $^ -lz $(THREADLIBS) -o $@

# nitropng robustness test on damaged PNGs, with AddressSanitizer + UBSan
bench/pngrobust: bench/pngrobust.c src/nitropng.c src/nitropng.h
	$(CC) $(ASAN) bench/pngrobust.c src/nitropng.c $(THREADLIBS) -o $@

bench/psdverify: bench/psdverify.c build/nitropsd.o build/nitropng.o
	$(CC) -O3 $(CPUFLAGS) $(PORTFLAGS) -w $^ $(THREADLIBS) -o $@

# portable benchmark of the three decoders (also builds on Linux, see README)
bench/nbench: bench/nbench.c build/nitrojpeg.o build/nitropng.o build/nitropsd.o
	$(CC) $(CFLAGS) $^ $(THREADLIBS) -o $@

# nitropsd robustness test on damaged PSDs, with AddressSanitizer + UBSan
bench/psdrobust: bench/psdrobust.c src/nitropsd.c src/nitropsd.h src/nitropng.c
	$(CC) $(ASAN) bench/psdrobust.c src/nitropsd.c src/nitropng.c $(THREADLIBS) -o $@

bench/pngsplit: bench/pngsplit.c
	$(CC) -O3 $(CPUFLAGS) $< -lz -o $@

bench/freqprobe: bench/freqprobe.c
	$(CC) -O2 $< -o $@

bench/sustain: bench/sustain.c build/nitrojpeg.o
	$(CC) -O2 $^ -o $@

tools: bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust \
       bench/nbench

# Linux (or any clang + pthreads system): the tools without macOS APIs; verify and robust
# only once scripts/get-deps.sh has built libjpeg-turbo
LINUX_TOOLS := bench/nbench bench/pngverify bench/psdverify bench/pngrobust bench/psdrobust \
               $(if $(wildcard $(LJT)/lib/libturbojpeg.a),bench/verify bench/robust)
linux: $(LINUX_TOOLS)
	@echo "built $(LINUX_TOOLS)$(if $(wildcard $(LJT)/lib/libturbojpeg.a),, (verify / robust: run scripts/get-deps.sh first))"

clean:
	rm -rf build nitroview nitroview.app nitroview.exe bench/bench bench/verify bench/robust bench/freqprobe bench/sustain bench/pngbench bench/pngsplit \
	  bench/pngverify bench/pngrobust bench/psdverify bench/psdrobust bench/nbench bench/*.exe

# Windows (cross, llvm-mingw): self-contained .exe files, only system DLLs (UCRT: Windows 10 /
# Server 2016 and later). WINARCH=x86-64-v2 for CPUs without AVX2.
WINCC   ?= x86_64-w64-mingw32-clang
WINARCH ?= x86-64-v3
WINFLAGS = -O3 -march=$(WINARCH) -fblocks -Wall -Wextra -Wno-unused-parameter -static
DEC_SRC := src/nitrojpeg.c src/nitropng.c src/nitropsd.c
DEC_HDR := src/nitrojpeg.h src/nitropng.h src/nitropsd.h src/nitro_os.h
bench/nbench.exe: bench/nbench.c $(DEC_SRC) $(DEC_HDR)
	$(WINCC) $(WINFLAGS) bench/nbench.c $(DEC_SRC) -lpthread -o $@
bench/psdverify.exe: bench/psdverify.c src/nitropsd.c src/nitropng.c $(DEC_HDR)
	$(WINCC) $(WINFLAGS) -w bench/psdverify.c src/nitropsd.c src/nitropng.c -lpthread -o $@
WINRES  ?= $(patsubst %clang,%windres,$(WINCC))
WINLIBS := -ld3d11 -ldxgi -ld3dcompiler -lwindowscodecs -lole32 -loleaut32 -lshlwapi -lshell32 -luser32 \
           -lgdi32 -ladvapi32 -luuid -ldxguid -lwinmm -lpthread
build/nitroview_res.o: packaging/nitroview.rc packaging/nitroview.ico
	@mkdir -p build
	$(WINRES) -I packaging $< -O coff -o $@
nitroview.exe: src/nitroview_win.c $(DEC_SRC) $(DEC_HDR) build/nitroview_res.o
	$(WINCC) $(WINFLAGS) -municode src/nitroview_win.c $(DEC_SRC) build/nitroview_res.o $(WINLIBS) -o $@
windows: nitroview.exe bench/nbench.exe bench/psdverify.exe
	@echo "built nitroview.exe bench/nbench.exe bench/psdverify.exe (Windows x64, $(WINARCH))"

.PHONY: all app tools linux windows clean deps ljt
