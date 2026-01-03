# chjson

A small, performant, header-only **C++17** JSON library focused on:

- **Strict JSON** parsing (RFC-style; no comments / no trailing commas)
- **High-quality errors** (error code + byte offset + line/column)
- **Two DOM models**:
  - Owning DOM (`chjson::value`) — copies strings
  - Zero-copy in-situ DOM (`chjson::document` + `chjson::sv_value`) — strings are `std::string_view` into the input buffer
- **Low allocation** parsing with an arena (`chjson::pmr::arena_resource`) for the in-situ DOM

---

## Table of Contents

- [Quick Start](#quick-start)
- [Build](#build)
- [API Overview](#api-overview)
- [Zero-copy In-situ DOM](#zero-copy-in-situ-dom)
- [Arena / PMR](#arena--pmr)
- [Benchmarks](#benchmarks)
- [Contract (Compatibility & Guarantees)](#contract-compatibility--guarantees)
- [Notes / Limitations](#notes--limitations)

## Quick Start

### Parse JSON (owning DOM)

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  auto r = chjson::parse(R"({"a": [1,2,3], "ok": true})");
  assert(!r.err);

  const chjson::value* a = r.val.find("a");
  assert(a && a->is_array());
  assert(a->as_array()[0].as_int() == 1);
}
```

### Serialize JSON

```cpp
#include <chjson/chjson.hpp>
#include <iostream>

int main() {
  auto r = chjson::parse(R"({"pi": 3.141592653589793, "s":"hi"})");
  if (r.err) return 1;

  std::string compact = chjson::dump(r.val);
  std::string pretty  = chjson::dump(r.val, /*pretty=*/true);

  std::cout << compact << "\n";
  std::cout << pretty  << "\n";
}
```

---

## Build

chjson is header-only; you can just add the include path:

- Header: `include/chjson/chjson.hpp`

This repo also ships a small CMake project for tests & benchmarks.

### Build tests

```powershell
cmake -S chjson -B chjson/build-ninja -G Ninja
cmake --build chjson/build-ninja
ctest --test-dir chjson/build-ninja --output-on-failure
```

### Build & run benchmarks

```powershell
cmake -S chjson -B chjson/build-ninja -G Ninja
cmake --build chjson/build-ninja
& "chjson/build-ninja/chjson_bench.exe" 2000 200
```

If you want to build the comparison benchmark (vs `json_cpp` + `jsoncpp` in this workspace):

```powershell
cmake -S chjson -B chjson/build-ninja -G Ninja -DCHJSON_BUILD_COMPARE_BENCH=ON
cmake --build chjson/build-ninja
& "chjson/build-ninja/chjson_compare_bench.exe" 2000 200
```

---

## API Overview

### Strict parsing + error location

`chjson::parse()` returns `chjson::parse_result`:

- `val`: parsed value (may be null if failed)
- `err`: `chjson::error` (code/offset/line/column)

```cpp
#include <chjson/chjson.hpp>
#include <iostream>

int main() {
  auto r = chjson::parse("{\"a\":1,}"); // trailing comma is invalid
  if (r.err) {
    std::cout << "error code=" << static_cast<int>(r.err.code)
              << " offset=" << r.err.offset
              << " line="   << r.err.line
              << " col="    << r.err.column
              << "\n";
  }
}
```

### Parse options (depth limit, require EOF)

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  chjson::parse_options opt;
  opt.max_depth = 32;
  opt.require_eof = true;

  auto r = chjson::parse("[1]   ", opt);
  assert(!r.err);
}
```

### DOM types (`chjson::value`)

Supported JSON types:

- null
- boolean
- number (stored as int64 when possible, otherwise double)
- string
- array
- object (in insertion order)

Example: inspect types and access values:

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  auto r = chjson::parse(R"({"n":42,"x":0.1,"s":"hi"})");
  assert(!r.err);

  const chjson::value* n = r.val.find("n");
  assert(n && n->is_number() && n->is_int());
  assert(n->as_int() == 42);

  const chjson::value* x = r.val.find("x");
  assert(x && x->is_number());
  (void)x->as_double();

  const chjson::value* s = r.val.find("s");
  assert(s && s->is_string());
  assert(s->as_string() == "hi");
}
```

### Object lookup (`find`)

Objects preserve insertion order and support linear lookup via `find()`.

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  auto r = chjson::parse(R"({"a":1,"b":2})");
  assert(!r.err);

  const chjson::value* b = r.val.find("b");
  assert(b && b->as_int() == 2);
}
```

---

## Zero-copy In-situ DOM

When you need speed and fewer allocations, use `parse_in_situ()`.

It takes ownership of a mutable JSON buffer (`std::string`), **decodes escapes in-place**, and stores strings as `std::string_view` into that buffer.

### In-situ parse (`chjson::document` + `chjson::sv_value`)

```cpp
#include <chjson/chjson.hpp>
#include <cassert>
#include <string>

int main() {
  std::string json = R"({"s":"a\nB","arr":["x","y"],"obj":{"k":"v"}})";

  auto r = chjson::parse_in_situ(std::move(json));
  assert(!r.err);

  const chjson::sv_value& root = r.doc.root();
  const chjson::sv_value* s = root.find("s");
  assert(s && s->is_string());
  assert(s->as_string_view() == std::string_view("a\nB", 3));
}
```

Reuse a `document` in a loop (keeps arena blocks and string capacity):

```cpp
#include <chjson/chjson.hpp>
#include <string_view>

int main() {
  chjson::document doc;
  std::string_view input = R"({"a":1,"b":[true,false,null]})";

  for (int i = 0; i < 1000; ++i) {
    chjson::error err = chjson::parse_in_situ_into(doc, input);
    if (err) return 1;
    (void)doc.root();
  }
}
```

### Zero-copy guarantee (`string_view` points into the document buffer)

```cpp
#include <chjson/chjson.hpp>
#include <cassert>
#include <string>

int main() {
  std::string json = R"({"s":"hello"})";
  auto r = chjson::parse_in_situ(std::move(json));
  assert(!r.err);

  auto buf = r.doc.buffer();
  const char* begin = buf.data();
  const char* end = buf.data() + buf.size();

  const auto* s = r.doc.root().find("s");
  assert(s && s->is_string());

  std::string_view sv = s->as_string_view();
  assert(sv.data() >= begin && sv.data() + sv.size() <= end);
}
```

### Serialize `sv_value`

```cpp
#include <chjson/chjson.hpp>
#include <cassert>
#include <string>

int main() {
  auto r = chjson::parse_in_situ(std::string(R"({"a":1,"s":"x"})"));
  assert(!r.err);

  std::string out = chjson::dump(r.doc.root(), /*pretty=*/true);
  auto r2 = chjson::parse(out);
  assert(!r2.err);
}
```

---

## Arena / PMR

The in-situ DOM uses `std::pmr::vector` internally, backed by a monotonic arena:

- `chjson::pmr::arena_resource`

Characteristics:

- Very fast allocations (bump allocator)
- `deallocate()` is a no-op (memory is freed all-at-once)
- Use `reset()` / destruction to release memory

### Read arena stats

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  auto r = chjson::parse_in_situ(std::string(R"([{"k":"v"},{"k":"v"}])"));
  assert(!r.err);

  std::size_t used = r.doc.arena().bytes_used();
  std::size_t committed = r.doc.arena().bytes_committed();
  (void)used;
  (void)committed;
}
```

### Reset arena (batch-free)

```cpp
#include <chjson/chjson.hpp>

int main() {
  chjson::pmr::arena_resource arena;
  arena.reset(); // frees all blocks
}
```

---

## Benchmarks

Benchmarks are small and intentionally dependency-free.

### chjson micro-benchmark

Executable: `chjson_bench`

- Args: `chjson_bench <n_objects> <iters>`
- Prints MiB/s for:
  - parse(dom)
  - parse(in_situ)
  - dump(dom)
  - plus arena average used/committed bytes

Run it:

```powershell
& "chjson/build-ninja/chjson_bench.exe" 2000 200
```

### Comparison benchmark (chjson vs nlohmann/json vs jsoncpp)

Executable: `chjson_compare_bench`

- Args: `chjson_compare_bench <n_objects> <iters>`
- Parses and dumps the same generated payload across:
  - `chjson` (owning DOM + in-situ parse)
  - `json_cpp` (nlohmann/json)
  - `jsoncpp`

Notes on “fair” dump settings:

- All writers are configured for **compact output** (no indentation).
- Unicode is written as **UTF-8** when supported (`jsoncpp` sets `emitUTF8=true`).
- Real numbers use **17 significant digits** where configurable (`jsoncpp` precision=17, significant).
- Non-finite floats are not emitted as JSON numbers (chjson throws; jsoncpp uses `useSpecialFloats=false`).

Run it:

```powershell
& "chjson/build-ninja/chjson_compare_bench.exe" 2000 200
```

Sample results (Windows, clang++ 17, `Release`, args `2000 200`):

Note: `chjson parse(in_situ)` in this benchmark reuses a single `document` via `parse_in_situ_into()`.

```text
payload bytes: 145645

== Parse ==
chjson parse(dom): 57.7575 MiB/s
chjson parse(in_situ): 201.961 MiB/s
nlohmann parse: 46.8392 MiB/s
jsoncpp parse: 42.5692 MiB/s
rapidjson parse: 504.344 MiB/s

== Dump ==
chjson dump(dom): 523.727 MiB/s
nlohmann dump: 400.439 MiB/s
jsoncpp dump: 72.1507 MiB/s
rapidjson dump: 680.212 MiB/s
```

These numbers are hardware- and toolchain-dependent; treat them as a sanity check and a quick regression signal, not an absolute ranking.

Sample platform

```
- os: Windows 10.0.26100
- cpu: 12th Gen Intel(R) Core(TM) i9-12900K
- logical_cores: 24
- ram_total_gib: 31.7474
- compiler: clang 17.0.6
- builder: cmake 3.28.0 && ninja 1.11.1
```

### Performance notes (why dump got faster)

Recent versions of chjson’s serializer speed up `dump()` primarily by:

- Reserving output capacity based on a cheap structural size estimate (fewer reallocations)
- Escaping strings using chunked `append()` instead of per-character concatenation
- Formatting integers via `std::to_chars` (no temporary `std::string`)
- Formatting doubles via `std::to_chars` in general format with `max_digits10` (fast + round-trip-safe)

---

## Contract (Compatibility & Guarantees)

This section documents behavioral guarantees and engineering constraints.

### C++ standard & portability

chjson targets **C++17** and uses only the standard library.

```cpp
#include <chjson/chjson.hpp>

static_assert(__cplusplus >= 201703L, "chjson requires C++17");

int main() {}
```

### Versioning policy (within this workspace)

This project currently does not publish semantic version tags. Treat public APIs as **experimental** unless you pin a commit.

Example: pin by Git commit (PowerShell):

```powershell
git rev-parse HEAD
```

### ABI / binary compatibility

Because this is **header-only** (templates/inline code, `std::variant` layout, etc.), there is **no stable ABI guarantee** across compilers/flags/standard library versions.

Example: prefer rebuilding all targets together:

```cmake
add_subdirectory(chjson)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE chjson)
```

### Error model (no-throw parsing by default)

- `parse()` / `parse_in_situ()` return `{..., err}`; parse failures do not throw.
- Allocation failures can still throw (`std::bad_alloc`).

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  auto r = chjson::parse("{\"a\":1,}");
  assert(r.err); // parse error captured here
}
```

### Throwing helpers for convenience

- `parse_or_throw()` / `parse_in_situ_or_throw()` throw `std::runtime_error` on parse failure.

```cpp
#include <chjson/chjson.hpp>
#include <string>

int main() {
  try {
    auto v = chjson::parse_or_throw("not-json");
    (void)v;
  } catch (const std::exception&) {
    // handle error
  }
}
```

### Exception safety expectations

- `dump()` throws if asked to serialize non-finite doubles (NaN/Inf), because they are not valid JSON numbers.
- `value::as_int()` throws if the stored number is not an integer.

```cpp
#include <chjson/chjson.hpp>
#include <cmath>

int main() {
  try {
    auto v = chjson::value::number(std::nan(""));
    auto s = chjson::dump(v); // throws
    (void)s;
  } catch (const std::exception&) {
  }
}
```

### Thread-safety

- Parsing functions are **re-entrant**: calling `parse()` from multiple threads is safe as long as each call uses its own inputs/results.
- `document` / `sv_value` are not designed for concurrent mutation; treat a `document` as thread-confined unless you implement your own synchronization.

```cpp
#include <chjson/chjson.hpp>
#include <thread>

static void worker() {
  auto r = chjson::parse(R"({"x":1})");
  (void)r;
}

int main() {
  std::thread t1(worker);
  std::thread t2(worker);
  t1.join();
  t2.join();
}
```

### Time complexity

- Parse: $O(n)$ over input bytes (single pass with local backtracking for numbers/escapes)
- Dump: $O(n)$ over produced output

Example: the benchmark processes input bytes linearly:

```powershell
& "chjson/build-ninja/chjson_bench.exe" 20000 50
```

### Memory model (owning vs in-situ)

- `value` DOM allocates/copies strings; memory is owned by the DOM.
- `document` in-situ DOM stores `string_view` into `document::buffer()` and uses an arena for container allocations.

Example: in-situ strings depend on document lifetime:

```cpp
#include <chjson/chjson.hpp>
#include <string>

int main() {
  auto r = chjson::parse_in_situ(std::string(R"({"s":"x"})"));
  std::string_view sv = r.doc.root().find("s")->as_string_view();
  (void)sv;
}
```

## Notes / Limitations

### JSON-only (no extensions)

This parser is strict JSON:

- No comments (`//` or `/* */`)
- No trailing commas
- No NaN/Inf output in JSON numbers (dump throws for NaN/Inf)

Example: trailing comma fails:

```cpp
#include <chjson/chjson.hpp>
#include <cassert>

int main() {
  auto r = chjson::parse("{\"a\":1,}");
  assert(r.err);
}
```

### In-situ lifetime rule

All `std::string_view` inside `chjson::sv_value` refer to `chjson::document`'s internal buffer.

Example: keep the document alive:

```cpp
#include <chjson/chjson.hpp>
#include <string>

int main() {
  auto r = chjson::parse_in_situ(std::string(R"({"s":"x"})"));
  auto sv = r.doc.root().find("s")->as_string_view();
  // sv is valid only while r.doc is alive.
  (void)sv;
}
```
