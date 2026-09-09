# chjson

A header-only **C++17** JSON library with an arena-backed DOM, strict UTF-8 parsing, and optional parallel parsing/serialization.

Design goals:

- **Fast enough in practice** (arena-backed DOM, optional MT parse/dump).
- **Strict JSON by default** (RFC 8259-style JSON, no JSON5 extensions).
- **High-quality errors** (error code + byte offset + line/column).

This folder contains the standalone `chjson` library (header + tests/benchmarks).

---

## Contents

- [Quick start](#quick-start)
- [Public header](#public-header)
- [Parsing APIs](#parsing-apis)
- [DOM models](#dom-models)
- [Dumping (serialization)](#dumping-serialization)
- [Error handling](#error-handling)
- [Configuration macros](#configuration-macros)
- [Build with CMake](#build-with-cmake)
- [Examples](#examples)

---

## Quick start

```cpp
#include <chjson/chjson.hpp>

#include <iostream>
#include <string_view>

int main() {

  std::string_view json = R"({"a":[1,2,3],"s":"ok"})";

  auto r = chjson::parse(json);
  if (r.err) {
    std::cerr << "parse failed: code=" << static_cast<int>(r.err.code)
              << " offset=" << r.err.offset
              << " line=" << r.err.line
              << " column=" << r.err.column << "\n";
    return 1;
  }

  const chjson::sv_value& root = r.doc.root();
  const chjson::sv_value* a = root.find("a");
  if (a && a->is_array()) {
    std::cout << "a[0]=" << a->as_array()[0].as_int() << "\n";
  }

  std::cout << chjson::dump(root, /*pretty=*/true) << "\n";
  return 0;
}
```

---

## Public header

Include:

```cpp
#include <chjson/chjson.hpp>
```

`chjson` is header-only; you only need to add the `include/` directory to your include paths.

---

## Parsing APIs

`chjson` provides multiple parse entry points depending on the lifetime/ownership model you want.

### Which parse API should I use?

- Use `parse()` when you want the recommended default: an owning `document` with good all-around performance.
  - It may use MT parsing for large top-level arrays/objects.
  - If the input contains no strings (no `"`), it may take a no-string fast path (see below).
- Use `parse_in_situ()` when you can provide a mutable `std::string` buffer and want maximum throughput.
  - This can be the fastest mode because it avoids copying strings/keys and can decode escapes in-place.
  - Many `string_view`s will point into `document::buffer()`.
- Use `parse_view()` when you want a non-mutating parse and can guarantee the source buffer outlives the parsed `view_document`.
  - Unescaped strings/keys may be zero-copy `string_view`s into the source.
- Use `parse_owning_view()` when you have read-only input but still need an owning `document`, and you want to avoid copying the full JSON text.
  - Strings/keys are copied into the arena; `document::buffer()` is not used.

Notes about numbers:

- `sv_value` may preserve the raw token for non-integer numbers to enable exact round-trip `dump()` and lazy `double` parsing.
- When parsing inputs with no strings, `parse()` can still preserve float tokens efficiently by allocating + copying the full input into the arena at most once (on-demand) and storing token views into that backing.

### Primary API: `parse()` (owning `document`)

```cpp
namespace chjson {

  struct parse_options {
    std::size_t max_depth{256};
    bool require_eof{true};
  };

  struct document_parse_result {
    document doc;
    error err;
  };

  document_parse_result parse(std::string_view json, parse_options opt = {});
}
```

What it does:

- Parses into an arena-backed DOM (`sv_value`) stored inside an owning `document`.
- By default, `document` owns a backing buffer, so many strings/keys can be returned as `std::string_view`.
  - Note: `document::buffer()` is an implementation detail of the chosen parse path. In particular, `parse_owning_view()` (and `parse()` when it takes the no-string fast path) may produce a valid owning DOM without storing the full original JSON text in `document::buffer()`.
- For inputs that contain **no `"` characters** (i.e. there are no JSON strings at all), `parse()` may use a **no-string fast path**:
  - It parses from the read-only input without storing `document::buffer()`.
  - Numbers are parsed eagerly into `int64_t` / `double`.
  - For non-integer numbers, `chjson` preserves the original raw token for exact round-trip dumping. To do that efficiently, the parser may allocate + copy the full input **once** into the arena on-demand and store number token views into that backing.
  - For very large float-heavy inputs, `parse()` may prefer keeping a backing buffer (so number tokens can be views into the single buffer copy).
- For large top-level arrays/objects, `parse()` may use a multithreaded path (see [Configuration macros](#configuration-macros)).

### In-situ parse: `parse_in_situ()`

```cpp
chjson::document_parse_result parse_in_situ(std::string json, chjson::parse_options opt = {});
chjson::error parse_in_situ_into(chjson::document& d, std::string_view json, chjson::parse_options opt = {});
```

What it does:

- Parses by **mutating** the provided buffer (escape decoding can write into the buffer).
- Keys and unescaped string values can be `string_view`s pointing into `document::buffer()`.

### Non-mutating view parse: `parse_view()` (non-owning source)

```cpp
chjson::view_document_parse_result parse_view(std::string_view json, chjson::parse_options opt = {});
chjson::error parse_view_into(chjson::view_document& d, std::string_view json, chjson::parse_options opt = {});
```

What it does:

- Does **not** copy or mutate the input.
- Unescaped strings/keys may be `string_view`s into the original source.
- Escaped strings are decoded into the document arena.

Important: the input buffer backing `json` must outlive the `view_document` (or any `string_view` extracted from it).

### Read-only input, owning result: `parse_owning_view()`

```cpp
chjson::document_parse_result parse_owning_view(std::string_view json, chjson::parse_options opt = {});
chjson::error parse_owning_view_into(chjson::document& d, std::string_view json, chjson::parse_options opt = {});
chjson::document parse_owning_view_or_throw(std::string_view json, chjson::parse_options opt = {});
```

What it does:

- Parses from read-only input **without copying the full JSON text** into the document buffer.
- Strings/keys are copied into the arena.
- Non-integer numbers may preserve a raw token and parse to `double` lazily on first `as_double()`.

### Parallel helper: `parse_many_into()`

```cpp
namespace chjson {

  struct parse_many_options {
    parse_options opt{};
    unsigned max_threads{0}; // 0 => hardware_concurrency()
  };

  void parse_many_into(document_parse_result* out,
                       const std::string_view* inputs,
                       std::size_t count,
                       parse_many_options options = {});
}
```

This is a throughput helper for parsing many independent JSON documents concurrently. Workers are reused through a shared thread pool. Parsing within a pool worker runs serially to avoid nested waits and extra worker creation. Scheduling failures may throw; all already-started tasks finish before the call exits.

---

## DOM models

`chjson` ships two DOM representations:

### 1) Arena-backed DOM: `sv_value` (recommended)

Used by: `parse()`, `parse_in_situ()`, `parse_view()`, `parse_owning_view()`.

Key properties:

- Most string values are `std::string_view` (either into a backing buffer/source or into the arena).
- Numbers can be stored as:
  - `int64_t` when representable
  - or a raw token (preserved for exact dump + lazy `double` conversion)

Core API (selected):

```cpp
namespace chjson {

  struct sv_value {
    enum class kind : std::uint8_t { null, boolean, number, string, array, object };

    kind type() const noexcept;
    bool is_null() const noexcept;
    bool is_bool() const noexcept;
    bool is_number() const noexcept;
    bool is_string() const noexcept;
    bool is_array() const noexcept;
    bool is_object() const noexcept;

    bool as_bool() const;

    bool is_int() const noexcept;
    std::int64_t as_int() const;
    double as_double() const;
    double as_double_unchecked() const; // UB if not number; allocation failures may throw

    std::string_view as_string_view() const;

    sv_array_view as_array() const;
    sv_object_view as_object() const;

    const sv_value* find(std::string_view key) const noexcept;
    sv_value* find(std::string_view key) noexcept;

    // Programmatic construction (storage uses a std::pmr::memory_resource*):
    static sv_value make_array(std::pmr::memory_resource*);
    static sv_value make_object(std::pmr::memory_resource*);
    void array_push_back(std::pmr::memory_resource*, sv_value&&);
    void object_emplace_back(std::pmr::memory_resource*, std::string_view key, sv_value&&);
  };
}
```

Notes:

- Object member order is preserved; `find()` returns the **first** matching key if duplicates exist.
- `object_emplace_back()` stores the key as a `std::string_view`; if you build DOM values programmatically, ensure the key storage outlives the DOM (or store/copy it into the arena yourself).
- For non-integer numbers stored as raw tokens, `as_double()` parses the token lazily on first call and caches the resulting `double` inside the value for subsequent calls. `as_double_unchecked()` has the same caching behavior but skips type checking.


### Storage reuse and explicit release

Small arenas start at 1 KiB. Empty containers and scalar roots do not reserve container storage. Containers grow their last arena allocation in place when space is available; other growth retains the usual monotonic arena lifetime. Large flat numeric arrays are counted before allocation, avoiding abandoned growth buffers. Long string arrays reserve space for DOM elements separately from the input text.

```cpp
chjson::document doc;
auto err = chjson::parse_in_situ_into(doc, R"({"items":[1,2,3]})");
doc.clear();                 // invalidate the DOM, retain buffer/arena capacity
doc.reset();                 // invalidate the DOM and release this document's storage
chjson::release_thread_caches(); // release idle parse, number scratch, and MT dump caches on this thread
```

`release_thread_caches()` does not invalidate live documents, release another thread's caches, or stop the shared worker pool. With the experimental internal allocator enabled, pool pages still remain allocated until process exit. Allocator-level release does not guarantee an immediate reduction in process RSS.

Parsed strings/keys and number tokens remain valid when an owning `document` is moved, including short inputs. The moved-from document has a null root and can be reused. `clear()`, `reset()`, reparsing, and backing-buffer reallocation invalidate existing views. A `view_document` still requires its original source to remain alive.

Numbers stored as raw tokens cache their `double` conversion on access. Concurrent first-time numeric access to the same value requires external synchronization. `as_double()` and `as_double_unchecked()` may throw if an unusually long token needs scratch storage and allocation fails.

### 2) Fully owning DOM: `value` (legacy / convenient construction)

Used by: `parse_value()`.

Key properties:

- Strings are `std::string`.
- Arrays/objects are `std::vector`.
- Useful when you want deep copies and easy C++ value semantics.

API entry points:

```cpp
namespace chjson {

  struct value_parse_result { value val; error err; };
  value_parse_result parse_value(std::string_view json, parse_options opt = {});
  value parse_value_or_throw(std::string_view json, parse_options opt = {});
}
```

---

## Dumping (serialization)

### Default `dump()` (MT-aware)

```cpp
std::string chjson::dump(const chjson::sv_value& v, bool pretty = false);
std::string chjson::dump(const chjson::value& v, bool pretty = false);
```

Default `dump()` will only parallelize when the top-level container is large enough; for small values it behaves like a normal single-thread dumper.

### Opt-in explicit multithreaded dump

```cpp
namespace chjson {

  struct dump_mt_options {
    bool pretty{false};
    unsigned max_threads{0};
    std::size_t min_parallel_items{1024};
    unsigned max_parallel_depth{1};
  };

  std::string dump_mt(const sv_value& v, dump_mt_options opt = {});
  std::string dump_mt(const value& v, dump_mt_options opt = {});
}
```

---

## Error handling

Syntax errors and allocation failures inside the public parse functions are returned through the error object. Exceptions from argument construction, allocation-dependent debug STL result construction, or OS thread creation can still propagate. Allocation failures report `out_of_memory` with offset 0, line 1, column 1. All parse results include an error object:

```cpp
namespace chjson {

  enum class error_code {
    ok = 0,
    unexpected_eof,
    invalid_value,
    invalid_number,
    invalid_string,
    invalid_escape,
    invalid_unicode_escape,
    invalid_utf16_surrogate,
    expected_colon,
    expected_comma_or_end,
    expected_key_string,
    trailing_characters,
    nesting_too_deep,
    out_of_memory
  };

  struct error {
    error_code code{error_code::ok};
    std::size_t offset{0};
    std::size_t line{1};
    std::size_t column{1};
    constexpr explicit operator bool() const noexcept;
  };
}
```

Typical pattern:

```cpp
auto r = chjson::parse(json);
if (r.err) {
  // r.err.code / r.err.offset / r.err.line / r.err.column
}
```

Some APIs throw intentionally:

- Type mismatches like `as_int()` on a non-int number.
- `parse_value_or_throw()` / `parse_owning_view_or_throw()`.
- Dumping NaN/Inf as JSON numbers (JSON disallows them).

---

## Configuration macros

All macros must be defined **before** including `<chjson/chjson.hpp>`.

### Floating-point parsing backend

- `CHJSON_USE_FROM_CHARS_DOUBLE` (default `1`)
  - When set to `1`, uses locale-independent, allocation-free `std::from_chars` after the optional chfloat backend. The fallback uses a private C numeric locale; changing the process locale does not change JSON number parsing.
- `CHJSON_USE_CHFLOAT` (default `1`)
  - Uses the vendored `chfloat` backend when available (often faster on MSVC).
  - Back-compat alias: `CHJSON_USE_FAST_FLOAT`.

### Thread-local caches (single-shot parse)

- `CHJSON_USE_TLS_PARSE_CACHE` (default `1`)
- `CHJSON_TLS_PARSE_BUFFER_MAX` (default `4 MiB`)
- `CHJSON_TLS_PARSE_ARENA_MAX` (default `16 MiB`)
- `CHJSON_PARSE_DOM_ARENA_RESERVE_MAX` (default `16 MiB`)
- `CHJSON_TLS_PARSE_MT_BACKING_MAX` (default `16 MiB`)
- `CHJSON_TLS_PARSE_MT_RESOURCES_MAX` (default `64`)

These caches primarily target workloads that call `chjson::parse()` repeatedly in a tight loop.

### Parse(dom) fast paths / MT thresholds

- `CHJSON_PARSE_DOM_NO_STRING_FASTPATH` (default `1`)
  - Enables a special fast path for inputs with **no `"` characters** (no JSON strings).
- `CHJSON_PARSE_MT_MIN_BYTES` (default `1 MiB`)
- `CHJSON_PARSE_MT_MIN_SPANS` (default `64`)
- `CHJSON_PARSE_MT_MIN_AVG_SPAN_BYTES` (default `2 KiB`)
- `CHJSON_PARSE_MT_SCAN_SIMD` (default `0`)
  - If fixed worker slices remain too small after retrying, parsing falls back to the growable serial arena. A real allocation failure still reports `out_of_memory`.
- `CHJSON_DUMP_MT_MIN_ITEMS` (default `1024`)
- `CHJSON_DUMP_MT_MIN_BYTES` (default `1 MiB`)
  - Both thresholds must be met for automatic parallel serialization. Explicit `dump_mt()` uses its own options.

### Internal allocation / number scratch

- `CHJSON_USE_INTERNAL_ALLOCATOR` (default `0`)
  - Opts into the experimental sized page allocator. Its pages remain allocated until process exit. The default uses the system allocator plus bounded parse caches, so releasing documents/caches can return their storage to the system allocator.
- `CHJSON_TLS_LONG_NUMBER_SCRATCH_MAX` (default `256 KiB`)
  - Caps the per-thread scratch buffer used for parsing very long floating-point tokens (NUL-termination workaround for `strtod`).

---

## Build with CMake

This directory contains a simple CMake project that exposes an **INTERFACE** library target named `chjson`.

### Consume as a subdirectory

```cmake
add_subdirectory(path/to/chjson/chjson)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE chjson)
```

Options:

- `CHJSON_BUILD_TESTS` (default `ON`)
- `CHJSON_BUILD_BENCHMARKS` (default `OFF`)
- `CHJSON_BUILD_COMPARE_BENCH` (default `OFF`)
- `CHJSON_BUILD_CONFIG_TESTS` (default `OFF`): additionally test disabled caches/fast path, the optional allocator/SIMD splitter, the C-locale number fallback, and deliberately small MT slices.
- `CHJSON_ENABLE_SANITIZERS` (default `OFF`): ASan + UBSan on GCC/Clang, or ASan on MSVC.
- `CHJSON_BASELINE_INCLUDE` (default empty): a previous version's include directory; builds `chjson_baseline_bench` from the same benchmark source and compiler/runtime flags.

---


### Build and verify

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCHJSON_BUILD_BENCHMARKS=ON -DCHJSON_BUILD_CONFIG_TESTS=ON
cmake --build build/release --config Release --parallel
ctest --test-dir build/release -C Release --output-on-failure
```

For GCC/Clang on Linux, add `-DCHJSON_ENABLE_SANITIZERS=ON` in a separate Debug build. Tests cover every parse mode, UTF-8 boundaries, document moves, depth limits, raw number preservation, allocation failure injection, deep dumps, worker exceptions, deterministic round trips, and input mutations. The isolated OOM test disables MSVC iterator proxies because those debug-only STL allocations can occur inside `noexcept` operations.

The benchmark accepts `count iterations runs workload [string_length]`. Workloads are `objects` (default), `integers`, `floats`, `strings`, `escaped_strings`, `utf8_strings`, `empty`, and `scalar`. Timings use the median of repeated runs; cold arena storage and reused arena storage are reported separately from throughput. Neither metric is process RSS or total memory, and MT backing storage is separate from the main arena.

```sh
./build/release/chjson_bench 20000 500 7 floats
python benchmark/compare_baseline.py --baseline /path/to/baseline/chjson_bench --candidate /path/to/new/chjson_bench --output benchmark-results.json
```

Both comparison executables must contain the same benchmark source and use the same compiler, runtime library, and optimization flags. Set `-DCHJSON_BASELINE_INCLUDE=/path/to/old/include` to build `chjson_baseline_bench` alongside the current benchmark with matching flags. The comparison script records process failures as well as successful measurements. Use `--samples 3` to alternate multiple independent process pairs and take their median, `--extended` for Unicode, escapes and larger MT payloads, and optionally `--affinity-mask` to keep both processes on the same available logical CPUs. See the [second optimization report](OPTIMIZATION_ROUND2_REPORT.md) and [first optimization report](OPTIMIZATION_REPORT.md) for measured results and limitations.

## Examples

### Example 1: Parse + query + dump (recommended DOM)

```cpp
#include <chjson/chjson.hpp>

#include <iostream>
#include <string_view>

int main() {

  std::string_view json = R"({"a":[1,2,3],"b":{"x":true,"y":null},"s":"ok"})";

  auto r = chjson::parse(json);
  if (r.err) return 1;

  // Note: `parse()` may choose different internal strategies; do not assume
  // `r.doc.buffer()` always contains the full original JSON text.

  const chjson::sv_value& root = r.doc.root();
  const chjson::sv_value* a = root.find("a");
  if (a && a->is_array()) {
    std::cout << "a.size=" << a->as_array().size() << "\n";
  }

  std::cout << chjson::dump(root) << "\n";
}
```

### Example 2: In-situ parse (string/key views into the buffer)

```cpp
#include <chjson/chjson.hpp>

#include <string>
#include <string_view>

int main() {

  std::string json = R"({"plain":"ok","esc":"a\nB","arr":["x","y"]})";

  auto r = chjson::parse_in_situ(std::move(json));
  if (r.err) return 1;

  // In-situ parsing is the most common mode where many string_views point into
  // `r.doc.buffer()` (because the input buffer is owned and mutated in-place).
  std::string_view buf = r.doc.buffer();
  (void)buf;

  const chjson::sv_value* plain = r.doc.root().find("plain");
  if (plain) {
    std::string_view s = plain->as_string_view();
    (void)s;
  }
}
```

### Example 3: Non-mutating parse_view (zero-copy for unescaped strings)

```cpp
#include <chjson/chjson.hpp>

#include <string>
#include <string_view>

int main() {

  std::string json = R"({"plain":"ok","esc":"a\nB"})";

  auto r = chjson::parse_view(json);
  if (r.err) return 1;

  // Unescaped strings may view into `json`.
  const chjson::sv_value* plain = r.doc.root().find("plain");
  if (plain) {
    std::string_view sv = plain->as_string_view();
    (void)sv;
  }
}
```

### Example 4: Programmatic construction with `sv_value`

```cpp
#include <chjson/chjson.hpp>

#include <iostream>

int main() {

  chjson::document d;
  auto* mr = d.resource();

  chjson::sv_value root = chjson::sv_value::make_object(mr);
  root.object_emplace_back(mr, "k", chjson::sv_value("v"));

  chjson::sv_value arr = chjson::sv_value::make_array(mr);
  arr.array_push_back(mr, chjson::sv_value::integer(1));
  arr.array_push_back(mr, chjson::sv_value::number(3.5));
  root.object_emplace_back(mr, "a", std::move(arr));

  d.root() = std::move(root);
  std::cout << chjson::dump(d.root(), /*pretty=*/true) << "\n";
}
```
