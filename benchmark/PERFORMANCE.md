# Portable performance changes

The public API and C++17 requirement are unchanged. The changes use bounded loads,
unsigned integer arithmetic and standard library facilities; they require neither
CPU-specific flags nor padded input.

- Number scanning and integer conversion share a pass. Long integer runs combine
  eight digits at a time and check the signed range after validation; oversized
  integers retain their original tokens. A bounded scalar path handles input
  tails of up to eight bytes, including floats and exponents, without setting up
  word scanning or rescanning a float token. Eager/lazy variants keep numeric
  scratch state outside recursive value dispatch; read-only floats remain lazy.
- Read-only string sizing consumes consecutive escapes together, avoiding repeated
  calls to the ordinary-text/UTF-8 scanner between adjacent backslashes. In-place
  decoding also groups adjacent escapes while retaining source-position tracking
  before writing decoded newlines.
- String scanning uses a 256-byte constant table for scalar classification and
  pairs bounded eight-byte iterations after the first plain word. Compiler inline
  controls keep the small scanner in its caller and isolate parser scratch state
  and error diagnostics from container loops; unsupported compilers use normal
  C++ `inline`. Borrowed-view object string values bypass recursive type dispatch
  while retaining the depth limit and error precedence. Empty borrowed-view
  containers return their empty metadata directly after the same depth check.
- Dense escape runs use a 256-byte stack buffer to batch output appends. Sparse
  escapes retain direct copying of ordinary spans.
- Compact dumping resumes at the first unhandled root member/element when a nested
  value ends the shallow path. It retains the already-written prefix. Scalar roots
  bypass the traversal stack, whose unused frames no longer need initialization.
- Known arena resources can grow the last allocation in place even without RTTI.
- Big-endian scanners use exact masks to prevent cross-byte borrow/carry from
  reporting a match before its actual position. The flat-array sizing pass stops
  at the end of an unpadded input instead of reading an extra byte.

## Reproduce

Both executables must use the current benchmark source, compiler and flags. The
baseline header for this comparison is commit
`2a3f73d8c5585ddeb11e2041d8549d28f0c31bec`.

```sh
mkdir -p build/perf-baseline/include/chjson
git show 2a3f73d:include/chjson/chjson.hpp > build/perf-baseline/include/chjson/chjson.hpp
cmake -S . -B build/perf -DCMAKE_BUILD_TYPE=Release \
  -DCHJSON_BUILD_BENCHMARKS=ON -DCHJSON_BUILD_CONFIG_TESTS=ON \
  -DCHJSON_BASELINE_INCLUDE="$PWD/build/perf-baseline/include"
cmake --build build/perf --config Release --parallel
ctest --test-dir build/perf -C Release --output-on-failure
python3 benchmark/compare_baseline.py \
  --baseline build/perf/chjson_baseline_bench --candidate build/perf/chjson_bench \
  --runs 5 --samples 3 --extended --short-scalars --output build/perf/results.json
```

On multi-configuration generators, use the executables under `build/perf/Release`.
For a separate no-RTTI comparison, configure another build with
`-DCMAKE_CXX_FLAGS=-fno-rtti` (GCC/Clang) or `/GR-` (MSVC), using the same flag for
both baseline and candidate. CPU affinity is optional and supported only where the
operating system exposes the corresponding facility.

## Validation scope

The regression suite covers all five parse modes, exact integer limits, raw float
tokens, UTF-8 validation, error locations, document ownership, allocation failures,
deep/parallel dumps, every escape byte and output-buffer boundary. The additional
endian tests execute both digit-conversion branches and the production big-endian
mask branches on any host. Unpadded heap inputs exercise scanner bounds under ASan. The short-number
regressions compare 7,381 strings drawn from a numeric grammar alphabet against
the general parser, plus signed values, boundary lengths, raw-token reuse and
prefix parsing. Adjacent-escape tests cover every truncation point in strings
mixing ordinary text, simple escapes, Unicode escapes, surrogate pairs and UTF-8.

The CI workflow configures Windows/MSVC, Linux/GCC and Clang, macOS/Clang, Linux
32-bit, and ASan/UBSan builds. A configured CI matrix is not evidence of completed
remote runs; local validation and measured results are recorded below.

Local validation on Apple M4 Max / macOS with Apple Clang 17.0.0:

| Build | Result |
| --- | --- |
| ARM64 Release | 7/7 CTest targets passed |
| ARM64 ASan + UBSan (`-O2 -gline-tables-only`) | 7/7 passed |
| x86_64 Release, executed through Rosetta | 7/7 passed |

