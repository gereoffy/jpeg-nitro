# zlib-ng measurement — 2026-11-15

No production source or build configuration was changed. Test artifacts are under
/private/tmp/zlib-ng-review; prior corpus and harness artifacts are under
/private/tmp/deflate-review. Private PDF/ZIP packs and path lists must not be committed.

## Environment and build

Intel i9-13900K, x86-64 macOS 15.7.7, Apple Clang 17. The library implementations
are compared on this same machine. Windows, Linux and ARM64 execution was not measured.

zlib-ng release 2.3.3 (2026-02-03), GitHub release tarball SHA256:
92c0dd38b1548debf6c4b2d7b5aa16e4416ce5df6ddd405fa96431eb7aaa4a09
Source: https://github.com/zlib-ng/zlib-ng/releases/tag/2.3.3

CMake build: ZLIB_COMPAT=ON, BUILD_SHARED_LIBS=OFF, BUILD_TESTING=OFF,
WITH_RUNTIME_CPU_DETECTION=ON, WITH_NATIVE_INSTRUCTIONS=OFF,
CMAKE_C_COMPILER=clang, CMAKE_BUILD_TYPE=Release,
CMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG -march=x86-64-v3'.
Default architecture optimizations were enabled, including SIMD implementations
selected at runtime. The generic compilation baseline is still x86-64-v3:
for an older-CPU production build, its baseline flags must be lowered too.

The generated compatible archive is build/libz.a; zlibVersion() reports
1.3.1.zlib-ng. Benchmark and frozen PDF parser objects were compiled against the
ordinary system zlib header and linked to the compatible archive unchanged.
This tests compatibility for the exercised calls, not every possible zlib API.
Production integration should use the matching generated headers/archive.

Reference builds: bundled upstream zlib 1.3.1 with identical O2/v3/NDEBUG flags;
Apple system zlib 1.2.12; libdeflate 1.26 with identical O2/v3/NDEBUG flags.
Reference decoder-only timings were measured in the preceding experiment on the
same day and corpus. libdeflate was also remeasured in the zlib-ng executable.

## Decoder-only results

Same preloaded packs, exact-size preallocated output buffers, 3 rotated passes,
median. Fresh decoder allocation/destruction per stream. File I/O, output allocation,
XML parsing and rendering excluded. PDF includes Adler-32; raw ZIP timings exclude
CRC-32, matching the existing ZIP reader.

| Corpus | Streams | Upstream zlib ms | Apple zlib ms | zlib-ng ms | libdeflate ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| PDF | 46,997 | 2062.339 | 997.169 | 893.179 | 682.899 |
| ZIP CDR | 58,301 | 7875.150 | 3849.735 | 3704.588 | 2649.423 |
| ZIP VSDX | 581 | 5.220 | 3.481 | 3.265 | 2.787 |

The VSDX batch was repeated for at least 150ms per pass. The PDF/CDR improvements
relative to upstream are 2.31x/2.13x. zlib-ng is close to Apple's implementation.
libdeflate remains faster for whole-buffer decompression with a known output size.

Every one of the 105,879 records was validated with full byte comparison against
libdeflate plus the captured reference CRC. libdeflate consumed-byte counts matched
the original capture for every record. zlib-ng inflateReset2 reuse was also exercised;
its large-corpus gains over fresh state were negligible (PDF) to ~3% (CDR).

## Tiny PDF streams

Compressed input size groups, hot data, >=150ms repeats per pass, fresh decoder
per stream. Microseconds per call, including init/end:

| Compressed bytes | Count | Upstream zlib | Apple zlib | zlib-ng |
| --- | ---: | ---: | ---: | ---: |
| <=32 | 2570 | 0.299 | 0.344 | 0.136 |
| 33–128 | 8011 | 0.965 | 0.962 | 0.734 |
| 129–512 | 12636 | 3.475 | 2.694 | 2.084 |
| 513–4096 | 15996 | 16.194 | 8.951 | 8.841 |

Tiny-bin results are not a full parser latency prediction. No decoder reuse is
needed to obtain these zlib-ng results.

## Unmodified PDF parser

351 preloaded PDFs, same frozen base.o and crypt.o, linked to one of the three
zlib libraries. Three passes, per-file median, summed. No libdeflate fast path,
parser code change, guessed output size or rendering. run-rest.sh records commands.

| Operation | Upstream zlib ms | Apple zlib ms | zlib-ng ms |
| --- | ---: | ---: | ---: |
| Open, page tree, first-page contents and operator lexer | 675.985 | 637.788 | 602.437 |
| Open and decode all object streams | 3933.180 | 2740.148 | 2817.589 |

Relative to upstream: ~10.9% less time for the first-page path and ~28.4% less
for all streams. Apple's whole-parser performance is similar; there is no clear
reason here to replace the macOS system library for performance.

The prior libdeflate fast-path prototype saved ~14% on the all-stream upstream
pipeline; zlib-ng's ~28% comes from linking the unchanged streaming parser. The
known-size decoder-only libdeflate advantage does not translate directly to this
parser, which does not know decoded output size and uses 64KiB output chunks.
CDR/VSDX complete application loading and rendering have not been benchmarked.

## Compatibility and damaged data

504 PDFs, 60,387 stream visits, 2,022,086,845 decoded bytes:
zlib-ng vs upstream 1.3.1 matched all complete per-file fingerprints, including
object values, decoded bytes, first-page contents, success status and diagnostics.

Against Apple zlib, fingerprints differ only on three intentionally damaged line
ending fixtures (cases 438–440, transfer_crlf, transfer_crlf_naive and
transfer_crlf_textheader). A stream's decoded byte hash and success status match;
the error text is 'invalid literal/length code' in upstream/zlib-ng and
'invalid literal/length/distance code' in Apple. diagnostics-*.txt and the three
per-fixture dumps record this. This is an existing Apple-vs-upstream wording
difference, not a zlib-ng regression.

checksum-fixed.cpp reads the SAME system-zlib-compressed fixture for both library
runs. Complete checksum; 0/1/2/3 checksum bytes; low16 only; two bytes plus CRLF;
wrong low16; wrong high16; and a cut DEFLATE body were tested. Complete logs match
byte-for-byte for Apple and zlib-ng. The unchanged parser preserves its missing or
bad Adler-32 recovery and its partial-output recovery for a genuinely truncated
DEFLATE stream. The wrapped decoder itself still requires the checksum, as zlib does.

## Recommendation

Use zlib-ng's ZLIB_COMPAT mode instead of the bundled upstream library for
Windows/Linux builds, subject to platform build/regression checks. The measurements
support this as a lower-intrusion first step than adding libdeflate to the parser.
Keep the macOS system library. zlib-ng documents x86 SIMD and ARM64/NEON paths plus
runtime dispatch; speed on ARM64 remains to be measured.

Compatibility mode is not the default: explicitly set ZLIB_COMPAT=ON. Keep runtime
CPU detection ON and native instructions OFF for distributed binaries. Build-system
integration must compile its architecture/configuration sources (e.g. via CMake),
not reuse the six-file upstream-zlib source list unchanged.

Official build/API/intrinsics documentation:
https://github.com/zlib-ng/zlib-ng/blob/2.3.3/README.md
