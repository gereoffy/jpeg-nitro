# zlib / libdeflate measurements — 2026-11-15

No repository source or production build configuration was modified.

## Environment

- Intel Core i9-13900K, macOS 15.7.7 (24G720), x86-64.
- Apple Clang 17.0.0. Default library benchmark flags: -O2 -march=x86-64-v3 -DNDEBUG.
- System zlib: 1.2.12, loaded from /usr/lib/libz.1.dylib, zlibCompileFlags=0x2a9.
- Bundled upstream zlib: 1.3.1, from Nitro third_party/zlib; built locally for macOS
  to compare implementations on the same CPU/OS/compiler. This is not a Windows
  runtime measurement.
- libdeflate: release v1.26, published 2026-08-22. GitHub tarball SHA-256:
  b2dfeb6ac67919118956f3eb46287c882d289b47847001ec859dabecb121958e
- ARM64 was not executed. Results must not be assumed to hold on ARM64 unchanged.

## Data

- PDF: 46,997 successful inflate invocations captured after decryption and preceding
  filters, from 351 supplied PDF files excluding the separate stress-test directory.
  196,469,094 compressed bytes; 1,563,132,199 decoded bytes. Includes 12 successful
  raw recovery invocations. Repeated invocations are retained as workload frequency.
- ZIP CDR: all 11,027 ZIP-based CDR files among 11,715 CDR samples, 58,301 unencrypted
  method-8 members; 1,116,675,881 compressed / 9,748,575,091 decoded bytes.
- ZIP VSDX: all 36 files, 581 unencrypted method-8 members;
  670,523 compressed / 3,824,763 decoded bytes.
- No ZIP member exceeded the 512 MiB extraction cap; none was skipped for size.
- Input packs and path lists are private local test data; do not commit or publish them.

## Decoder-only measurements

