# Files

**Decoders** (`src/`)
- `nitrojpeg.c/.h`: parallel JPEG decoder (own Huffman decoding, AVX2 / NEON IDCT); with `NJ_REFERENCE` also the libjpeg-turbo based comparison code
- `nitropng.c/.h`: parallel PNG decoder
- `png_wuffs.c/.h`: the optional Wuffs path for interlaced PNGs
- `nitropsd.c/.h`: parallel PSD / PSB merged-image decoder
- `nitroheic.c/.h`: HEIC (HEIF) reading, the tiles handled in parallel
- `nitrohevc.c/.h`: our optional HEVC intra decoder (`NITROHEVC=1`)
- `nitroavif.c/.h`: AVIF (AV1 through dav1d, our color conversion)
- `heif.h`: the HEIF structure, shared by nitroheic and nitroavif
- `nitrodng.c/.h`: DNG raw development (lossless JPEG, demosaicing, color)
- `nitrowebp.c/.h`: lossy WebP (VP8) decoder
- `nitrotiff.c/.h`: TIFF decoder (strips / tiles in parallel)
- `tiff.h`: reading TIFF directories, shared by nitrotiff, nitrodng, nitrojpeg (raw files, EXIF), nitropng and nitrowebp
- `nitrosvg.cpp/.h`: SVG with lunasvg and plutovg
- `nitrodxf.cpp/.h`: DXF, our own reader, drawn with plutovg
- `nitrodwg.cpp/.h`: DWG (R13 – R2018, not R2007), our own reader, for nitrodxf
- `nitrometa.cpp/.h`: WMF / EMF, our own reader (also the pictures inside Visio and CorelDRAW files), drawn with plutovg
- `nitrocdr.cpp/.h`: CorelDRAW with libcdr and Visio with libvisio (one painter for librevenge's drawing interface), drawn with plutovg; the pictures inside through nitrometa, nitrotiff and nitrodxf (an embedded DWG)
- `nitrodio.cpp/.h`: draw.io, our own reader (libxml2, zlib), drawn with plutovg
- `nitroalloc.cpp`: the vector renderers' mimalloc hookup
- `nitro_os.h`: what the decoders need from the OS (parallel loop, clock, CPU count)

**The library**
- `src/nitro_load.c/.h`: file → decoded image with the decoders above (the library's face, what the viewers use)
- `src/nitro_convert.c/.h`: its planes → RGB
- `examples/nitroconvert.c`: the example program on the library, any image → BMP
- `examples/nitrodraw.c`: the example program on libnitrovec, a page of any drawing → BMP
- `src/nitro_prefetch.c/.h`: the viewers' shared prefetching loader
- `src/nitro_view.h`: the viewers' shared zoom arithmetic
- `src/nitro_vector.c/.h` (in libnitrovec.a, with the readers below): the viewers' vector drawings (SVG, DXF / DWG, WMF / EMF, CorelDRAW, Visio, draw.io behind one interface: loading, pages, rendering in parallel bands, the thread limit, the background setting; what the four viewers do alike: a page rendered in full for the target size, the part rendered sharp when zoomed in, the window title's tail)
- `src/nitro_draw.h`: what the display-list renderers share (the backgrounds, a render's canvas over the caller's buffer, the cancel callback, an item's box against the view and plutovg's fixed point, the boxes of paths and turned rectangles, the union of boxes a page's area is made of)
- `src/nitro_font.cpp/.h`: the vector renderers' fonts, one index of the installed font files and their faces for nitrosvg (lunasvg asks it for every font), nitrodxf, nitrometa, nitrocdr and nitrodio, and the lock they draw under

**The viewers**
- `src/nitroview.m`: Cocoa + Metal
- `src/shaders.metal`: its GPU shaders (color conversion, drawing), embedded at build time
- `packaging/Info.plist`: the Mac app's settings and the file types it opens
- `packaging/icon.png`: the app's icon
- `src/nitroview_win.c`: Win32 + Direct3D 11 + WIC
- `packaging/nitroview.ico/.rc`: its icon
- `src/gnome/`: GTK 4 + libadwaita
- `packaging/gnome/`: desktop integration
- `packaging/flatpak/`: the Flatpak manifest
- `src/nitroview_sdl.c`: SDL3 + OpenGL
- `Makefile`: every build (see [BUILDING.md](BUILDING.md))
- `scripts/get-deps.sh`: downloads and builds the optional dependencies
- `patches/`: the librevenge and libxml2 fixes it applies, libxml2's configuration, libvisio's token tables

**The project**
- `shell.nix`: a Nix shell with the build tools
- `.github/workflows/macos-release.yml`: the notarized macOS release (build, sign, notarize, publish)
- `.github/workflows/macos-release-finish.yml`: finishes a release whose notarization took longer
- `docs/logo.webp`: the README's logo

**Tests and benchmarks** (`bench/`)
- `bench.m`: decoder benchmark (macOS, all decoders)
- `nbench.c`: the portable one (nitrojpeg / nitropng / nitropsd)
- `verify.c`: bit-exactness and sync statistics vs libjpeg-turbo
- `rgbverify.c`: nitro_convert's RGB vs libjpeg-turbo's
- `bgrxverify.m`: nitrotiff's and nitropng's BGRX vs ImageIO
- `robust.c`: damaged JPEG files under ASan
- `pngrobust.c`: damaged PNG files under ASan
- `psdrobust.c`: damaged PSD files under ASan
- `pngbench.m`: PNG decoder comparison
- `pngsplit.c`: PNG decoding's time split
- `pngverify.c`: nitropng byte-exactness vs zlib
- `mkpsd.py`: test PSD writer (every compression)
- `psdverify.c`: all compressions vs raw
- `freqprobe.c`: clock measurement
- `sustain.c`: sustained load
- `ab.sh`: noise-resistant A/B comparison
- `results/`: measured results

## Licenses

The viewer is MIT; the code it takes from others and the licenses of the optional dependencies are
in [LICENSE](LICENSE).
