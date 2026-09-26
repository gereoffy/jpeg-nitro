<p align="center"><img src="docs/logo.webp" alt="NitroView – JPEG viewer for macOS" width="800"></p>

# jpeg-nitro

**English** | [Magyar](README.hu.md)

**nitroview** is a very fast image viewer for macOS, and **nitrojpeg** is a parallel
baseline JPEG decoder that decodes a *single* image on all CPU cores. A 24 MP JPEG decodes
in ~11 ms, where libjpeg-turbo needs ~107 ms on one core. Photo series such as timelapses
can be "played back" at full resolution in real time, up to 60 images/s.
(The name: racing cars boost the turbo with nitro.)

Developed and measured on an Intel x86-64 Mac (i9 13th gen, 8 cores / 16 threads, AMD RX 580).
Not tested on Apple Silicon (it builds the plain C IDCT there instead of AVX2).

```
./nitroview [-f] [-s ms] [-j threads] image.jpg ... | directory/
```

| key | action |
|---|---|
| PgDn / Space | next image |
| PgUp / Backspace | previous image |
| Home / End | first / last |
| + / − | zoom in / out (√2 steps, stops exactly at 100% and at fit) |
| 0 | fit to the screen: small images are enlarged too (stays on while paging, until W) |
| 1 | actual size: one image pixel = one screen pixel |
| 2 … 8 | 200% … 800%, pixel-exact: one image pixel = N×N screen pixels |
| ← → ↑ ↓ | move a zoomed image (Shift: half a screen per step) |
| W | back to the image's own size (small images at 100%), window follows image sizes again |
| mouse drag | move a zoomed image |
| scroll wheel | zoom around the point under the cursor (one √2 step per notch; trackpad: smooth, pinch too) |
| double click | fit → 100% at the clicked point; again → back to fit |
| right click / Shift + right click | next / previous image |
| Ctrl + scroll wheel | previous (forward) / next (back) image; trackpad: one image per 40 px |
| mouse side buttons (back / forward) | previous / next image |
| F / Enter | toggle full screen (`-f`: start in full screen) |
| P | pause / resume the slideshow |
| Esc / Q | quit (Esc in full screen: back to a window) |

The window is sized to the image: at exactly 100% if it is smaller than the screen, otherwise
with the image's aspect ratio as large as fits (no black bars). When zooming in, the window grows with the image up to the screen
size (and shrinks back when zooming out; 0 restores it). When paging, it follows each image's
size (keeping its centre), except while zoomed in: then the window, zoom and position stay. After you resize the window yourself (edge drag, green button, tiling) it keeps that
size; W brings it back to the image size and turns the automatic sizing back on. The title shows the zoom level right after the file name. Integer zoom levels (200%, 300%,
400%, … also when reached with the wheel or +/−) show the pixels as exact square blocks;
other levels are smoothly interpolated. Paging stops at the first and last image. **Zoom and position are kept when paging**, so in
a burst or series you can zoom into a detail (e.g. the eyes) and page through to find the
sharpest shot. Wheel zoom keeps the point under the cursor in place, except while the image is narrower
than the window along an axis: it stays centred there until it fills the window. If Ctrl + scroll zooms the whole screen instead, macOS Accessibility zoom is set to use
Ctrl with the scroll gesture (System Settings → Accessibility → Zoom). The zoom keys work by character, so they work on any keyboard layout and the
numeric keypad; the title shows the current zoom. The window title shows the file name, size and
decode time; stdout gets one line per image with read time, decode time and the
key → on-screen latency. EXIF orientation is honoured.

**Slideshow:** `-s <ms>` (or `--slideshow <ms>`): the next image comes when the current one
has been on screen for `ms` milliseconds (`-s 2000` is a classic slideshow, `-s 40` is
~25 images/s). If an image takes longer to load, it waits, so no image is skipped. It stops
at the last image; P pauses; the paging keys keep working and the slideshow continues from
there. Timing follows the display refresh (±8 ms at 60 Hz). Run without arguments (or `-h`)
for the full list of options.