Whole input corpus is preloaded. Output buffers are preallocated to the known
uncompressed size. Allocation of input/output buffers, I/O, XML parsing and rendering
are excluded. Three passes, rotated method order, median times, worker QoS
USER_INTERACTIVE. PDF wrapped streams include Adler-32; raw ZIP DEFLATE does not
include a timed CRC-32 check (matching the existing ZIP reader's decode path).

Fresh creates/destroys a decoder per stream; reset/reuse retains decoder state.
The tiny and VSDX reruns repeat each measured batch for >=150 ms per pass; this
avoids timing a whole VSDX corpus that takes only a few milliseconds.

Fresh decoder times, milliseconds:

| Corpus | System zlib | Upstream zlib 1.3.1 | libdeflate 1.26 |
| --- | ---: | ---: | ---: |
| PDF | 997.169 | 2062.339 | 680.364 |
| CDR raw ZIP members | 3849.735 | 7875.150 | 2594.284 |
| VSDX raw ZIP members (stable rerun) | 3.481 | 5.220 | 2.760 |

Thus libdeflate is about 1.47-1.48x faster on PDF/CDR than Apple's system zlib,
and about 3.04-3.06x faster than the project's upstream zlib build. VSDX is about
1.26x / 1.89x respectively; its entire sample corpus is small, so absolute savings
are small.

## Why Apple's zlib is faster

The Apple source tree contains a separate optimized inflate_fast implementation:
https://github.com/apple-oss-distributions/zlib/blob/06673bf7cb4066003fd14a0b87085c05739fc4ac/AddOn/zopt_inffast.c
It uses, among other changes, a 64-bit refill path and different fast decode/copy
logic. Its build enables INFFAST_OPT and VEC_OPTIMIZE:
https://github.com/apple-oss-distributions/zlib/blob/06673bf7cb4066003fd14a0b87085c05739fc4ac/zlib.xcconfig
These are source-fork features, not generic stock-zlib compiler switches.

Upstream 1.3.1, same PDF corpus, fresh decoder, median milliseconds:
- -O2 -march=x86-64-v3: 2062.339
- -O3 -march=x86-64-v3: 2066.519
- -O3 -march=native: 2096.095
- -O3 -march=native -mbranches-within-32B-boundaries: 2091.019
These flag changes do not close the observed gap. The public Apple source explains
why an older zlibVersion string does not imply an older/unoptimized implementation.
The exact system binary was identified via dladdr; the source link is Apple's
published implementation, not a binary-to-source identity proof.

## Tiny PDF streams

Groups are based on COMPRESSED input size. Each size group is benchmarked separately
with hot input and many repetitions. Reusing a decoder also permits internal tables
to be reused where applicable; real mixed-order pipeline results are below.

Median microseconds per stream:

| Input bytes | Count | Apple zlib fresh | Stock zlib fresh | libdeflate fresh | libdeflate reuse |
| --- | ---: | ---: | ---: | ---: | ---: |
| <=32 | 2570 | 0.344 | 0.299 | 1.266 | 0.157 |
| 33-128 | 8011 | 0.962 | 0.965 | 1.646 | 0.901 |
| 129-512 | 12636 | 2.694 | 3.475 | 2.564 | 2.314 |
| 513-4096 | 15996 | 8.951 | 16.194 | 6.998 | 6.813 |

The libdeflate reuse values in the table come from the system-linked run; the
stock-linked run has minor run-to-run differences, all preserved in results.csv.
The decompressor allocation code zeros its state and initializes internal table
flags; repeatedly creating it is a poor choice for tiny inputs. A thread-owned
reused context avoids this cost. At 33-128 bytes there is little gain over zlib
in this workload even with reuse; it is not universally faster at every size.

## Full PDF pipeline experiment

Temporary fast.cpp tries libdeflate only for ordinary 32K-window zlib headers,
with a thread-local decompressor. It guesses output capacity as compressed_size*4
+4096, retries up to five times growing capacity by 4, and falls back to the
unchanged zlib code on failure/limit/other cases. Raw recovery remains zlib.
It does not use an oracle uncompressed size. Guesses, zero filling, reallocations,
retries, parsing and recovery are INCLUDED here. No rendering or I/O is timed.

Three rotated passes; sum of per-file medians, 351 files:

| Workload | zlib | libdeflate + zlib fallback | Time reduction |
| --- | ---: | ---: | ---: |
| System zlib, open/page tree/first content/lexing | 611.072 ms | 576.305 ms | 5.7% |
| System zlib, open/every object/every stream | 2804.522 ms | 2711.745 ms | 3.3% |
| Stock zlib, open/page tree/first content/lexing | 671.780 ms | 606.062 ms | 9.8% |
| Stock zlib, open/every object/every stream | 3973.407 ms | 3410.873 ms | 14.2% |

This is a prototype, not a production integration or exhaustive tuning of buffer
heuristics. ZIP has an advantage here: the uncompressed member size is already
in its directory entry. libdeflate has a different API and is not a drop-in zlib
replacement. The existing zlib is still useful for streaming and tolerant PDF
recovery. The current RVNGZipStream reader already reads the full compressed
member and has its uncompressed size, making it a natural integration point.
The CDR/VSDX decoder microbenchmarks above do NOT measure full document loading
or rendering.

## Validation

- All 105,879 captured PDF/ZIP records: byte-for-byte decoder output comparison,
  output size, CRC against recorded/ZIP metadata, and consumed input size checked.
  No mismatches; consumed input differences zero.
- Experimental full PDF fast path vs unchanged parser: 504 files (all 381 Nitro
  PDF samples plus 123 pdfparse3 regression cases), 60,387 stream visits and
  2,022,086,845 decoded bytes; object/stream/diagnostic/first-content fingerprints
  identical with both system and upstream zlib. No mismatches.
- The experimental pipeline was not installed into the application.

## Files

- bench.cpp: decoder-only corpus benchmark and validation.
- bench-tiny.cpp: repeated-batch/small-input variant.
- bench-options.cpp: compiler-flag diagnostic.
- pdfbench.cpp + fast.cpp: full PDF pipeline experiment.
- validate.cpp + validate-{base,fast}.cpp: extended semantic comparison.
- results.csv: raw aggregate results, with microseconds per stream.
- *-results.txt, *-stable.txt: original logs.
- build.py / build_options.py: temporary local library builds.