Each group includes default parsing, allocation-failure injection, disabled caches,
internal allocator, `strtod` fallback, low MT thresholds and disabled RTTI. The x86_64
run validates a separately compiled architecture but is not a native x86 performance
measurement. Windows, Linux, 32-bit and a physical big-endian host were not executed
locally.

## Measured results (2026-10-13, follow-up)

Apple M4 Max, macOS ARM64, Apple Clang 17.0.0 / libc++, `-O3 -DNDEBUG
-std=gnu++17 -arch arm64`. Both binaries use identical flags and benchmark source.
Each full-matrix result is the median of three process samples, each using five
timed runs; baseline/candidate process order alternates. No affinity was applied.
[Follow-up machine-readable results](results/2026-10-13-arm64-followup.json) include
all 27 workloads against both the original and previous-round headers, absolute
throughput, every process sample, arena sizes, arguments, and source/binary hashes.
The [first-round results](results/2026-10-13-arm64.json) remain available separately.

Ratios below are candidate throughput / baseline throughput: **higher is faster**.
`parse(in_situ)` and the two read-only modes reuse a document; `dump(reuse)` reuses
its output buffer. Other modes construct their results on each iteration.

### Against the original header

| Workload | Parse DOM | In situ | View reuse | Owning view reuse | Dump | Dump reuse |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| scalar | 1.06x | 1.13x | 1.20x | 1.14x | 8.54x | 10.63x |
| small_object | 1.04x | 1.06x | 1.02x | 1.07x | 0.95x | 0.98x |
| objects | 1.06x | 1.11x | 1.03x | 1.07x | 1.00x | 0.98x |
| integers | 1.08x | 1.13x | 1.08x | 1.07x | 0.96x | 1.03x |
| floats | 1.00x | 0.98x | 1.01x | 1.03x | 0.97x | 0.98x |
| empty_containers | 1.04x | 1.05x | 1.09x | 1.00x | 0.99x | 0.99x |
| strings | 1.02x | 1.04x | 1.06x | 1.00x | 0.97x | 0.95x |
| large_objects | 1.07x | 1.09x | 1.11x | 1.14x | 0.99x | 0.98x |
| escaped_strings | 1.06x | 1.06x | 1.06x | 1.04x | 0.97x | 0.97x |
| dense_escapes | 1.17x | 1.17x | 1.32x | 1.30x | 1.75x | 1.70x |
| utf8_strings | 0.94x | 0.92x | 1.16x | 1.03x | 0.98x | 0.95x |
| mt_objects_2m | 1.02x | 1.09x | 1.09x | 1.11x | 0.99x | 0.99x |
| mt_objects_8m | 1.00x | 1.09x | 1.11x | 1.11x | 1.01x | 0.99x |
| long_integers | 1.80x | 1.81x | 1.84x | 1.74x | 0.95x | 0.94x |
| late_nested_array | 1.05x | 1.04x | 1.00x | 1.08x | 2.02x | 2.03x |
| late_nested_object | 0.98x | 0.98x | 0.93x | 1.00x | 1.98x | 2.00x |
| scalar_zero | 1.05x | 1.07x | 1.16x | 1.11x | 9.17x | 12.58x |
| scalar_negative | 1.16x | 1.16x | 1.23x | 1.20x | 7.98x | 10.35x |
| scalar_seven_digits | 1.24x | 1.23x | 1.32x | 1.27x | 7.68x | 9.72x |
| scalar_eight_digits | 1.28x | 1.22x | 1.38x | 1.26x | 8.20x | 10.86x |
| scalar_nine_digits | 1.27x | 1.35x | 1.36x | 1.21x | 8.23x | 10.78x |
| scalar_float | 1.03x | 1.05x | 1.07x | 1.00x | 10.23x | 14.17x |
| scalar_exponent | 1.04x | 1.11x | 1.07x | 1.03x | 9.54x | 12.72x |
| scalar_true | 0.95x | 1.20x | 1.26x | 1.18x | 10.42x | 14.64x |
| scalar_null | 1.01x | 1.26x | 1.35x | 1.15x | 10.53x | 14.79x |
| scalar_string | 1.00x | 1.06x | 1.08x | 1.04x | 7.15x | 8.56x |
| scalar_escaped | 0.97x | 1.04x | 1.06x | 1.02x | 5.41x | 6.18x |

The previously regressed `scalar` workload (`123`) now improves by 6% in DOM,
13% in situ and 14–20% in the read-only modes relative to the original header.
Dense-escape read-only parsing improves by 30–32%. The short-scalar extension
covers zero, signed integers, the seven/eight/nine-digit boundaries, decimals,
exponents, literals and ordinary/escaped strings; its read-only ratios are all
at least 1.00x in this run.