**Other formats:** besides JPEG, nitroview opens PNG, PSD/PSB, HEIC/HEIF, TIFF, WebP, GIF and BMP.
PSD/PSB: the merged (composite) image is shown, decoded by **nitropsd** for 8-bit RGB and
grayscale files (all four compressions: raw, RLE, ZIP, ZIP with prediction), otherwise by Apple
ImageIO — which cannot read ZIP-compressed PSDs at all. A 24 MP RLE PSD decodes in ~5 ms
(ImageIO: ~170 ms), ZIP in ~46 ms: the RLE row table gives every row's position, so all rows
are unpacked in parallel; ZIP uses nitropng's parallel inflate. Big layered PSDs are mostly layer
data (the merged image is often only 13–23% of the file), so the viewer reads just the header,
the image resources (ICC profile) and the merged image: a 482 MB PSD loads in ~20 ms instead of
~155 ms. PNG is decoded by **nitropng**, our own parallel PNG decoder
(8-bit gray / RGB / RGBA, non-interlaced: most PNGs), otherwise by
[Wuffs](https://github.com/google/wuffs) when it is available (`scripts/get-deps.sh`) or by
Apple ImageIO like the other formats. Transparent images are shown over black. On 38 real PNGs
(screenshots, scans, AI images, upscaled textures, Photoshop exports) nitropng averages
**~43 ms/image** vs ~171 ms for Wuffs and ~318 ms for ImageIO; see [nitropng](#nitropng).

**Colour management:** embedded ICC profiles are honoured for every format (e.g. Display P3
from iPhones and Mac screenshots, Adobe RGB scans); images without a profile are treated as
sRGB. The decoded pixel values are left as they are and the display layer is tagged with the
image's colour space, so macOS converts to the monitor's profile at no extra cost and wide-gamut
colours are kept. A 24 MP photo PNG takes ~190 ms with
Wuffs (~310 ms with ImageIO), so PNG is much slower than JPEG; see
[PNG: why not parallel (yet)](#png-why-not-parallel-yet).

`-j N` limits the decoder to N threads (default: all logical CPUs). With 8 threads decoding
takes ~15.4 ms/image instead of 11.8, but heats the CPU less.

Other modes:
- `--bench files…`: load everything without a window (read, decode, GPU) and print timings.
- `--selftest files…`: compare the GPU colour conversion with libjpeg-turbo's output.
- `--zoomtest files…`: replays zoom/pan key sequences offscreen and checks that at 100% every
  drawn pixel equals the decoded image (plus edge clamping and zoom snapping).
- `--inputtest files…`: sends synthesized mouse and wheel events and checks paging and zoom.
- `--auto <ms>`: test mode: pages every `ms` (even if the image isn't shown yet) and quits
  at the end. For latency measurements.

## Results (50 photos, mostly 6000×4000, 17 MB on average)

**Full load in the viewer: ~23 ms/image** (file read 3.5 + decode 12 + GPU 7).
When paging, a prefetched image reaches the screen in **7–20 ms** (one frame at 60 Hz).
A folder of 24 MP images plays back continuously at **~60 images/s** (the display refresh
rate), also from an SSD with nothing cached.

### Decoder comparison (`bench/bench`, decode only, from memory)

| decoder | ms/image |
|---|---|
| Apple ImageIO (CGImageSource) → BGRA | 274 |
| Apple ImageIO, native buffer | 249 |
| stb_image | 175 |
| Wuffs | 159 |
| ImageIO thumbnail 3000 px (DCT scaling) | 159 |
| VideoToolbox (JPEG; no HW JPEG on this machine, runs in software) | 157 |
| libjpeg-turbo 3.1.2 (AVX2), BGRX | 124 |
| libjpeg-turbo, fast DCT + fast upsampling | 112 |
| libjpeg-turbo → YUV planes (no colour conversion) | 107 |
| libjpeg-turbo, 1/2 scale | 100 |
| libjpeg-turbo, 1/4 scale | 96 |
| nitrojpeg, own engine, **1 thread** → YUV | 103 |
| nitrojpeg, old engine (libjpeg-turbo decodes the bands) → YUV | 20.6 |
| **nitrojpeg, own engine, parallel → YUV planes** (used by the viewer) | **11.7** |

1/4 scale barely helps, so most of the time goes into Huffman decoding, which is
inherently sequential in JPEG. Most camera JPEGs have no restart markers, so libjpeg-turbo
can't parallelise within one image. The RX 580 has no hardware JPEG decoder.

### How nitrojpeg parallelises a single image

The Huffman stream is decoded **once**, without libjpeg-turbo:

1. **Preparation (parallel, ~1 ms):** find the end of the entropy-coded data, remove byte
   stuffing (0xFF 0x00) and restart markers, giving a "clean" bit stream.
2. **Files without restart markers: speculative full decoding.** The bit stream is cut into
   ~512 chunks (`32 × CPUs`, ~200 KB of coefficients each, which fits in L2). Every chunk is
   decoded by a thread that starts at an arbitrary byte offset, writing coefficients right
   away. Huffman codes self-synchronise with the true decoding path after a few MCUs.
3. **Stitching, in order, pipelined.** When the true path starts an MCU at a bit position
   that the next chunk also recorded as an MCU start, both paths are identical from there.
   This gives the chunk's MCU index and per-component DC offset; the few MCUs before the
   sync point are discarded.
4. **Dequantisation + IDCT** with AVX2. The algorithm is libjpeg's "ISLOW" IDCT (jidctint.c),
   so the output is bit-identical. As soon as a chunk is stitched, the thread that decoded it
   runs its IDCT while the coefficients are still in cache.
5. **Files with restart markers:** no speculation; the intervals are decoded in parallel with
   the IDCT done per MCU right away.
6. The output is planar Y/Cb/Cr, written directly into a Metal (shared) buffer. Upsampling
   and YCbCr→RGB run in a GPU compute kernel, the GPU builds mipmaps, and the image is drawn
   with trilinear filtering.

Measured synchronisation (`bench/verify`, 512 chunks, first 256 sync points per image,
40 images = 10240 cases): on average a chunk joins the true path after 2.40 "garbage" MCUs
(~922 bits, ~115 bytes). In 40% of the cases after 1 MCU; the worst case was 21 MCUs
(8754 bits), and synchronisation never failed.

**Accuracy:** the parallel output is **bit-identical** to single-threaded libjpeg-turbo on all
50 test images (`bench/verify`, with both the scalar and the AVX2 IDCT). The GPU colour
conversion differs by at most ±1 (rounding, `--selftest`). Unsupported files (progressive,
CMYK, RGB, >16384 px) and damaged files fall back to TurboJPEG in the viewer.

### How it got there (ms/image, 16 threads)

| version | ms/image |
|---|---|
| libjpeg-turbo, 1 thread | 107 |
| speculative skip pass + libjpeg-turbo decodes 128 re-emitted "band" JPEGs (Huffman twice) | 19.9 |
| same, but a single-threaded Huffman pass instead of speculation (`NJ_SEQ=1`, old engine) | 67.2 |
| own engine: Huffman once, coefficients kept in memory, then IDCT | 15.7 |
| + single-lookup AC decoding, pipeline: IDCT while the chunk is in cache | **11.7** |

The single-threaded Huffman pass is slow because it is a serial dependency chain: the length
of each code must be known before the next one can be read (~12 cycles/symbol). Without the
pipeline, the IDCT was memory-bound: writing and re-reading the 96 MB coefficient array
saturated at ~45 GB/s and did not scale beyond 8 threads.

Chunk count (`NJ_CHUNKS`), ms/image:

| chunks | 64 | 128 | 256 | 512 | 1024 | 2048 |
|---|---|---|---|---|---|---|
| ms/image | 12.3 | 12.0 | 11.9 | **11.4** | 12.1 | 14.7 |

Measurements of the old engine (libjpeg-turbo bands, `nj_set_engine(1)`, `NJ_ENGINE=1`) are in
`bench/results/matrix.txt` (chunks × threads) and `bench/results/bands.txt` (band count).

### Measurement method: clock speed and temperature

The development machine's clock jumps between 4 and 5.5 GHz and the CPU passes 90 °C within
seconds under 16-thread load, so a single measurement can be off by ±10%.

- `bench/freqprobe [threads] [seconds]`: measures the real core clock without root (a chain
  of dependent adds runs at one add per cycle). Measured: 4.48 GHz single-threaded from idle
  (the scheduler raises the clock slowly), ~4.95 GHz with 16 threads, ~5.3 GHz with 8.
- `bench/sustain SECONDS THREADS files…`: continuous decoding, speed + clock every 0.5 s.
  Over 30 s with 16 threads there is no downward trend (10.1–11.9 ms/image), so the cooling
  holds; the spread comes from clock jumps, not throttling.
- `bench/ab.sh REPS PAUSE -- "name:ENV=value" …`: runs variants interleaved, with a cool-down
  pause and a clock measurement before each run; prints median / min / max. This keeps the
  min–max spread within about ±3%.

Re-measured this way (5 repetitions, 3 s pause, ms/image, `bench/results/ab_results.txt`):

| comparison | median (min–max) |
|---|---|
| own engine / old engine (libjpeg-turbo bands) | **11.8** (11.8–11.9) / 19.8 (19.5–20.8) |
| 4 / 8 / 12 / 16 threads | 27.0 / 15.4 / 12.3 / **11.8** |
| 256 / 512 / 1024 chunks | 11.9 / **11.7** / 12.2 (overlapping ranges: within noise) |

### nitropng

PNG = deflate (LZ77 + Huffman) + per-row filters, both sequential by design:

1. **Parallel inflate (speculative, like *pugz*):** the compressed stream is cut into one chunk
   per thread. Every thread finds the first valid deflate block header in its chunk (Huffman
   tables must be exactly complete, so false positives are rare and get rejected anyway) and
   decodes from there. Back-references into the still unknown 32 KB before the chunk are written
   as placeholders (16-bit symbols). The chunks are chained in order; only the last 32 KB of each
   need to be resolved sequentially, the rest in parallel (AVX2). The Adler-32 of the whole
   output (combined from per-chunk sums, AVX2) must match, otherwise the viewer falls back.
2. **Parallel filter reversal ("wavefront"):** None / Sub rows are independent; Up / Average /
   Paeth rows follow the previous row segment by segment (1 KB), on 12 of the 16 threads
   (waiting hyper-threads would slow down their working siblings).
3. The unfiltered rows go straight to the GPU, which reads them in place (skipping the filter
   byte), premultiplies alpha and converts to RGBA — no CPU colour conversion.

`bench/pngverify` checks the output byte-for-byte against zlib + a straightforward filter
reversal (all 33 supported test PNGs identical), `bench/pngrobust` runs damaged PNGs with ASan +
UBSan (no errors), `--selftest` checks the whole path against ImageIO (pixel-exact).

### PNG: why it was not parallel

`bench/pngbench` (decoders) and `bench/pngsplit` (where the time goes), on 24 MP photos saved
as PNG:

| decoder | ms/image |
|---|---|
| Apple ImageIO | 339 |
| Wuffs | 193 |
| zlib inflate alone | 91 |
| filter reversal alone (plain C) | 67–134 |

PNG is deflate (LZ77 + Huffman with per-block tables) plus per-row filters. A thread starting
in the middle of the stream does not know the previous 32 KB of output that back-references
point to, and 97% of the rows used the Paeth filter, which depends on the previous row. Both
can be parallelised in principle: speculative inflate with placeholder back-references resolved
later (as in *pugz*), and a "wavefront" filter reversal where row *r* trails row *r−1*. That
could bring a 24 MP PNG to perhaps 20–40 ms (the estimate at the time — see nitropng above
for what it became).

### Damaged files

When the decoder detects an error (invalid Huffman code, coefficient index overrun, the last
MCU ends past the end of the data, wrong number of restart markers, …) it gives up, and the
viewer falls back to TurboJPEG, which shows whatever is recoverable (e.g. the top part of a
half-downloaded file). If that fails too (not a JPEG, broken header, empty file), the title
shows `CANNOT DECODE` and paging continues.

`make bench/robust && bench/robust samples/*` creates ~90 damaged variants of every image
(truncation from 0 bytes to length−1, random byte and bit errors, zeroed or 0xFF-filled
blocks, markers inserted into the data, header damage, cut or duplicated sections) plus
garbage and text files, and runs them through the viewer's decode path built with
AddressSanitizer and UBSan: 4372 cases, 0 errors. The pipeline's synchronisation was also
checked with ThreadSanitizer (on valid and damaged files).

### Display and prefetching

- A background thread decodes 3 images ahead in the paging direction and 2 behind, and
  uploads/converts the neighbours on the GPU in advance.
- Metal buffers and textures are reused (allocating the 128 MB texture per image cost ~6 ms).
- A shared (system memory) buffer read directly by the conversion kernel was the fastest
  (8 ms vs 10 ms with managed buffers / textures).
- The decoder's large buffers (clean bit stream, coefficients) are reused from image to image.

## Using the decoder

`src/nitrojpeg.c` + `src/nitrojpeg.h` are self-contained (C, pthreads, GCD, optional AVX2).
They decode baseline JPEGs (8-bit, Huffman, 1 or 3 components, YCbCr/greyscale, any chroma
subsampling, with or without restart markers) into planar Y/Cb/Cr:

```c
#include "nitrojpeg.h"

nj_info info;
if (nj_read_info(data, len, &info) == 0 && info.supported) {
    uint8_t *planes[3];
    size_t pitch[3];
    for (int c = 0; c < info.ncomp; c++) {
        pitch[c] = info.plane_w[c];                       // any pitch >= plane_w[c]
        planes[c] = malloc(pitch[c] * info.plane_h[c]);   // padded to whole MCUs
    }
    if (nj_decode_planes(data, len, &info, planes, pitch, 0 /* all CPUs */, NULL) == 0) {
        // info.width x info.height visible pixels; chroma subsampled by
        // info.h[c] / info.hmax horizontally and info.v[c] / info.vmax vertically
    } else {
        // damaged or unsupported stream: use another decoder
    }
}
```

## Build

Requirements: Xcode Command Line Tools.

```
make                 # nitroview; uses libjpeg-turbo as fallback if it is in third_party/
make TURBOJPEG=0     # no libjpeg-turbo: other JPEGs go to Apple ImageIO
make WUFFS=0         # no Wuffs: PNG via Apple ImageIO (~1.6x slower)
scripts/get-deps.sh  # download + build libjpeg-turbo, stb_image, Wuffs into third_party/ (~30 s)
make tools           # bench/bench, verify, robust, freqprobe, sustain (needs get-deps.sh)
```

**libjpeg-turbo is optional.** nitrojpeg decodes baseline JPEGs on its own.

- **`TURBOJPEG=1`** (default when `third_party/ljt` exists): progressive, CMYK, RGB, >16384 px
  and damaged files are decoded by TurboJPEG, which also shows what is recoverable from
  damaged images. `--selftest` is only available in this mode.
- **`TURBOJPEG=0`**: those files are decoded by Apple ImageIO instead (slower; damaged files
  that ImageIO can't read show `CANNOT DECODE` and paging continues). Valid baseline images are
  exactly as fast. Without any optional dependency the viewer needs only macOS frameworks.

The parts of nitrojpeg that need libjpeg-turbo are behind `#ifdef NJ_REFERENCE`: the old
engine where libjpeg-turbo decodes the bands, the CPU BGRX output and the single-threaded
pass experiment. Only the tests and benchmarks use them (`build/nitrojpeg_ref.o`). In the
viewer, the fallback decoder is behind `#ifdef NV_TURBOJPEG`.

The tests and benchmarks look for images in `samples/`: put your own JPEGs there (or a
symlink). The measurements above were made on 50 private photos that are not in the repo.

`-march=native` optimises the binary for the CPU it is built on.

## Files

- `src/nitrojpeg.c/.h`: self-contained parallel JPEG decoder (own Huffman + AVX2 IDCT);
  with `NJ_REFERENCE` also the libjpeg-turbo based comparison code
- `src/nitroview.m`: Cocoa + Metal viewer; with `NV_TURBOJPEG` the TurboJPEG fallback
- `scripts/get-deps.sh`: downloads and builds the optional dependencies
- `bench/bench.m`: decoder benchmark, `bench/verify.c`: bit-exactness + sync statistics,
  `bench/robust.c`: robustness test, `bench/freqprobe.c`: clock measurement,
  `bench/sustain.c`: sustained load, `bench/ab.sh`: noise-resistant A/B comparison
- `src/nitropng.c/.h`: self-contained parallel PNG decoder
- `src/nitropsd.c/.h`: parallel PSD/PSB merged-image decoder
- `src/png_wuffs.c/.h`: optional Wuffs PNG decoding for the viewer (PNG types nitropng skips)
- `bench/pngbench.m`, `bench/pngsplit.c`: PNG decoder comparison and time split,
  `bench/pngverify.c`: nitropng byte-exactness vs zlib, `bench/pngrobust.c`: damaged PNGs (ASan),
  `bench/mkpsd.py`: test PSD writer (every compression), `bench/psdverify.c`: all compressions
  vs raw, `bench/psdrobust.c`: damaged PSDs (ASan)
- `bench/results/`: measured results

## License

MIT, see [LICENSE](LICENSE). The optional dependencies downloaded by `scripts/get-deps.sh`
have their own licenses: libjpeg-turbo (BSD-style / IJG / zlib), Wuffs (Apache-2.0),
stb_image (public domain / MIT). The inverse DCT implements the algorithm of jidctint.c from the
Independent JPEG Group's libjpeg: this software is based in part on the work of the
Independent JPEG Group.