The earlier major gains remain: long-integer parsing is 1.73–1.84x, dense-escape
serialization is 1.70–1.75x, and serialization with late nesting is about 2.00x.
Empty-container borrowed-view parsing is 1.09x after bypassing recursive dispatch
for adjacent `[]` and `{}` while preserving the depth check.

### Against the previous optimization round

Both headers were rebuilt with the current benchmark source and identical flags.
The previous-round header SHA-256 is
`ff6363a73644c38795419e5ec3cd3662a9916e983506def9929c5722e527ee72`.
The full raw report also includes the 11 extra short-scalar workloads.

| Workload | Parse DOM | In situ | View reuse | Owning view reuse | Dump | Dump reuse |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| scalar | 1.11x | 1.17x | 1.37x | 1.27x | 1.02x | 1.02x |
| small_object | 1.03x | 1.05x | 0.99x | 1.06x | 1.01x | 1.00x |
| objects | 1.08x | 1.08x | 1.01x | 1.05x | 1.02x | 0.99x |
| integers | 1.19x | 1.16x | 1.17x | 1.15x | 0.99x | 0.97x |
| floats | 0.95x | 0.96x | 0.94x | 0.96x | 0.99x | 1.00x |
| empty_containers | 1.04x | 1.04x | 1.09x | 0.98x | 1.00x | 1.01x |
| strings | 1.02x | 1.07x | 1.06x | 1.00x | 0.96x | 0.95x |
| large_objects | 1.04x | 1.03x | 1.13x | 1.12x | 1.00x | 1.00x |
| escaped_strings | 1.06x | 1.07x | 1.04x | 1.03x | 0.98x | 0.98x |
| dense_escapes | 1.06x | 1.04x | 1.17x | 1.14x | 1.00x | 0.99x |
| utf8_strings | 1.10x | 0.82x | 1.02x | 1.02x | 0.97x | 0.97x |
| mt_objects_2m | 0.99x | 1.07x | 1.11x | 1.14x | 0.99x | 0.99x |
| mt_objects_8m | 1.00x | 1.06x | 1.13x | 1.13x | 1.04x | 0.99x |
| long_integers | 1.07x | 1.08x | 1.07x | 1.07x | 0.97x | 0.96x |
| late_nested_array | 1.06x | 1.07x | 1.01x | 1.06x | 0.98x | 1.01x |
| late_nested_object | 1.01x | 0.99x | 0.99x | 1.01x | 0.99x | 1.00x |

Relative to that round, the original short scalar gains 11% in DOM and 27–37%
in read-only parsing; dense-escape read-only parsing gains 14–17%. Long integers
gain another 7–8%. Dense-escape and late-nesting serialization retain approximately
the previous throughput.

### Variability and remaining tradeoffs

These are measurements on a shared workstation. The full matrix includes smaller
regressions: for example `scalar_true` DOM is 0.95x and `scalar_escaped` DOM is
0.97x against the original. Some ordinary dump workloads measure 0.94–0.98x.
Against the previous round, float parsing measures 0.94–0.96x and UTF-8 in-situ
parsing measures 0.82x. All of these results are retained in the report; the data
supports the targeted scalar/escape improvements and the large existing gains,
but does not establish a speedup for every workload or compiler.

UTF-8 and late-nested-object parsing showed substantial variation between process
samples, so both were repeated against the original header with **nine** process
pairs and five timed runs per process. The original three-sample matrix and this
additional measurement are both recorded:

| Workload | Parse DOM | In situ | View reuse | Owning view reuse | Dump | Dump reuse |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| utf8_strings | 1.05x | 1.00x | 0.96x | 1.00x | 0.96x | 0.97x |
| late_nested_object | 0.98x | 0.98x | 0.98x | 1.03x | 1.97x | 1.98x |

Recheck on the target compiler, hardware and payloads before treating differences
of a few percent as stable. The portability validation scope is listed above;
these throughput ratios apply only to the local ARM64 build.

### RTTI disabled

The same original-header comparison with `-fno-rtti` on both binaries:

| Workload | Parse DOM ratio | Cold arena committed, before | After |
| --- | ---: | ---: | ---: |
| objects | 1.11x | 1,310,805 | 436,935 |
| integers | 1.12x | 480,000 | 480,000 |
| empty_containers | 0.99x | 1,680,021 | 1,680,021 |

The object workload still commits 67% less arena storage because growth can reuse
the last arena allocation without RTTI. Default-configuration arena figures are
unchanged across the measured cases. These storage counters are not process RSS.
