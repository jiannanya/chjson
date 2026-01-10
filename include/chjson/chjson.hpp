#pragma once

// chjson: a small, header-only C++17 JSON library.
// Goals: fast enough in practice, strict JSON by default, high-quality errors.

#include <cassert>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#if defined(_M_X64) || defined(__SSE2__)
  #if defined(_MSC_VER)
    #include <intrin.h>
  #endif 
  #include <immintrin.h>
#endif
#include <limits>
#include <map>
#include <memory_resource>
#include <new>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <thread>
#include <future>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

// Config: floating-point parsing backend.
// Override by defining CHJSON_USE_FROM_CHARS_DOUBLE to 0/1 before including this header.
#ifndef CHJSON_USE_FROM_CHARS_DOUBLE
  #define CHJSON_USE_FROM_CHARS_DOUBLE 0
#endif

// Optional: use chfloat (vendored under chjson/another/chfloat) when available.
// This is typically much faster than strtod/std::from_chars on MSVC for JSON numbers.
//
// Back-compat: if you previously defined CHJSON_USE_FAST_FLOAT, it is treated as
// an alias for CHJSON_USE_CHFLOAT.
#ifndef CHJSON_USE_CHFLOAT
  #ifdef CHJSON_USE_FAST_FLOAT
    #define CHJSON_USE_CHFLOAT CHJSON_USE_FAST_FLOAT
  #else
    #define CHJSON_USE_CHFLOAT 1
  #endif
#endif

// Config: thread-local caches for single-shot parse(dom).
// These caches reduce heap churn for workloads that repeatedly call chjson::parse()
// (which constructs/destroys a document each time).
//
// Override by defining these macros before including this header.
#ifndef CHJSON_USE_TLS_PARSE_CACHE
  #define CHJSON_USE_TLS_PARSE_CACHE 1
#endif

// Max std::string capacity to cache per thread (bytes).
#ifndef CHJSON_TLS_PARSE_BUFFER_MAX
  #define CHJSON_TLS_PARSE_BUFFER_MAX (4u * 1024u * 1024u)
#endif

// Max arena committed bytes to cache per thread.
#ifndef CHJSON_TLS_PARSE_ARENA_MAX
  // Default raised to keep parse(dom) competitive for medium/large documents when
  // users call chjson::parse() in a tight loop (benchmarks, batch parsing, etc.).
  // Override this macro before including chjson.hpp to cap per-thread cached memory.
  #define CHJSON_TLS_PARSE_ARENA_MAX (16u * 1024u * 1024u)
#endif

// Max arena bytes to reserve up front for parse(dom).
// This is independent from CHJSON_TLS_PARSE_ARENA_MAX (which only controls the
// per-thread cache cap). Reserving too little for large number-heavy inputs can
// cause excessive monotonic growth and copying.
#ifndef CHJSON_PARSE_DOM_ARENA_RESERVE_MAX
  #define CHJSON_PARSE_DOM_ARENA_RESERVE_MAX (16u * 1024u * 1024u)
#endif

// Max MT-parse backing bytes to cache per thread.
// This backing stores per-thread monotonic allocations for the MT parse path.
#ifndef CHJSON_TLS_PARSE_MT_BACKING_MAX
  #define CHJSON_TLS_PARSE_MT_BACKING_MAX (16u * 1024u * 1024u)
#endif

// Max number of per-thread monotonic resources to cache per thread.
#ifndef CHJSON_TLS_PARSE_MT_RESOURCES_MAX
  #define CHJSON_TLS_PARSE_MT_RESOURCES_MAX 64u
#endif

// Config: parse(dom) fast path for inputs with no strings.
// When enabled, chjson::parse() may skip copying the full input buffer for JSON
// texts that contain no '"' characters, and will eagerly parse numbers.
// This primarily improves number-heavy workloads (e.g. Parse+sum(numbers)).
#ifndef CHJSON_PARSE_DOM_NO_STRING_FASTPATH
  #define CHJSON_PARSE_DOM_NO_STRING_FASTPATH 1
#endif

// Config: conservative thresholds for MT parse(dom).
// The MT parse path has overhead (span scan, task launch, extra alloc/init), so
// it should be reserved for large top-level containers.
//
// Override these macros before including this header to tune for your workload.
#ifndef CHJSON_PARSE_MT_MIN_BYTES
  #define CHJSON_PARSE_MT_MIN_BYTES (256u * 1024u)
#endif

#ifndef CHJSON_PARSE_MT_MIN_SPANS
  #define CHJSON_PARSE_MT_MIN_SPANS 64u
#endif

// Minimum average bytes per top-level span. Helps avoid MT on many tiny elements.
#ifndef CHJSON_PARSE_MT_MIN_AVG_SPAN_BYTES
  #define CHJSON_PARSE_MT_MIN_AVG_SPAN_BYTES (4u * 1024u)
#endif

// Enable SSE2-accelerated scanning in the MT span splitter.
// This only affects the initial top-level span scan (used to decide MT splitting).
#ifndef CHJSON_PARSE_MT_SCAN_SIMD
  #define CHJSON_PARSE_MT_SCAN_SIMD 0
#endif

// Config: use chjson internal allocator for chjson-owned heap allocations.
// This does NOT replace user allocations (e.g. std::string/std::vector in the legacy DOM).
// It targets hot internal allocations: arena blocks, MT-parse backing/resources, and
// owned number token storage.
#ifndef CHJSON_USE_INTERNAL_ALLOCATOR
  #define CHJSON_USE_INTERNAL_ALLOCATOR 1
#endif

#if CHJSON_USE_CHFLOAT
  #if defined(__has_include)
    #if __has_include(<chfloat/chfloat.h>)
      #include <chfloat/chfloat.h>
      #define CHJSON_HAS_CHFLOAT 1
    #else
      #define CHJSON_HAS_CHFLOAT 0
    #endif
  #else
    #define CHJSON_HAS_CHFLOAT 0
  #endif
#else
  #define CHJSON_HAS_CHFLOAT 0
#endif

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

  constexpr explicit operator bool() const noexcept { return code != error_code::ok; }
};

namespace detail {

inline void update_line_col(std::string_view s, std::size_t pos, std::size_t& line, std::size_t& col) {
  line = 1;
  col = 1;
  for (std::size_t i = 0; i < pos && i < s.size(); ++i) {
    if (s[i] == '\n') {
      ++line;
      col = 1;
    } else {
      ++col;
    }
  }
}

inline bool is_ws(char c) noexcept {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

inline void skip_ws(const char* buf, std::size_t size, std::size_t& i) noexcept {
  // Extremely common case for compact JSON: no whitespace at the current position.
  // Avoid the heavier SIMD path (which does a 16-byte load and mask setup) unless
  // we actually see whitespace.
  if (i >= size) return;
  if (!is_ws(buf[i])) return;

  // Fast path: SSE2 scan 16 bytes at a time (available on MSVC x64 and most x86).
#if defined(_M_X64) || defined(__SSE2__)
  while (i + 16 <= size) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + i));
    const __m128i is_space = _mm_cmpeq_epi8(v, _mm_set1_epi8(' '));
    const __m128i is_nl = _mm_cmpeq_epi8(v, _mm_set1_epi8('\n'));
    const __m128i is_cr = _mm_cmpeq_epi8(v, _mm_set1_epi8('\r'));
    const __m128i is_tab = _mm_cmpeq_epi8(v, _mm_set1_epi8('\t'));
    const __m128i is_ws_v = _mm_or_si128(_mm_or_si128(is_space, is_nl), _mm_or_si128(is_cr, is_tab));
    const unsigned ws_mask = static_cast<unsigned>(_mm_movemask_epi8(is_ws_v));
    if (ws_mask == 0xFFFFu) {
      i += 16;
      continue;
    }

    const unsigned non = (~ws_mask) & 0xFFFFu;
    if (non != 0u) {
#if defined(_MSC_VER)
      unsigned long idx = 0;
      _BitScanForward(&idx, non);
      i += static_cast<std::size_t>(idx);
#else
      i += static_cast<std::size_t>(__builtin_ctz(non));
#endif
      return;
    }
    i += 16;
  }
#endif

  while (i < size && is_ws(buf[i])) ++i;
}

inline void skip_ws(std::string_view s, std::size_t& i) noexcept {
  skip_ws(s.data(), s.size(), i);
}

inline int hex_val(char c) noexcept {
  const unsigned char uc = static_cast<unsigned char>(c);
  if (uc >= static_cast<unsigned char>('0') && uc <= static_cast<unsigned char>('9')) {
    return static_cast<int>(uc - static_cast<unsigned char>('0'));
  }
  const unsigned char lc = static_cast<unsigned char>(uc | 0x20u); // ASCII to-lower
  if (lc >= static_cast<unsigned char>('a') && lc <= static_cast<unsigned char>('f')) {
    return 10 + static_cast<int>(lc - static_cast<unsigned char>('a'));
  }
  return -1;
}

inline void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

inline bool parse_u4_unsafe(const char* p, std::uint32_t& out_cp) noexcept {
  const int h0 = hex_val(p[0]);
  const int h1 = hex_val(p[1]);
  const int h2 = hex_val(p[2]);
  const int h3 = hex_val(p[3]);
  if ((h0 | h1 | h2 | h3) < 0) return false;
  out_cp = (static_cast<std::uint32_t>(h0) << 12) |
           (static_cast<std::uint32_t>(h1) << 8) |
           (static_cast<std::uint32_t>(h2) << 4) |
           static_cast<std::uint32_t>(h3);
  return true;
}

inline bool parse_u4(std::string_view s, std::size_t& i, std::uint32_t& out_cp) {
  if (i + 4 > s.size()) return false;
  std::uint32_t v = 0;
  if (!parse_u4_unsafe(s.data() + i, v)) return false;
  i += 4;
  out_cp = v;
  return true;
}

inline bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

// -----------------------------
// Internal allocator (mimalloc-inspired)
// -----------------------------
// Design goals (for chjson internal usage):
// - Very fast for small, frequent allocations.
// - Thread-local fast path (no atomics) with occasional global refill/drain.
// - No per-allocation headers; deallocate requires the size+alignment (known at call sites).
// - Large allocations fall back to aligned ::operator new/delete.
//
// This allocator is intentionally minimal and only used where chjson knows the size.
// It is NOT a general-purpose replacement for global new/delete.

inline constexpr bool chjson_is_pow2(std::size_t x) noexcept {
  return x != 0 && (x & (x - 1)) == 0;
}

inline std::size_t chjson_round_up_pow2(std::size_t x) noexcept {
  // Round up to next power-of-two, with a minimum of 8.
  if (x <= 8) return 8;
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
#if SIZE_MAX > 0xFFFFFFFFu
  x |= x >> 32;
#endif
  return x + 1;
}

struct chjson_internal_allocator {
  static constexpr std::size_t k_small_min = 8;
  static constexpr std::size_t k_small_max = 4096;
  static constexpr std::size_t k_page_size = 64u * 1024u;
  static constexpr std::size_t k_page_align = 64u; // used only for alignment math; page allocation itself is unaligned
  static constexpr unsigned k_num_classes = 10; // 8..4096 (powers of two)
  static constexpr unsigned k_local_flush_threshold = 256;
  static constexpr unsigned k_local_flush_batch = 128;

  // One global per size class.
  inline static std::atomic<void*> g_free[k_num_classes];
  inline static std::atomic<void*> g_pages{nullptr};

  // One TLS per size class.
  inline static thread_local void* tls_free[k_num_classes];
  inline static thread_local unsigned tls_count[k_num_classes];

  static unsigned size_to_class(std::size_t size_pow2) noexcept {
    // size_pow2 in {8,16,...,4096}
    unsigned idx = 0;
    std::size_t v = size_pow2;
    while (v > k_small_min) {
      v >>= 1;
      ++idx;
    }
    return idx;
  }

  static void push_global(unsigned idx, void* p) noexcept {
    void* head = g_free[idx].load(std::memory_order_relaxed);
    do {
      *reinterpret_cast<void**>(p) = head;
    } while (!g_free[idx].compare_exchange_weak(head, p, std::memory_order_release, std::memory_order_relaxed));
  }

  static void push_global_list(unsigned idx, void* list_head) noexcept {
    if (!list_head) return;
    // Find tail.
    void* tail = list_head;
    while (void* next = *reinterpret_cast<void**>(tail)) tail = next;

    void* head = g_free[idx].load(std::memory_order_relaxed);
    do {
      *reinterpret_cast<void**>(tail) = head;
    } while (!g_free[idx].compare_exchange_weak(head, list_head, std::memory_order_release, std::memory_order_relaxed));
  }

  static void* pop_local(unsigned idx) noexcept {
    void* p = tls_free[idx];
    if (!p) return nullptr;
    tls_free[idx] = *reinterpret_cast<void**>(p);
    --tls_count[idx];
    return p;
  }

  static void push_local(unsigned idx, void* p) noexcept {
    *reinterpret_cast<void**>(p) = tls_free[idx];
    tls_free[idx] = p;
    ++tls_count[idx];

    if (tls_count[idx] > k_local_flush_threshold) {
      // Detach a small batch and publish to global.
      void* batch = tls_free[idx];
      void* cur = batch;
      unsigned n = 1;
      while (cur && n < k_local_flush_batch) {
        cur = *reinterpret_cast<void**>(cur);
        ++n;
      }
      if (cur) {
        void* rest = *reinterpret_cast<void**>(cur);
        *reinterpret_cast<void**>(cur) = nullptr;
        tls_free[idx] = rest;
        tls_count[idx] -= n;
        push_global_list(idx, batch);
      }
    }
  }

  static void push_local_noflush(unsigned idx, void* p) noexcept {
    *reinterpret_cast<void**>(p) = tls_free[idx];
    tls_free[idx] = p;
    ++tls_count[idx];
  }

  static void refill_from_global(unsigned idx) noexcept {
    void* list = g_free[idx].exchange(nullptr, std::memory_order_acquire);
    if (!list) return;
    // Count and prepend.
    unsigned n = 0;
    void* tail = list;
    while (void* next = *reinterpret_cast<void**>(tail)) {
      tail = next;
      ++n;
    }
    ++n;
    *reinterpret_cast<void**>(tail) = tls_free[idx];
    tls_free[idx] = list;
    tls_count[idx] += n;
  }

  static void alloc_new_page_for_class(unsigned idx, std::size_t block_size) {
    // Allocate a fresh page and carve it into blocks for this size class.
    void* page = ::operator new(k_page_size);

    // Track pages for optional cleanup at exit.
    void* pages_head = g_pages.load(std::memory_order_relaxed);
    do {
      *reinterpret_cast<void**>(page) = pages_head;
    } while (!g_pages.compare_exchange_weak(pages_head, page, std::memory_order_release, std::memory_order_relaxed));

    std::byte* p = reinterpret_cast<std::byte*>(page);
    std::byte* start = p + sizeof(void*);
    // Align start to block_size.
    const std::uintptr_t s = reinterpret_cast<std::uintptr_t>(start);
    const std::uintptr_t aligned = (s + (block_size - 1)) & ~(static_cast<std::uintptr_t>(block_size) - 1u);
    start = reinterpret_cast<std::byte*>(aligned);
    std::size_t avail = k_page_size - static_cast<std::size_t>(start - p);
    const std::size_t count = avail / block_size;
    if (count == 0) {
      throw std::bad_alloc();
    }

    // Push all blocks onto TLS free list.
    for (std::size_t i = 0; i < count; ++i) {
      void* b = start + i * block_size;
      push_local_noflush(idx, b);
    }
  }

  static void* allocate(std::size_t bytes, std::size_t alignment) {
    if (bytes == 0) bytes = 1;
    if (alignment == 0) alignment = alignof(std::max_align_t);
    if (!chjson_is_pow2(alignment)) throw std::bad_alloc();

#if CHJSON_USE_INTERNAL_ALLOCATOR
    std::size_t need = bytes < alignment ? alignment : bytes;
    if (need <= k_small_max) {
      const std::size_t block = chjson_round_up_pow2(need);
      const unsigned idx = size_to_class(block);
      if (void* p = pop_local(idx)) return p;
      refill_from_global(idx);
      if (void* p = pop_local(idx)) return p;
      alloc_new_page_for_class(idx, block);
      if (void* p = pop_local(idx)) return p;
      throw std::bad_alloc();
    }
#endif

    // Large fallback.
    if (alignment <= alignof(std::max_align_t)) {
      return ::operator new(bytes);
    }

    // Portable aligned allocation fallback (stores original pointer just before aligned block).
    const std::size_t over = bytes + alignment + sizeof(void*);
    void* raw = ::operator new(over);
    std::uintptr_t p = reinterpret_cast<std::uintptr_t>(raw) + sizeof(void*);
    std::uintptr_t aligned = (p + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1u);
    void* out = reinterpret_cast<void*>(aligned);
    *(reinterpret_cast<void**>(out) - 1) = raw;
    return out;
  }

  static void deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept {
    if (!p) return;
    if (bytes == 0) bytes = 1;
    if (alignment == 0) alignment = alignof(std::max_align_t);
    if (!chjson_is_pow2(alignment)) {
      // Should never happen in chjson; avoid UB.
      ::operator delete(p);
      return;
    }

#if CHJSON_USE_INTERNAL_ALLOCATOR
    std::size_t need = bytes < alignment ? alignment : bytes;
    if (need <= k_small_max) {
      const std::size_t block = chjson_round_up_pow2(need);
      const unsigned idx = size_to_class(block);
      push_local(idx, p);
      return;
    }
#endif

    if (alignment <= alignof(std::max_align_t)) {
      ::operator delete(p);
      return;
    }

    void* raw = *(reinterpret_cast<void**>(p) - 1);
    ::operator delete(raw);
  }

  ~chjson_internal_allocator() noexcept {
    // Best-effort cleanup of pages allocated by the small-object pools.
    // This runs at program exit; it does not attempt to synchronize with
    // concurrent allocations.
    void* page = g_pages.exchange(nullptr, std::memory_order_acquire);
    while (page) {
      void* next = *reinterpret_cast<void**>(page);
      ::operator delete(page);
      page = next;
    }
  }
};

// One program-wide allocator state (single instance across TUs).
inline chjson_internal_allocator g_chjson_internal_allocator_state;

inline void* chjson_allocate(std::size_t bytes, std::size_t alignment) {
  (void)g_chjson_internal_allocator_state;
  return chjson_internal_allocator::allocate(bytes, alignment);
}

inline void chjson_deallocate(void* p, std::size_t bytes, std::size_t alignment) noexcept {
  (void)g_chjson_internal_allocator_state;
  chjson_internal_allocator::deallocate(p, bytes, alignment);
}

template <class T>
inline T* chjson_allocate_n(std::size_t n) {
  if (n == 0) return nullptr;
  const std::size_t bytes = sizeof(T) * n;
  return static_cast<T*>(chjson_allocate(bytes, alignof(T)));
}

template <class T>
inline void chjson_deallocate_n(T* p, std::size_t n) noexcept {
  if (!p) return;
  const std::size_t bytes = sizeof(T) * n;
  chjson_deallocate(static_cast<void*>(p), bytes, alignof(T));
}

struct chjson_sized_byte_deleter {
  std::size_t size{0};
  void operator()(std::byte* p) const noexcept {
    if (!p) return;
    chjson_deallocate(p, size, alignof(std::max_align_t));
  }
};

using chjson_byte_ptr = std::unique_ptr<std::byte, chjson_sized_byte_deleter>;

struct number_value {
  // Keep both: integers preserve round-trip and allow fast integer APIs.
  // If `is_int == false`, use `d`.
  bool is_int{false};
  std::int64_t i{0};
  double d{0.0};
};

struct owned_number_value {
  // For non-integers, keep the original token for fast dump and lazy double parse.
  // If raw token is empty, use `d`.
  bool is_int{false};
  std::int64_t i{0};
  mutable bool has_double{false};
  mutable double d{0.0};

  // Compact small-string storage for raw number tokens.
  // Avoids heap allocations for common tokens without bloating value size.
  static constexpr std::size_t kInlineRawCap = 32;

  std::uint32_t raw_len{0};
  bool raw_is_heap{false};
  union {
    char raw_inline[kInlineRawCap];
    char* raw_heap;
  };

  owned_number_value() noexcept : raw_inline{} {}

  owned_number_value(const owned_number_value& other)
      : is_int(other.is_int), i(other.i), has_double(other.has_double), d(other.d), raw_len(other.raw_len), raw_is_heap(false),
        raw_inline{} {
    if (other.raw_len == 0) return;
    if (other.raw_is_heap) {
      raw_is_heap = true;
      raw_heap = static_cast<char*>(chjson_allocate(static_cast<std::size_t>(raw_len), alignof(char)));
      std::memcpy(raw_heap, other.raw_heap, raw_len);
      return;
    }
    std::memcpy(raw_inline, other.raw_inline, raw_len);
  }

  owned_number_value& operator=(const owned_number_value& other) {
    if (this == &other) return *this;
    clear_raw();
    is_int = other.is_int;
    i = other.i;
    has_double = other.has_double;
    d = other.d;
    raw_len = other.raw_len;
    if (other.raw_len == 0) {
      raw_is_heap = false;
      return *this;
    }
    if (other.raw_is_heap) {
      raw_is_heap = true;
      raw_heap = static_cast<char*>(chjson_allocate(static_cast<std::size_t>(raw_len), alignof(char)));
      std::memcpy(raw_heap, other.raw_heap, raw_len);
      return *this;
    }
    raw_is_heap = false;
    std::memcpy(raw_inline, other.raw_inline, raw_len);
    return *this;
  }

  owned_number_value(owned_number_value&& other) noexcept
      : is_int(other.is_int), i(other.i), has_double(other.has_double), d(other.d), raw_len(other.raw_len), raw_is_heap(other.raw_is_heap),
        raw_inline{} {
    if (raw_len == 0) {
      raw_is_heap = false;
      return;
    }
    if (raw_is_heap) {
      raw_heap = other.raw_heap;
      other.raw_heap = nullptr;
      other.raw_len = 0;
      other.raw_is_heap = false;
      return;
    }
    std::memcpy(raw_inline, other.raw_inline, raw_len);
    other.raw_len = 0;
  }

  owned_number_value& operator=(owned_number_value&& other) noexcept {
    if (this == &other) return *this;
    clear_raw();
    is_int = other.is_int;
    i = other.i;
    has_double = other.has_double;
    d = other.d;
    raw_len = other.raw_len;
    raw_is_heap = other.raw_is_heap;
    if (raw_len == 0) {
      raw_is_heap = false;
      return *this;
    }
    if (raw_is_heap) {
      raw_heap = other.raw_heap;
      other.raw_heap = nullptr;
      other.raw_len = 0;
      other.raw_is_heap = false;
      return *this;
    }
    std::memcpy(raw_inline, other.raw_inline, raw_len);
    other.raw_len = 0;
    other.raw_is_heap = false;
    return *this;
  }

  ~owned_number_value() { clear_raw(); }

  void clear_raw() noexcept {
    if (raw_is_heap) {
      chjson_deallocate(raw_heap, static_cast<std::size_t>(raw_len), alignof(char));
    }
    raw_is_heap = false;
    raw_len = 0;
  }

  [[nodiscard]] std::string_view raw_token() const noexcept {
    if (raw_len == 0) return {};
    if (raw_is_heap) return std::string_view(raw_heap, static_cast<std::size_t>(raw_len));
    return std::string_view(raw_inline, static_cast<std::size_t>(raw_len));
  }

  void set_raw(std::string_view token) {
    clear_raw();
    if (token.empty()) return;

    if (token.size() <= kInlineRawCap) {
      raw_is_heap = false;
      raw_len = static_cast<std::uint32_t>(token.size());
      std::memcpy(raw_inline, token.data(), token.size());
      return;
    }

    raw_is_heap = true;
    raw_len = static_cast<std::uint32_t>(token.size());
    raw_heap = static_cast<char*>(chjson_allocate(static_cast<std::size_t>(raw_len), alignof(char)));
    std::memcpy(raw_heap, token.data(), token.size());
  }
};

inline double parse_double(const char* first, const char* last) {
  const std::size_t len = static_cast<std::size_t>(last - first);
  // Fast path: chfloat.
#if CHJSON_HAS_CHFLOAT
  {
    double v = 0.0;
    auto r = chfloat::from_chars(first, last, v);
    if (r.ec == chfloat::errc::ok && r.ptr == last) return v;
  }
#endif

  // Backend choice:
  // - strtod: often quite fast on MSVC/Windows and very robust.
  // - from_chars: locale-free and allocation-free, but performance varies by STL.
  // Enable from_chars explicitly if it benchmarks better for your toolchain.
#if defined(CHJSON_USE_FROM_CHARS_DOUBLE) && CHJSON_USE_FROM_CHARS_DOUBLE
#if defined(__cpp_lib_to_chars)
  {
    double v = 0.0;
    auto r = std::from_chars(first, last, v, std::chars_format::general);
    if (r.ec == std::errc{} && r.ptr == last) return v;
  }
#endif
#endif

  // Fallback: token is not NUL-terminated; avoid heap alloc for typical short numbers.
  constexpr std::size_t kStackCap = 128;
  if (len < kStackCap) {
    char buf[kStackCap];
    if (len != 0) std::memcpy(buf, first, len);
    buf[len] = '\0';
    return std::strtod(buf, nullptr);
  }

  // Long token fallback: avoid std::string heap allocation.
  // Allocate a temporary NUL-terminated buffer from chjson internal allocator.
  char* tmp = static_cast<char*>(detail::chjson_allocate(len + 1, alignof(char)));
  if (len != 0) std::memcpy(tmp, first, len);
  tmp[len] = '\0';
  const double v = std::strtod(tmp, nullptr);
  detail::chjson_deallocate(tmp, len + 1, alignof(char));
  return v;
}

inline double parse_double(std::string_view token) {
  // Fast path: chfloat.
#if CHJSON_HAS_CHFLOAT
  {
    double v = 0.0;
    const char* first = token.data();
    const char* last = token.data() + token.size();
    auto r = chfloat::from_chars(first, last, v);
    if (r.ec == chfloat::errc::ok && r.ptr == last) return v;
  }
#endif

  // Backend choice:
  // - strtod: often quite fast on MSVC/Windows and very robust.
  // - from_chars: locale-free and allocation-free, but performance varies by STL.
  // Enable from_chars explicitly if it benchmarks better for your toolchain.
#if defined(CHJSON_USE_FROM_CHARS_DOUBLE) && CHJSON_USE_FROM_CHARS_DOUBLE
#if defined(__cpp_lib_to_chars)
  {
    double v = 0.0;
    const char* first = token.data();
    const char* last = token.data() + token.size();
    auto r = std::from_chars(first, last, v, std::chars_format::general);
    if (r.ec == std::errc{} && r.ptr == last) return v;
  }
#endif
#endif

  // Fallback: token is not NUL-terminated; avoid heap alloc for typical short numbers.
  constexpr std::size_t kStackCap = 128;
  if (token.size() < kStackCap) {
    char buf[kStackCap];
    if (!token.empty()) std::memcpy(buf, token.data(), token.size());
    buf[token.size()] = '\0';
    return std::strtod(buf, nullptr);
  }

  // Long token fallback: avoid std::string heap allocation.
  const std::size_t len = token.size();
  char* tmp = static_cast<char*>(detail::chjson_allocate(len + 1, alignof(char)));
  if (len != 0) std::memcpy(tmp, token.data(), len);
  tmp[len] = '\0';
  const double v = std::strtod(tmp, nullptr);
  detail::chjson_deallocate(tmp, len + 1, alignof(char));
  return v;
}

inline bool parse_number(const char* buf, std::size_t size, std::size_t& i, number_value& out, bool parse_fp = true) {
  // JSON number grammar:
  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  //
  // Performance note: float-heavy inputs ('.'/'e') are common in benchmarks.
  // Avoid doing integer accumulation/overflow tracking unless the token is
  // actually an integer.
  const std::size_t start = i;
  if (i >= size) return false;

  std::size_t p = i;
  bool neg = false;
  if (buf[p] == '-') {
    neg = true;
    ++p;
    if (p >= size) return false;
  }

  const std::size_t digits_begin = p;

  // Integer accumulation while scanning digits (one pass for integers).
  // We still need the full token span for float parsing fallback.
  std::uint64_t acc = 0;
  bool overflow = false;

  if (buf[p] == '0') {
    acc = 0;
    ++p;
    if (p < size && is_digit(buf[p])) return false;
  } else {
    const char c0 = buf[p];
    if (c0 < '1' || c0 > '9') return false;
    acc = static_cast<std::uint64_t>(c0 - '0');
    ++p;
    while (p < size && is_digit(buf[p])) {
      const std::uint64_t d = static_cast<std::uint64_t>(buf[p] - '0');
      if (!overflow) {
        if (acc > (std::numeric_limits<std::uint64_t>::max() - d) / 10u) {
          overflow = true;
        } else {
          acc = acc * 10u + d;
        }
      }
      ++p;
    }
  }

  bool is_int = true;
  if (p < size && buf[p] == '.') {
    is_int = false;
    ++p;
    if (p >= size || !is_digit(buf[p])) return false;
    while (p < size && is_digit(buf[p])) ++p;
  }

  if (p < size && (buf[p] == 'e' || buf[p] == 'E')) {
    is_int = false;
    ++p;
    if (p >= size) return false;
    if (buf[p] == '+' || buf[p] == '-') {
      ++p;
      if (p >= size) return false;
    }
    if (!is_digit(buf[p])) return false;
    while (p < size && is_digit(buf[p])) ++p;
  }

  const std::size_t end = p;
  i = end;
  const char* token_first = buf + start;
  const char* token_last = buf + end;

  if (!is_int) {
    out.is_int = false;
    if (parse_fp) out.d = parse_double(token_first, token_last);
    return true;
  }

  if (overflow) {
    out.is_int = false;
    if (parse_fp) out.d = parse_double(token_first, token_last);
    return true;
  }

  if (neg) {
    const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull;
    if (acc > limit) {
      out.is_int = false;
      if (parse_fp) out.d = parse_double(token_first, token_last);
      return true;
    }
    out.is_int = true;
    out.i = (acc == limit) ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(acc);
    return true;
  }

  if (acc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    out.is_int = false;
    if (parse_fp) out.d = parse_double(token_first, token_last);
    return true;
  }
  out.is_int = true;
  out.i = static_cast<std::int64_t>(acc);
  return true;
}



inline bool parse_number(const char* buf, std::size_t size, std::size_t& i, owned_number_value& out, bool parse_fp = true) {
  const std::size_t start = i;
  if (i >= size) return false;

  std::size_t p = i;
  bool neg = false;
  if (buf[p] == '-') {
    neg = true;
    ++p;
    if (p >= size) return false;
  }

  const std::size_t digits_begin = p;

  if (buf[p] == '0') {
    ++p;
    if (p < size && is_digit(buf[p])) return false;
  } else {
    const char c0 = buf[p];
    if (c0 < '1' || c0 > '9') return false;
    ++p;
    while (p < size && is_digit(buf[p])) ++p;
  }

  bool is_int = true;
  if (p < size && buf[p] == '.') {
    is_int = false;
    ++p;
    if (p >= size || !is_digit(buf[p])) return false;
    while (p < size && is_digit(buf[p])) ++p;
  }

  if (p < size && (buf[p] == 'e' || buf[p] == 'E')) {
    is_int = false;
    ++p;
    if (p >= size) return false;
    if (buf[p] == '+' || buf[p] == '-') {
      ++p;
      if (p >= size) return false;
    }
    if (!is_digit(buf[p])) return false;
    while (p < size && is_digit(buf[p])) ++p;
  }

  const std::size_t end = p;
  i = end;
  const std::string_view token(buf + start, end - start);

  auto set_raw = [&]() {
    out.set_raw(token);
  };
  auto set_double = [&]() {
    if (parse_fp) {
      out.d = parse_double(token);
      out.has_double = true;
    } else {
      out.d = 0.0;
      out.has_double = false;
    }
  };

  if (!is_int) {
    out.is_int = false;
    set_raw();
    set_double();
    return true;
  }

  // Integer: accumulation pass.
  std::uint64_t acc = 0;
  bool overflow = false;
  for (std::size_t q = digits_begin; q < end; ++q) {
    const std::uint64_t d = static_cast<std::uint64_t>(buf[q] - '0');
    if (!overflow) {
      if (acc > (std::numeric_limits<std::uint64_t>::max() - d) / 10u) {
        overflow = true;
      } else {
        acc = acc * 10u + d;
      }
    }
  }

  if (overflow) {
    out.is_int = false;
    set_raw();
    set_double();
    return true;
  }

  if (neg) {
    const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull;
    if (acc > limit) {
      out.is_int = false;
      set_raw();
      set_double();
      return true;
    }
    out.is_int = true;
    out.i = (acc == limit) ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(acc);
    out.clear_raw();
    out.has_double = false;
    return true;
  }

  if (acc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    out.is_int = false;
    set_raw();
    set_double();
    return true;
  }
  out.is_int = true;
  out.i = static_cast<std::int64_t>(acc);
  out.clear_raw();
  out.has_double = false;
  return true;
}

inline bool parse_number(std::string_view s, std::size_t& i, number_value& out, bool parse_fp = true) {
  return parse_number(s.data(), s.size(), i, out, parse_fp);
}

inline bool parse_number(std::string_view s, std::size_t& i, owned_number_value& out, bool parse_fp = true) {
  return parse_number(s.data(), s.size(), i, out, parse_fp);
}

} // namespace detail

class value {
public:
  using array = std::vector<value>;
  using object = std::vector<std::pair<std::string, value>>;

  enum class kind { null, boolean, number, string, array, object };

  value() noexcept : data_(std::monostate{}) {}
  value(std::nullptr_t) noexcept : data_(std::monostate{}) {}
  value(bool b) : data_(b) {}

  static value integer(std::int64_t i) {
    detail::owned_number_value n;
    n.is_int = true;
    n.i = i;
    n.has_double = false;
    n.d = 0.0;
    n.clear_raw();
    value v;
    v.data_ = n;
    return v;
  }

  static value number(double d) {
    detail::owned_number_value n;
    n.is_int = false;
    n.has_double = true;
    n.d = d;
    n.clear_raw();
    value v;
    v.data_ = n;
    return v;
  }

  value(std::string s) : data_(std::move(s)) {}
  value(const char* s) : data_(std::string(s)) {}
  value(array a) : data_(std::move(a)) {}
  value(object o) : data_(std::move(o)) {}

  kind type() const noexcept {
    switch (data_.index()) {
      case 0: return kind::null;
      case 1: return kind::boolean;
      case 2: return kind::number;
      case 3: return kind::string;
      case 4: return kind::array;
      case 5: return kind::object;
      default: return kind::null;
    }
  }

  bool is_null() const noexcept { return std::holds_alternative<std::monostate>(data_); }
  bool is_bool() const noexcept { return std::holds_alternative<bool>(data_); }
  bool is_number() const noexcept { return std::holds_alternative<detail::owned_number_value>(data_); }
  bool is_string() const noexcept { return std::holds_alternative<std::string>(data_); }
  bool is_array() const noexcept { return std::holds_alternative<array>(data_); }
  bool is_object() const noexcept { return std::holds_alternative<object>(data_); }

  bool as_bool() const { return std::get<bool>(data_); }

  bool is_int() const noexcept {
    if (!is_number()) return false;
    return std::get<detail::owned_number_value>(data_).is_int;
  }

  std::int64_t as_int() const {
    const auto& n = std::get<detail::owned_number_value>(data_);
    if (!n.is_int) throw std::runtime_error("chjson: number is not int");
    return n.i;
  }

  double as_double() const {
    const auto& n = std::get<detail::owned_number_value>(data_);
    if (n.is_int) return static_cast<double>(n.i);
    const auto tok = n.raw_token();
    if (!tok.empty()) {
      if (!n.has_double) {
        n.d = detail::parse_double(tok);
        n.has_double = true;
      }
      return n.d;
    }
    return n.d;
  }

  const std::string& as_string() const { return std::get<std::string>(data_); }
  const array& as_array() const { return std::get<array>(data_); }
  const object& as_object() const { return std::get<object>(data_); }

  array& as_array() { return std::get<array>(data_); }
  object& as_object() { return std::get<object>(data_); }
  std::string& as_string() { return std::get<std::string>(data_); }

  const value* find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    const auto& o = std::get<object>(data_);
    for (const auto& kv : o) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  value* find(std::string_view key) noexcept {
    if (!is_object()) return nullptr;
    auto& o = std::get<object>(data_);
    for (auto& kv : o) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

private:
  // index: 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object
  std::variant<std::monostate, bool, detail::owned_number_value, std::string, array, object> data_;

  friend struct parser;
  friend void dump_to(std::string&, const value&, bool, int);
};

struct parse_options {
  std::size_t max_depth{256};
  bool require_eof{true};
};

// Options for opt-in multithreaded dumping.
//
// Notes:
// - Multithreaded dumping is primarily beneficial for very large arrays/objects.
// - For small values it may be slower due to task overhead.
struct dump_mt_options {
  bool pretty{false};
  // 0 => use std::thread::hardware_concurrency() (clamped to >= 1)
  unsigned max_threads{0};
  // Only parallelize arrays/objects with at least this many items.
  std::size_t min_parallel_items{1024};
  // Limit parallelization depth to avoid task explosion.
  unsigned max_parallel_depth{1};
};

// Forward declaration: forced-mt dump() wrappers call this.
inline void dump_to_mt(std::string& out, const value& v, dump_mt_options opt);

// Options for parallel parsing of many independent JSON texts.
struct parse_many_options {
  parse_options opt{};
  // 0 => use std::thread::hardware_concurrency() (clamped to >= 1)
  unsigned max_threads{0};
};

// Legacy: fully owning DOM value tree (std::string/std::vector backed).
// Kept for programmatic construction + deep-copy parsing when desired.
struct value_parse_result {
  value val;
  error err;
};

struct parser {
  std::string_view s;
  std::size_t i{0};
  parse_options opt;

  value_parse_result run() {
    value_parse_result r;
    detail::skip_ws(s, i);
    r.val = parse_value(0, r.err);
    if (r.err) return r;

    detail::skip_ws(s, i);
    if (opt.require_eof && i != s.size()) {
      set_error(r.err, error_code::trailing_characters);
      return r;
    }
    return r;
  }

  void set_error(error& e, error_code code, std::size_t at = std::numeric_limits<std::size_t>::max()) {
    if (e) return;
    e.code = code;
    e.offset = (at == std::numeric_limits<std::size_t>::max()) ? i : at;
    detail::update_line_col(s, e.offset, e.line, e.column);
  }

  value parse_value(std::size_t depth, error& e) {
    if (depth > opt.max_depth) {
      set_error(e, error_code::nesting_too_deep);
      return nullptr;
    }
    if (i >= s.size()) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }

    const char c = s[i];
    switch (c) {
      case 'n': return parse_literal("null", 4, nullptr, e);
      case 't': return parse_literal("true", 4, value(true), e);
      case 'f': return parse_literal("false", 5, value(false), e);
      case '"': {
        std::string out;
        if (!parse_string(out, e)) return nullptr;
        return value(std::move(out));
      }
      case '[': return parse_array(depth + 1, e);
      case '{': return parse_object(depth + 1, e);
      default: {
        if (c == '-' || (c >= '0' && c <= '9')) {
          detail::owned_number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(s, i, num, /*parse_fp=*/false)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          value v;
          v.data_ = std::move(num);
          return v;
        }
        set_error(e, error_code::invalid_value);
        return nullptr;
      }
    }
  }

  value parse_literal(const char* lit, std::size_t len, value v, error& e) {
    if (i + len > s.size()) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }
    if (std::memcmp(s.data() + i, lit, len) != 0) {
      set_error(e, error_code::invalid_value);
      return nullptr;
    }
    i += len;
    return v;
  }

  bool parse_string(std::string& out, error& e) {
    // JSON string: " ... ", disallow raw control chars.
    if (i >= s.size() || s[i] != '"') {
      set_error(e, error_code::invalid_string);
      return false;
    }
    const std::size_t quote_pos = i;
    ++i;
    out.clear();

    const char* base = s.data();
    const std::size_t n = s.size();
    std::size_t chunk_begin = i;

    while (i < n) {
#if defined(_M_X64) || defined(__SSE2__)
      {
        const __m128i q = _mm_set1_epi8('"');
        const __m128i bs = _mm_set1_epi8('\\');
        const __m128i k1f = _mm_set1_epi8(0x1F);
        const __m128i zero = _mm_setzero_si128();
        while (i + 16 <= n) {
          const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + i));
          const __m128i is_q = _mm_cmpeq_epi8(v, q);
          const __m128i is_bs = _mm_cmpeq_epi8(v, bs);
          const __m128i sub = _mm_subs_epu8(v, k1f);
          const __m128i is_ctrl = _mm_cmpeq_epi8(sub, zero);
          const __m128i any = _mm_or_si128(_mm_or_si128(is_q, is_bs), is_ctrl);
          const int mask = _mm_movemask_epi8(any);
          if (mask == 0) {
            i += 16;
            continue;
          }
#if defined(_MSC_VER)
          unsigned long bit = 0;
          _BitScanForward(&bit, static_cast<unsigned long>(mask));
          i += static_cast<std::size_t>(bit);
#else
          i += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#endif
          break;
        }
      }
#endif
      const unsigned char uc = static_cast<unsigned char>(base[i]);
      const char c = base[i];

      if (c == '"') {
        if (i > chunk_begin) out.append(base + chunk_begin, i - chunk_begin);
        ++i;
        return true;
      }

      if (c == '\\') {
        if (i > chunk_begin) out.append(base + chunk_begin, i - chunk_begin);
        ++i;
        if (i >= n) {
          set_error(e, error_code::unexpected_eof, quote_pos);
          return false;
        }
        const char esc = base[i++];
        switch (esc) {
          case '"': out.push_back('"'); break;
          case '\\': out.push_back('\\'); break;
          case '/': out.push_back('/'); break;
          case 'b': out.push_back('\b'); break;
          case 'f': out.push_back('\f'); break;
          case 'n': out.push_back('\n'); break;
          case 'r': out.push_back('\r'); break;
          case 't': out.push_back('\t'); break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!detail::parse_u4(s, i, cp)) {
              set_error(e, error_code::invalid_unicode_escape, i);
              return false;
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
              if (i + 2 > n || base[i] != '\\' || base[i + 1] != 'u') {
                set_error(e, error_code::invalid_utf16_surrogate, i);
                return false;
              }
              i += 2;
              std::uint32_t low = 0;
              if (!detail::parse_u4(s, i, low)) {
                set_error(e, error_code::invalid_unicode_escape, i);
                return false;
              }
              if (low < 0xDC00u || low > 0xDFFFu) {
                set_error(e, error_code::invalid_utf16_surrogate, i);
                return false;
              }
              const std::uint32_t hi = cp - 0xD800u;
              const std::uint32_t lo = low - 0xDC00u;
              cp = 0x10000u + ((hi << 10) | lo);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
              set_error(e, error_code::invalid_utf16_surrogate, i);
              return false;
            }
            detail::append_utf8(out, cp);
            break;
          }
          default:
            set_error(e, error_code::invalid_escape, i - 1);
            return false;
        }
        chunk_begin = i;
        continue;
      }

      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, i);
        return false;
      }

      ++i;
    }

    set_error(e, error_code::unexpected_eof, quote_pos);
    return false;
  }

  value parse_array(std::size_t depth, error& e) {
    assert(i < s.size() && s[i] == '[');
    ++i;
    detail::skip_ws(s, i);

    value::array a;
    // Conservative reserve for the common pattern of a large top-level array.
    // Avoid aggressive over-allocation; small/medium arrays keep the default growth.
    if (depth == 1 && i < s.size()) {
      const std::size_t remaining = s.size() - i;
      if (remaining >= 8192) {
        std::size_t est = remaining / 72;
        if (est > 4096) est = 4096;
        if (est >= 16) a.reserve(est);
      }
    }
    if (i < s.size() && s[i] == ']') {
      ++i;
      return value(std::move(a));
    }

    while (true) {
      detail::skip_ws(s, i);
      value elem = parse_value(depth, e);
      if (e) return nullptr;
      a.emplace_back(std::move(elem));

      detail::skip_ws(s, i);
      if (i >= s.size()) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = s[i++];
      if (c == ',') {
        continue;
      }
      if (c == ']') {
        return value(std::move(a));
      }
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }

  value parse_object(std::size_t depth, error& e) {
    assert(i < s.size() && s[i] == '{');
    ++i;
    detail::skip_ws(s, i);

    value::object o;
    // Most objects are small; reserving 4 avoids a couple of reallocations
    // without over-allocating like a larger fixed reserve.
    (void)depth;
    o.reserve(4);
    if (i < s.size() && s[i] == '}') {
      ++i;
      return value(std::move(o));
    }

    while (true) {
      detail::skip_ws(s, i);
      if (i >= s.size()) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (s[i] != '"') {
        set_error(e, error_code::expected_key_string);
        return nullptr;
      }
      std::string key;
      if (!parse_string(key, e)) return nullptr;

      detail::skip_ws(s, i);
      if (i >= s.size()) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (s[i] != ':') {
        set_error(e, error_code::expected_colon);
        return nullptr;
      }
      ++i;

      detail::skip_ws(s, i);
      value v = parse_value(depth, e);
      if (e) return nullptr;
      o.emplace_back(std::move(key), std::move(v));

      detail::skip_ws(s, i);
      if (i >= s.size()) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = s[i++];
      if (c == ',') continue;
      if (c == '}') return value(std::move(o));
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }
};

inline value_parse_result parse_value(std::string_view json, parse_options opt = {}) {
  parser p;
  p.s = json;
  p.opt = opt;
  return p.run();
}

inline value parse_value_or_throw(std::string_view json, parse_options opt = {}) {
  auto r = parse_value(json, opt);
  if (r.err) throw std::runtime_error("chjson: parse failed");
  return std::move(r.val);
}

namespace detail {
inline bool needs_escaping(std::string_view s) noexcept {
  const char* p = s.data();
  const std::size_t n = s.size();

#if defined(_M_X64) || defined(__SSE2__)
  const __m128i q = _mm_set1_epi8('"');
  const __m128i bs = _mm_set1_epi8('\\');
  const __m128i k1f = _mm_set1_epi8(0x1F);
  const __m128i zero = _mm_setzero_si128();

  std::size_t i = 0;
  while (i + 16 <= n) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    const __m128i is_q = _mm_cmpeq_epi8(v, q);
    const __m128i is_bs = _mm_cmpeq_epi8(v, bs);
    // Unsigned check for v <= 0x1F using saturated subtract.
    const __m128i sub = _mm_subs_epu8(v, k1f);
    const __m128i is_ctrl = _mm_cmpeq_epi8(sub, zero);
    const __m128i any = _mm_or_si128(_mm_or_si128(is_q, is_bs), is_ctrl);
    if (_mm_movemask_epi8(any) != 0) return true;
    i += 16;
  }
  for (; i < n; ++i) {
    const unsigned char uc = static_cast<unsigned char>(p[i]);
    const char c = p[i];
    if (c == '"' || c == '\\' || uc <= 0x1F) return true;
  }
  return false;
#else
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char uc = static_cast<unsigned char>(p[i]);
    const char c = p[i];
    if (c == '"' || c == '\\' || uc <= 0x1F) return true;
  }
  return false;
#endif
}

inline std::size_t find_first_escape(std::string_view s) noexcept {
  const char* p = s.data();
  const std::size_t n = s.size();

#if defined(_M_X64) || defined(__SSE2__)
  const __m128i q = _mm_set1_epi8('"');
  const __m128i bs = _mm_set1_epi8('\\');
  const __m128i k1f = _mm_set1_epi8(0x1F);
  const __m128i zero = _mm_setzero_si128();

  std::size_t i = 0;
  while (i + 16 <= n) {
    const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + i));
    const __m128i is_q = _mm_cmpeq_epi8(v, q);
    const __m128i is_bs = _mm_cmpeq_epi8(v, bs);
    const __m128i sub = _mm_subs_epu8(v, k1f);
    const __m128i is_ctrl = _mm_cmpeq_epi8(sub, zero);
    const __m128i any = _mm_or_si128(_mm_or_si128(is_q, is_bs), is_ctrl);
    const int mask = _mm_movemask_epi8(any);
    if (mask != 0) {
#if defined(_MSC_VER)
      unsigned long bit = 0;
      _BitScanForward(&bit, static_cast<unsigned long>(mask));
      return i + static_cast<std::size_t>(bit);
#else
      return i + static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#endif
    }
    i += 16;
  }
  for (; i < n; ++i) {
    const unsigned char uc = static_cast<unsigned char>(p[i]);
    const char c = p[i];
    if (c == '"' || c == '\\' || uc <= 0x1F) return i;
  }
  return n;
#else
  for (std::size_t i = 0; i < n; ++i) {
    const unsigned char uc = static_cast<unsigned char>(p[i]);
    const char c = p[i];
    if (c == '"' || c == '\\' || uc <= 0x1F) return i;
  }
  return n;
#endif
}

inline void dump_escaped(std::string& out, std::string_view s) {
  static constexpr char hex[] = "0123456789ABCDEF";

  const char* data = s.data();
  const std::size_t n = s.size();
  const std::size_t first = find_first_escape(s);

  out.push_back('"');
  if (first == n) {
    out.append(data, n);
    out.push_back('"');
    return;
  }

  if (first > 0) out.append(data, first);

  std::size_t chunk_begin = first;
  for (std::size_t i = first; i < n; ++i) {
    const unsigned char uc = static_cast<unsigned char>(data[i]);
    const char c = data[i];

    const char* esc = nullptr;
    std::size_t esc_len = 0;

    switch (c) {
      case '"': esc = "\\\""; esc_len = 2; break;
      case '\\': esc = "\\\\"; esc_len = 2; break;
      case '\b': esc = "\\b"; esc_len = 2; break;
      case '\f': esc = "\\f"; esc_len = 2; break;
      case '\n': esc = "\\n"; esc_len = 2; break;
      case '\r': esc = "\\r"; esc_len = 2; break;
      case '\t': esc = "\\t"; esc_len = 2; break;
      default: break;
    }

    if (esc != nullptr) {
      if (i > chunk_begin) out.append(data + chunk_begin, i - chunk_begin);
      out.append(esc, esc_len);
      chunk_begin = i + 1;
      continue;
    }

    if (uc <= 0x1F) {
      if (i > chunk_begin) out.append(data + chunk_begin, i - chunk_begin);
      out.append("\\u00", 4);
      out.push_back(hex[(uc >> 4) & 0xF]);
      out.push_back(hex[uc & 0xF]);
      chunk_begin = i + 1;
      continue;
    }
  }

  if (n > chunk_begin) out.append(data + chunk_begin, n - chunk_begin);
  out.push_back('"');
}

inline void dump_int64(std::string& out, std::int64_t v) {
  char buf[32];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  if (r.ec != std::errc{}) {
    throw std::runtime_error("chjson: failed to format integer");
  }
  out.append(buf, static_cast<std::size_t>(r.ptr - buf));
}

inline std::size_t estimate_dump_reserve(const value& v, bool pretty);

inline std::size_t estimate_string_min(std::string_view s) {
  // Minimal estimate: quotes + raw bytes (does not account for escapes expanding).
  return 2 + s.size();
}

inline std::size_t estimate_dump_reserve(const value& v, bool pretty) {
  (void)pretty;
  switch (v.type()) {
    case value::kind::null: return 4;
    case value::kind::boolean: return 5;
    case value::kind::number: {
      return v.is_int() ? 24u : 32u;
    }
    case value::kind::string:
      return estimate_string_min(v.as_string());
    case value::kind::array: {
      const auto& a = v.as_array();
      std::size_t sum = 2; // [ ]
      if (!a.empty()) sum += (a.size() - 1); // commas
      for (const auto& e : a) sum += estimate_dump_reserve(e, pretty);
      return sum;
    }
    case value::kind::object: {
      const auto& o = v.as_object();
      std::size_t sum = 2; // { }
      if (!o.empty()) sum += (o.size() - 1); // commas
      for (const auto& kv : o) {
        sum += estimate_string_min(kv.first);
        sum += 1; // ':'
        sum += estimate_dump_reserve(kv.second, pretty);
      }
      return sum;
    }
  }
  return 256;
}

inline void dump_double(std::string& out, double d) {
  if (!std::isfinite(d)) {
    throw std::runtime_error("chjson: cannot dump NaN/Inf as JSON number");
  }
  // Prefer std::to_chars: fast, locale-independent, allocation-free.
  // Use the shortest general representation first (typically the fastest),
  // and fall back to a max_digits10 round-trip path if needed.
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general);
  if (r.ec == std::errc{}) {
    out.append(buf, static_cast<std::size_t>(r.ptr - buf));
    return;
  }

  r = std::to_chars(buf, buf + sizeof(buf), d, std::chars_format::general,
                    std::numeric_limits<double>::max_digits10);
  if (r.ec == std::errc{}) {
    out.append(buf, static_cast<std::size_t>(r.ptr - buf));
    return;
  }

  // Fallback: should be rare (e.g., implementation limitations).
  const int n = std::snprintf(buf, sizeof(buf), "%.17g", d);
  if (n <= 0) throw std::runtime_error("chjson: failed to format double");
  if (static_cast<std::size_t>(n) < sizeof(buf)) {
    out.append(buf, static_cast<std::size_t>(n));
    return;
  }
  std::string tmp(static_cast<std::size_t>(n) + 1, '\0');
  std::snprintf(&tmp[0], tmp.size(), "%.17g", d);
  tmp.pop_back();
  out += tmp;
}

inline void dump_indent(std::string& out, int indent) {
  for (int i = 0; i < indent; ++i) out.push_back(' ');
}

inline unsigned effective_threads(unsigned max_threads) noexcept {
  if (max_threads == 0) {
    const unsigned hc = std::thread::hardware_concurrency();
    return hc == 0 ? 1u : hc;
  }
  return max_threads;
}

} // namespace detail

namespace detail {

inline void dump_compact_iter(std::string& out, const value& root) {
  struct frame {
    const value* v{nullptr};
    std::size_t idx{0};
  };
  // Non-recursive traversal with a fixed-size stack to avoid per-call heap allocs.
  // Depth is bounded by parser max-depth; keep a generous safety margin.
  frame stack[512];
  std::size_t sp = 0;
  stack[sp++] = frame{&root, 0};

  while (sp != 0) {
    frame& f = stack[sp - 1];
    const value& v = *f.v;

    switch (v.type()) {
      case value::kind::null:
        out += "null";
        --sp;
        break;
      case value::kind::boolean:
        out += v.as_bool() ? "true" : "false";
        --sp;
        break;
      case value::kind::number:
        if (v.is_int()) dump_int64(out, v.as_int());
        else dump_double(out, v.as_double());
        --sp;
        break;
      case value::kind::string:
        dump_escaped(out, v.as_string());
        --sp;
        break;
      case value::kind::array: {
        const auto& a = v.as_array();
        if (f.idx == 0) {
          out.push_back('[');
          if (a.empty()) {
            out.push_back(']');
            --sp;
            break;
          }
        }
        if (f.idx == a.size()) {
          out.push_back(']');
          --sp;
          break;
        }
        if (f.idx > 0) out.push_back(',');
        const value* child = &a[f.idx++];
        stack[sp++] = frame{child, 0};
        break;
      }
      case value::kind::object: {
        const auto& o = v.as_object();
        if (f.idx == 0) {
          out.push_back('{');
          if (o.empty()) {
            out.push_back('}');
            --sp;
            break;
          }
        }
        if (f.idx == o.size()) {
          out.push_back('}');
          --sp;
          break;
        }
        if (f.idx > 0) out.push_back(',');
        const auto& kv = o[f.idx++];
        dump_escaped(out, kv.first);
        out.push_back(':');
        stack[sp++] = frame{&kv.second, 0};
        break;
      }
    }
  }
}

} // namespace detail

inline void dump_to_compact(std::string& out, const value& v);

inline void dump_to_pretty(std::string& out, const value& v, int indent) {
  switch (v.type()) {
    case value::kind::null:
      out += "null";
      return;
    case value::kind::boolean:
      out += v.as_bool() ? "true" : "false";
      return;
    case value::kind::number: {
      if (v.is_int()) detail::dump_int64(out, v.as_int());
      else detail::dump_double(out, v.as_double());
      return;
    }
    case value::kind::string:
      detail::dump_escaped(out, v.as_string());
      return;
    case value::kind::array: {
      const auto& a = v.as_array();
      out.push_back('[');
      if (!a.empty()) out.push_back('\n');
      for (std::size_t idx = 0; idx < a.size(); ++idx) {
        detail::dump_indent(out, indent + 2);
        dump_to_pretty(out, a[idx], indent + 2);
        if (idx + 1 != a.size()) out.push_back(',');
        out.push_back('\n');
      }
      if (!a.empty()) detail::dump_indent(out, indent);
      out.push_back(']');
      return;
    }
    case value::kind::object: {
      const auto& o = v.as_object();
      out.push_back('{');
      if (!o.empty()) out.push_back('\n');
      for (std::size_t idx = 0; idx < o.size(); ++idx) {
        detail::dump_indent(out, indent + 2);
        detail::dump_escaped(out, o[idx].first);
        out.append(": ", 2);
        dump_to_pretty(out, o[idx].second, indent + 2);
        if (idx + 1 != o.size()) out.push_back(',');
        out.push_back('\n');
      }
      if (!o.empty()) detail::dump_indent(out, indent);
      out.push_back('}');
      return;
    }
  }
}

inline void dump_to_compact(std::string& out, const value& v) {
  dump_to(out, v, /*pretty=*/false, 0);
}

inline void dump_to(std::string& out, const value& v, bool pretty = false, int indent = 0) {
  if (!pretty) {
    struct frame {
      const value* v{nullptr};
      std::size_t idx{0};
    };
    frame stack[512];
    std::size_t sp = 0;
    stack[sp++] = frame{&v, 0};

    while (sp != 0) {
      frame& f = stack[sp - 1];
      const value& cur = *f.v;

      switch (cur.data_.index()) {
        case 0: // null
          out.append("null", 4);
          --sp;
          break;
        case 1: // bool
          if (std::get<bool>(cur.data_)) out.append("true", 4);
          else out.append("false", 5);
          --sp;
          break;
        case 2: { // number
          const auto& n = std::get<detail::owned_number_value>(cur.data_);
          if (n.is_int) {
            detail::dump_int64(out, n.i);
          } else if (const auto tok = n.raw_token(); !tok.empty()) {
            out.append(tok.data(), tok.size());
          } else {
            detail::dump_double(out, n.d);
          }
          --sp;
          break;
        }
        case 3: { // string
          const auto& s = std::get<std::string>(cur.data_);
          detail::dump_escaped(out, s);
          --sp;
          break;
        }
        case 4: { // array
          const auto& a = std::get<value::array>(cur.data_);
          if (f.idx == 0) {
            out.push_back('[');
            if (a.empty()) {
              out.push_back(']');
              --sp;
              break;
            }
          }
          if (f.idx == a.size()) {
            out.push_back(']');
            --sp;
            break;
          }
          if (f.idx > 0) out.push_back(',');
          stack[sp++] = frame{&a[f.idx++], 0};
          break;
        }
        case 5: { // object
          const auto& o = std::get<value::object>(cur.data_);
          if (f.idx == 0) {
            out.push_back('{');
            if (o.empty()) {
              out.push_back('}');
              --sp;
              break;
            }
          }
          if (f.idx == o.size()) {
            out.push_back('}');
            --sp;
            break;
          }
          if (f.idx > 0) out.push_back(',');
          const auto& kv = o[f.idx++];
          detail::dump_escaped(out, kv.first);
          out.push_back(':');
          stack[sp++] = frame{&kv.second, 0};
          break;
        }
        default:
          out.append("null", 4);
          --sp;
          break;
      }
    }
    return;
  }
  dump_to_pretty(out, v, indent);
}

inline std::string dump(const value& v, bool pretty = false) {
  struct dump_reserve_hints {
    std::size_t compact{256};
    std::size_t pretty{256};
  };
  thread_local dump_reserve_hints hints;

  std::string out;
  std::size_t& hint = pretty ? hints.pretty : hints.compact;
  out.reserve(hint);

  // Default dump(): use the MT-aware dumper.
  // It will only parallelize when the top-level container is large enough.
  dump_mt_options opt;
  opt.pretty = pretty;
  opt.max_threads = 0;
  opt.min_parallel_items = 1024;
  opt.max_parallel_depth = 1;
  dump_to_mt(out, v, opt);

  constexpr std::size_t min_hint = 256;
  constexpr std::size_t max_hint = 16u * 1024u * 1024u;
  const std::size_t sz = out.size();
  hint = (sz < min_hint) ? min_hint : (sz > max_hint ? max_hint : sz);
  return out;
}

// -----------------------------
// Multithreaded dump (opt-in)
// -----------------------------

namespace detail {

template <class Fn>
inline void parallel_for_chunks(std::size_t n, unsigned threads, Fn&& fn) {
  if (n == 0 || threads <= 1) {
    fn(0, n);
    return;
  }
  if (threads > static_cast<unsigned>(n)) threads = static_cast<unsigned>(n);

  const std::size_t base = n / threads;
  const std::size_t rem = n % threads;

  std::unique_ptr<std::thread[]> thr(new std::thread[threads]);
  std::unique_ptr<std::exception_ptr[]> ep(new std::exception_ptr[threads]);
  std::size_t begin = 0;
  for (unsigned t = 0; t < threads; ++t) {
    const std::size_t len = base + (t < rem ? 1u : 0u);
    const std::size_t end = begin + len;
    ep[t] = nullptr;
    thr[t] = std::thread([&, t, begin, end]() {
      try {
        fn(begin, end);
      } catch (...) {
        ep[t] = std::current_exception();
      }
    });
    begin = end;
  }
  for (unsigned t = 0; t < threads; ++t) {
    if (thr[t].joinable()) thr[t].join();
  }
  for (unsigned t = 0; t < threads; ++t) {
    if (ep[t]) std::rethrow_exception(ep[t]);
  }
}

inline bool should_parallelize_container(std::size_t items, unsigned depth, const dump_mt_options& opt, unsigned threads) noexcept {
  if (threads <= 1) return false;
  if (depth >= opt.max_parallel_depth) return false;
  return items >= opt.min_parallel_items;
}

inline void dump_array_pretty_mt(std::string& out, const value::array& a, int indent, unsigned depth, const dump_mt_options& opt, unsigned threads) {
  out.push_back('[');
  if (a.empty()) {
    out.push_back(']');
    return;
  }
  out.push_back('\n');

  const bool par = should_parallelize_container(a.size(), depth, opt, threads);
  if (!par) {
    for (std::size_t i = 0; i < a.size(); ++i) {
      dump_indent(out, indent + 2);
      dump_to(out, a[i], /*pretty=*/true, indent + 2);
      if (i + 1 != a.size()) out.push_back(',');
      out.push_back('\n');
    }
    dump_indent(out, indent);
    out.push_back(']');
    return;
  }

  const unsigned used = threads > static_cast<unsigned>(a.size()) ? static_cast<unsigned>(a.size()) : threads;
  std::unique_ptr<std::string[]> chunks(new std::string[used]);
  {
    const std::size_t n = a.size();
    const std::size_t base = n / used;
    const std::size_t rem = n % used;
    std::unique_ptr<std::thread[]> thr(new std::thread[used]);
    std::unique_ptr<std::exception_ptr[]> ep(new std::exception_ptr[used]);
    std::size_t begin = 0;
    for (unsigned t = 0; t < used; ++t) {
      const std::size_t len = base + (t < rem ? 1u : 0u);
      const std::size_t end = begin + len;
      ep[t] = nullptr;
      thr[t] = std::thread([&, t, begin, end]() {
        try {
          std::string buf;
          for (std::size_t i = begin; i < end; ++i) {
            dump_indent(buf, indent + 2);
            dump_to(buf, a[i], /*pretty=*/true, indent + 2);
            if (i + 1 != a.size()) buf.push_back(',');
            buf.push_back('\n');
          }
          chunks[t] = std::move(buf);
        } catch (...) {
          ep[t] = std::current_exception();
        }
      });
      begin = end;
    }
    for (unsigned t = 0; t < used; ++t) {
      if (thr[t].joinable()) thr[t].join();
    }
    for (unsigned t = 0; t < used; ++t) {
      if (ep[t]) std::rethrow_exception(ep[t]);
    }
  }

  for (unsigned t = 0; t < used; ++t) out.append(chunks[t]);
  dump_indent(out, indent);
  out.push_back(']');
}

inline void dump_object_pretty_mt(std::string& out, const value::object& o, int indent, unsigned depth, const dump_mt_options& opt, unsigned threads) {
  out.push_back('{');
  if (o.empty()) {
    out.push_back('}');
    return;
  }
  out.push_back('\n');

  const bool par = should_parallelize_container(o.size(), depth, opt, threads);
  if (!par) {
    for (std::size_t i = 0; i < o.size(); ++i) {
      dump_indent(out, indent + 2);
      dump_escaped(out, o[i].first);
      out.append(": ", 2);
      dump_to(out, o[i].second, /*pretty=*/true, indent + 2);
      if (i + 1 != o.size()) out.push_back(',');
      out.push_back('\n');
    }
    dump_indent(out, indent);
    out.push_back('}');
    return;
  }

  const unsigned used = threads > static_cast<unsigned>(o.size()) ? static_cast<unsigned>(o.size()) : threads;
  std::unique_ptr<std::string[]> chunks(new std::string[used]);
  {
    const std::size_t n = o.size();
    const std::size_t base = n / used;
    const std::size_t rem = n % used;
    std::unique_ptr<std::thread[]> thr(new std::thread[used]);
    std::unique_ptr<std::exception_ptr[]> ep(new std::exception_ptr[used]);
    std::size_t begin = 0;
    for (unsigned t = 0; t < used; ++t) {
      const std::size_t len = base + (t < rem ? 1u : 0u);
      const std::size_t end = begin + len;
      ep[t] = nullptr;
      thr[t] = std::thread([&, t, begin, end]() {
        try {
          std::string buf;
          for (std::size_t i = begin; i < end; ++i) {
            dump_indent(buf, indent + 2);
            dump_escaped(buf, o[i].first);
            buf.append(": ", 2);
            dump_to(buf, o[i].second, /*pretty=*/true, indent + 2);
            if (i + 1 != o.size()) buf.push_back(',');
            buf.push_back('\n');
          }
          chunks[t] = std::move(buf);
        } catch (...) {
          ep[t] = std::current_exception();
        }
      });
      begin = end;
    }
    for (unsigned t = 0; t < used; ++t) {
      if (thr[t].joinable()) thr[t].join();
    }
    for (unsigned t = 0; t < used; ++t) {
      if (ep[t]) std::rethrow_exception(ep[t]);
    }
  }

  for (unsigned t = 0; t < used; ++t) out.append(chunks[t]);
  dump_indent(out, indent);
  out.push_back('}');
}

} // namespace detail

inline void dump_to_mt(std::string& out, const value& v, dump_mt_options opt = {}) {
  // Fast reject: only parallelize top-level arrays/objects that are large enough.
  // This keeps default dump() MT-aware while avoiding per-call scheduling overhead
  // in tight loops dumping small values.
  if (opt.max_parallel_depth == 0) {
    dump_to(out, v, opt.pretty, 0);
    return;
  }

  if (v.type() == value::kind::array) {
    if (v.as_array().size() < opt.min_parallel_items) {
      dump_to(out, v, opt.pretty, 0);
      return;
    }
  } else if (v.type() == value::kind::object) {
    if (v.as_object().size() < opt.min_parallel_items) {
      dump_to(out, v, opt.pretty, 0);
      return;
    }
  } else {
    dump_to(out, v, opt.pretty, 0);
    return;
  }

  const unsigned threads = detail::effective_threads(opt.max_threads);
  if (threads <= 1) {
    dump_to(out, v, opt.pretty, 0);
    return;
  }

  // Parallelize only for arrays/objects; everything else uses the existing dump_to.
  if (!opt.pretty) {
    if (v.type() == value::kind::array) {
      const auto& a = v.as_array();
      if (detail::should_parallelize_container(a.size(), /*depth=*/0, opt, threads)) {
        const unsigned used = threads > static_cast<unsigned>(a.size()) ? static_cast<unsigned>(a.size()) : threads;
        std::unique_ptr<std::string[]> chunks(new std::string[used]);
        const std::size_t n = a.size();
        const std::size_t base = n / used;
        const std::size_t rem = n % used;
        std::unique_ptr<std::thread[]> thr(new std::thread[used]);
        std::unique_ptr<std::exception_ptr[]> ep(new std::exception_ptr[used]);
        std::size_t begin = 0;
        for (unsigned t = 0; t < used; ++t) {
          const std::size_t len = base + (t < rem ? 1u : 0u);
          const std::size_t end = begin + len;
          ep[t] = nullptr;
          thr[t] = std::thread([&, t, begin, end]() {
            try {
              std::string buf;
              for (std::size_t i = begin; i < end; ++i) {
                if (i != begin) buf.push_back(',');
                dump_to(buf, a[i], /*pretty=*/false, 0);
              }
              chunks[t] = std::move(buf);
            } catch (...) {
              ep[t] = std::current_exception();
            }
          });
          begin = end;
        }
        for (unsigned t = 0; t < used; ++t) {
          if (thr[t].joinable()) thr[t].join();
        }
        for (unsigned t = 0; t < used; ++t) {
          if (ep[t]) std::rethrow_exception(ep[t]);
        }

        out.push_back('[');
        bool first = true;
        for (unsigned t = 0; t < used; ++t) {
          if (chunks[t].empty()) continue;
          if (!first) out.push_back(',');
          out.append(chunks[t]);
          first = false;
        }
        out.push_back(']');
        return;
      }
    } else if (v.type() == value::kind::object) {
      const auto& o = v.as_object();
      if (detail::should_parallelize_container(o.size(), /*depth=*/0, opt, threads)) {
        const unsigned used = threads > static_cast<unsigned>(o.size()) ? static_cast<unsigned>(o.size()) : threads;
        std::unique_ptr<std::string[]> chunks(new std::string[used]);
        const std::size_t n = o.size();
        const std::size_t base = n / used;
        const std::size_t rem = n % used;
        std::unique_ptr<std::thread[]> thr(new std::thread[used]);
        std::unique_ptr<std::exception_ptr[]> ep(new std::exception_ptr[used]);
        std::size_t begin = 0;
        for (unsigned t = 0; t < used; ++t) {
          const std::size_t len = base + (t < rem ? 1u : 0u);
          const std::size_t end = begin + len;
          ep[t] = nullptr;
          thr[t] = std::thread([&, t, begin, end]() {
            try {
              std::string buf;
              for (std::size_t i = begin; i < end; ++i) {
                if (i != begin) buf.push_back(',');
                detail::dump_escaped(buf, o[i].first);
                buf.push_back(':');
                dump_to(buf, o[i].second, /*pretty=*/false, 0);
              }
              chunks[t] = std::move(buf);
            } catch (...) {
              ep[t] = std::current_exception();
            }
          });
          begin = end;
        }
        for (unsigned t = 0; t < used; ++t) {
          if (thr[t].joinable()) thr[t].join();
        }
        for (unsigned t = 0; t < used; ++t) {
          if (ep[t]) std::rethrow_exception(ep[t]);
        }

        out.push_back('{');
        bool first = true;
        for (unsigned t = 0; t < used; ++t) {
          if (chunks[t].empty()) continue;
          if (!first) out.push_back(',');
          out.append(chunks[t]);
          first = false;
        }
        out.push_back('}');
        return;
      }
    }

    dump_to(out, v, /*pretty=*/false, 0);
    return;
  }

  // pretty
  if (v.type() == value::kind::array) {
    const auto& a = v.as_array();
    detail::dump_array_pretty_mt(out, a, /*indent=*/0, /*depth=*/0, opt, threads);
    return;
  }
  if (v.type() == value::kind::object) {
    const auto& o = v.as_object();
    detail::dump_object_pretty_mt(out, o, /*indent=*/0, /*depth=*/0, opt, threads);
    return;
  }

  dump_to(out, v, /*pretty=*/true, 0);
}

inline std::string dump_mt(const value& v, dump_mt_options opt = {}) {
  std::string out;
  out.reserve(detail::estimate_dump_reserve(v, opt.pretty));
  dump_to_mt(out, v, opt);
  return out;
}

// -----------------------------
// PMR arena + in-situ DOM
// -----------------------------

namespace pmr {

class arena_resource final : public std::pmr::memory_resource {
public:
  explicit arena_resource(std::size_t initial_block_size = 64 * 1024)
  : initial_block_size_(initial_block_size) {}

  arena_resource(const arena_resource&) = delete;
  arena_resource& operator=(const arena_resource&) = delete;

  arena_resource(arena_resource&& other) noexcept {
    *this = std::move(other);
  }

  arena_resource& operator=(arena_resource&& other) noexcept {
    if (this == &other) return *this;
    release();
    head_ = other.head_;
    tail_ = other.tail_;
    current_ = other.current_;
    initial_block_size_ = other.initial_block_size_;
    next_block_size_ = other.next_block_size_;
    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.current_ = nullptr;
    other.next_block_size_ = 0;
    return *this;
  }

  ~arena_resource() override { release(); }

  // Clear allocations but keep committed blocks for reuse.
  // This is the typical "bump allocator reset" operation.
  void clear() noexcept {
    for (block* b = head_; b != nullptr; b = b->next) b->used = 0;
    current_ = head_;

    // Keep growth state consistent with already committed capacity.
    if (tail_) {
      const std::size_t doubled = tail_->size <= (k_max_block_size / 2) ? (tail_->size * 2) : k_max_block_size;
      next_block_size_ = doubled < min_block_size() ? min_block_size() : doubled;
    } else {
      next_block_size_ = min_block_size();
    }
  }

  // Reset to empty (frees all blocks).
  void reset() noexcept { release(); }

  std::size_t blocks() const noexcept {
    std::size_t n = 0;
    for (block* b = head_; b != nullptr; b = b->next) ++n;
    return n;
  }
  std::size_t bytes_committed() const noexcept {
    std::size_t sum = 0;
    for (block* b = head_; b != nullptr; b = b->next) sum += b->size;
    return sum;
  }
  std::size_t bytes_used() const noexcept {
    std::size_t sum = 0;
    for (block* b = head_; b != nullptr; b = b->next) sum += b->used;
    return sum;
  }

  // Ensure at least `bytes` of backing storage is committed.
  // This can dramatically reduce the number of heap allocations when parsing
  // into a fresh document (where the arena would otherwise grow via many small
  // blocks).
  void reserve_bytes(std::size_t bytes) {
    if (bytes == 0) return;
    std::size_t committed = bytes_committed();
    if (committed >= bytes) {
      // Ensure growth state isn't stuck at 0 when reusing cached blocks.
      if (next_block_size_ == 0) {
        if (tail_) {
          const std::size_t doubled = tail_->size <= (k_max_block_size / 2) ? (tail_->size * 2) : k_max_block_size;
          next_block_size_ = doubled < min_block_size() ? min_block_size() : doubled;
        } else {
          next_block_size_ = min_block_size();
        }
      }
      return;
    }

    // Do not advance current_ while reserving; keep allocation starting point.
    while (committed < bytes) {
      const std::size_t need = bytes - committed;
      const std::size_t block_size = new_block_size(need);

      block* nb = block::create(block_size);
      if (!head_) {
        head_ = tail_ = nb;
      } else {
        tail_->next = nb;
        tail_ = nb;
      }
      committed += block_size;
    }

    if (!current_) current_ = head_;
  }

  void release() noexcept {
    block* b = head_;
    while (b) {
      block* next = b->next;
      block::destroy(b);
      b = next;
    }
    head_ = nullptr;
    tail_ = nullptr;
    current_ = nullptr;
    next_block_size_ = 0;
  }

private:
  struct block {
    std::size_t size{0};
    std::size_t used{0};
    block* next{nullptr};

    std::byte* data() noexcept { return reinterpret_cast<std::byte*>(this + 1); }
    const std::byte* data() const noexcept { return reinterpret_cast<const std::byte*>(this + 1); }

    static block* create(std::size_t block_size) {
      block* nb = static_cast<block*>(detail::chjson_allocate(sizeof(block) + block_size, alignof(block)));
      nb->size = block_size;
      nb->used = 0;
      nb->next = nullptr;
      return nb;
    }

    static void destroy(block* b) noexcept {
      if (!b) return;
      detail::chjson_deallocate(b, sizeof(block) + b->size, alignof(block));
    }
  };

  block* head_{nullptr};
  block* tail_{nullptr};
  block* current_{nullptr};
  std::size_t initial_block_size_{64 * 1024};
  std::size_t next_block_size_{0};

  static constexpr std::size_t k_default_initial_block_size = 64u * 1024u;
  static constexpr std::size_t k_max_block_size = 32u * 1024u * 1024u;

  std::size_t min_block_size() const noexcept {
    return initial_block_size_ ? initial_block_size_ : k_default_initial_block_size;
  }

  std::size_t new_block_size(std::size_t min_needed) noexcept {
    const std::size_t min_block = min_block_size();
    if (next_block_size_ == 0) next_block_size_ = min_block;

    std::size_t growth = next_block_size_;
    if (growth < min_block) growth = min_block;
    if (growth > k_max_block_size) growth = k_max_block_size;

    const std::size_t block_size = (min_needed > growth) ? min_needed : growth;

    // Geometric growth up to 32MiB; huge single allocations do not increase the cap.
    if (block_size >= k_max_block_size) {
      next_block_size_ = k_max_block_size;
    } else {
      const std::size_t doubled = (block_size <= (k_max_block_size / 2)) ? (block_size * 2) : k_max_block_size;
      next_block_size_ = doubled < min_block ? min_block : doubled;
    }

    return block_size;
  }

  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (bytes == 0) bytes = 1;
    if (alignment == 0) alignment = alignof(std::max_align_t);
    if ((alignment & (alignment - 1)) != 0) {
      // alignment must be power-of-two per pmr contract
      throw std::bad_alloc();
    }

    auto try_block = [&](block* b) -> void* {
      if (!b) return nullptr;
      std::uintptr_t base = reinterpret_cast<std::uintptr_t>(b->data()) + b->used;
      std::uintptr_t aligned = (base + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1u);
      const std::size_t padding = static_cast<std::size_t>(aligned - base);
      if (b->used + padding + bytes <= b->size) {
        b->used += padding + bytes;
        return reinterpret_cast<void*>(aligned);
      }
      return nullptr;
    };

    // Try current and subsequent existing blocks first.
    for (block* b = current_ ? current_ : head_; b != nullptr; b = b->next) {
      if (void* p = try_block(b)) {
        current_ = b;
        return p;
      }
    }

    // Need a new block.
    const std::size_t min_needed = bytes + alignment;
    const std::size_t block_size = new_block_size(min_needed);

    block* nb = block::create(block_size);
    if (!head_) {
      head_ = tail_ = nb;
    } else {
      tail_->next = nb;
      tail_ = nb;
    }
    current_ = nb;

    // Allocate from the fresh block.
    if (void* p = try_block(nb)) return p;
    throw std::bad_alloc();
  }

  void do_deallocate(void*, std::size_t, std::size_t) override {
    // monotonic: no-op
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
};

} // namespace pmr

struct sv_raw_view {
  const char* data;
  std::uint32_t size;
};

struct sv_number_value {
  // NOTE: This type must be trivially default constructible because it lives in
  // a union inside sv_value.
  //
  // Layout note (performance): sv_value size is dominated by the number payload.
  // Pack number flags into the high bits of raw.size to keep sv_number_value small.
  // raw.size low 30 bits: raw token size (0..(1<<30)-1)
  // raw.size bit 30: is_int
  // raw.size bit 31: has_double
  // Stores either:
  // - int64 bits when is_int()==true
  // - double bits when has_double()==true
  // When the value is a float token and has_double()==false, payload is unspecified.
  mutable std::uint64_t payload;
  const char* raw_data;
  mutable std::uint32_t raw_size_flags;

  std::int64_t int_value() const noexcept {
    std::int64_t x;
    std::memcpy(&x, &payload, sizeof(x));
    return x;
  }

  void set_int_value(std::int64_t v) const noexcept {
    std::memcpy(&payload, &v, sizeof(v));
  }

  double double_value() const noexcept {
    double x;
    std::memcpy(&x, &payload, sizeof(double));
    return x;
  }

  void set_double_value(double v) const noexcept {
    std::memcpy(&payload, &v, sizeof(double));
  }

  static constexpr std::uint32_t k_size_mask = (1u << 30) - 1u;
  static constexpr std::uint32_t k_is_int = 1u << 30;
  static constexpr std::uint32_t k_has_double = 1u << 31;

  std::uint32_t raw_size() const noexcept { return raw_size_flags & k_size_mask; }
  bool has_raw() const noexcept { return raw_data != nullptr && raw_size() != 0; }
  bool is_int() const noexcept { return (raw_size_flags & k_is_int) != 0; }
  bool has_double() const noexcept { return (raw_size_flags & k_has_double) != 0; }

  void set_is_int(bool v) noexcept {
    if (v) raw_size_flags |= k_is_int;
    else raw_size_flags &= ~k_is_int;
  }

  void set_has_double(bool v) const noexcept {
    if (v) raw_size_flags |= k_has_double;
    else raw_size_flags &= ~k_has_double;
  }

  void set_raw_span(const char* p, std::uint32_t n) noexcept {
    raw_data = p;
    raw_size_flags = (raw_size_flags & ~k_size_mask) | (n & k_size_mask);
  }
};

struct sv_value;
struct sv_member;
struct sv_array_view;
struct sv_object_view;

struct sv_value {
  enum class kind : std::uint8_t { null, boolean, number, string, array, object };

  struct array_data {
    sv_value* data;
    std::uint32_t size;
    std::uint32_t cap;
  };

  struct object_data {
    sv_member* data;
    std::uint32_t size;
    std::uint32_t cap;
  };

  kind k{kind::null};
  union {
    bool b;
    sv_number_value num;
    sv_raw_view s;
    array_data a;
    object_data o;
  } u;

  sv_value() noexcept : k(kind::null) { u.a = {}; }
  sv_value(std::nullptr_t) noexcept : k(kind::null) { u.a = {}; }
  explicit sv_value(bool vb) : k(kind::boolean) { u.b = vb; }
  explicit sv_value(std::string_view vs) : k(kind::string) {
    u.s = {vs.data(), static_cast<std::uint32_t>(vs.size())};
  }

  static sv_value make_array(std::pmr::memory_resource*) {
    sv_value v;
    v.k = kind::array;
    v.u.a = {};
    return v;
  }

  static sv_value make_object(std::pmr::memory_resource*) {
    sv_value v;
    v.k = kind::object;
    v.u.o = {};
    return v;
  }

  static sv_value integer(std::int64_t vi) {
    sv_value v;
    v.k = kind::number;
    v.u.num = {};
    v.u.num.set_int_value(vi);
    v.u.num.raw_data = nullptr;
    v.u.num.raw_size_flags = 0;
    v.u.num.set_is_int(true);
    v.u.num.set_has_double(false);
    return v;
  }

  static sv_value number(double vd) {
    sv_value v;
    v.k = kind::number;
    v.u.num = {};
    v.u.num.set_double_value(vd);
    v.u.num.raw_data = nullptr;
    v.u.num.raw_size_flags = 0;
    v.u.num.set_is_int(false);
    v.u.num.set_has_double(true);
    return v;
  }

  static sv_value number_token(std::string_view token) {
    sv_value v;
    v.k = kind::number;
    v.u.num = {};
    v.u.num.raw_data = nullptr;
    v.u.num.raw_size_flags = 0;
    v.u.num.set_is_int(false);
    v.u.num.set_has_double(false);
    v.u.num.set_raw_span(token.data(), static_cast<std::uint32_t>(token.size()));
    v.u.num.payload = 0;
    return v;
  }

  static sv_value number_token_with_double(std::string_view token, double vd) {
    sv_value v;
    v.k = kind::number;
    v.u.num = {};
    v.u.num.raw_data = nullptr;
    v.u.num.raw_size_flags = 0;
    v.u.num.set_is_int(false);
    v.u.num.set_has_double(true);
    v.u.num.set_raw_span(token.data(), static_cast<std::uint32_t>(token.size()));
    v.u.num.set_double_value(vd);
    return v;
  }

  kind type() const noexcept { return k; }

  bool is_null() const noexcept { return k == kind::null; }
  bool is_bool() const noexcept { return k == kind::boolean; }
  bool is_number() const noexcept { return k == kind::number; }
  bool is_string() const noexcept { return k == kind::string; }
  bool is_array() const noexcept { return k == kind::array; }
  bool is_object() const noexcept { return k == kind::object; }

  bool as_bool() const {
    if (!is_bool()) throw std::runtime_error("chjson: not bool");
    return u.b;
  }

  bool is_int() const noexcept {
    if (!is_number()) return false;
    return u.num.is_int();
  }

  std::int64_t as_int() const {
    if (!is_number() || !u.num.is_int()) throw std::runtime_error("chjson: number is not int");
    return u.num.int_value();
  }

  double as_double() const {
    if (!is_number()) throw std::runtime_error("chjson: not number");
    const auto& n = u.num;
    const std::uint32_t flags = n.raw_size_flags;
    if ((flags & sv_number_value::k_has_double) != 0) return n.double_value();
    if ((flags & sv_number_value::k_is_int) != 0) return static_cast<double>(n.int_value());
    const std::uint32_t raw_n = flags & sv_number_value::k_size_mask;
    if (n.raw_data != nullptr && raw_n != 0) {
      n.set_double_value(detail::parse_double(std::string_view(n.raw_data, raw_n)));
      n.set_has_double(true);
      return n.double_value();
    }
    return n.double_value();
  }

  // Fast path: assumes this value is a number.
  // Undefined behavior if called when !is_number().
  double as_double_unchecked() const noexcept {
    const auto& n = u.num;
    const std::uint32_t flags = n.raw_size_flags;
    if ((flags & sv_number_value::k_has_double) != 0) return n.double_value();
    if ((flags & sv_number_value::k_is_int) != 0) return static_cast<double>(n.int_value());
    const std::uint32_t raw_n = flags & sv_number_value::k_size_mask;
    if (n.raw_data != nullptr && raw_n != 0) {
      n.set_double_value(detail::parse_double(std::string_view(n.raw_data, raw_n)));
      n.set_has_double(true);
      return n.double_value();
    }
    return n.double_value();
  }

  std::string_view as_string_view() const {
    if (!is_string()) throw std::runtime_error("chjson: not string");
    return std::string_view(u.s.data, u.s.size);
  }

  sv_array_view as_array() const;
  sv_object_view as_object() const;

  void array_reserve(std::pmr::memory_resource* mr, std::uint32_t new_cap) {
    if (!is_array()) throw std::runtime_error("chjson: not array");
    if (new_cap <= u.a.cap) return;
    sv_value* nd = static_cast<sv_value*>(mr->allocate(sizeof(sv_value) * new_cap, alignof(sv_value)));
    static_assert(std::is_trivially_copyable_v<sv_value>, "sv_value must be trivially copyable for memcpy relocation");
    if (u.a.data != nullptr && u.a.size != 0) {
      std::memcpy(nd, u.a.data, sizeof(sv_value) * u.a.size);
    }
    u.a.data = nd;
    u.a.cap = new_cap;
  }

  void array_push_back(std::pmr::memory_resource* mr, sv_value&& v) {
    if (!is_array()) throw std::runtime_error("chjson: not array");
    if (u.a.size == u.a.cap) {
      const std::uint32_t next = u.a.cap ? (u.a.cap * 2u) : 8u;
      array_reserve(mr, next);
    }
    new (&u.a.data[u.a.size]) sv_value(std::move(v));
    ++u.a.size;
  }

  void object_reserve(std::pmr::memory_resource* mr, std::uint32_t new_cap);
  void object_emplace_back(std::pmr::memory_resource* mr, std::string_view key, sv_value&& v);

  const sv_value* find(std::string_view key) const noexcept;
  sv_value* find(std::string_view key) noexcept;

  friend class document;
  friend class view_document;
  friend void dump_to(std::string&, const sv_value&, bool, int);
};

struct sv_member {
  std::string_view first{};
  sv_value second{nullptr};
};

struct sv_array_view {
  const sv_value* data{nullptr};
  std::size_t n{0};

  bool empty() const noexcept { return n == 0; }
  std::size_t size() const noexcept { return n; }
  const sv_value& operator[](std::size_t idx) const { return data[idx]; }

  const sv_value* begin() const noexcept { return data; }
  const sv_value* end() const noexcept { return data + n; }
};

struct sv_object_kv {
  std::string_view first{};
  const sv_value& second;
};

struct sv_object_view {
  const sv_member* data{nullptr};
  std::size_t n{0};

  bool empty() const noexcept { return n == 0; }
  std::size_t size() const noexcept { return n; }

  sv_object_kv operator[](std::size_t idx) const { return {data[idx].first, data[idx].second}; }

  struct iterator {
    const sv_member* p{nullptr};
    sv_object_kv operator*() const { return {p->first, p->second}; }
    iterator& operator++() {
      ++p;
      return *this;
    }
    bool operator!=(const iterator& o) const { return p != o.p; }
  };

  iterator begin() const { return iterator{data}; }
  iterator end() const { return iterator{data + n}; }
};

inline sv_array_view sv_value::as_array() const {
  if (!is_array()) throw std::runtime_error("chjson: not array");
  return sv_array_view{u.a.data, u.a.size};
}

inline sv_object_view sv_value::as_object() const {
  if (!is_object()) throw std::runtime_error("chjson: not object");
  return sv_object_view{u.o.data, u.o.size};
}

inline void sv_value::object_reserve(std::pmr::memory_resource* mr, std::uint32_t new_cap) {
  if (!is_object()) throw std::runtime_error("chjson: not object");
  if (new_cap <= u.o.cap) return;
  sv_member* nm = static_cast<sv_member*>(mr->allocate(sizeof(sv_member) * new_cap, alignof(sv_member)));
  static_assert(std::is_trivially_copyable_v<sv_member>, "sv_member must be trivially copyable for memcpy relocation");
  if (u.o.data != nullptr && u.o.size != 0) {
    std::memcpy(nm, u.o.data, sizeof(sv_member) * u.o.size);
  }
  u.o.data = nm;
  u.o.cap = new_cap;
}

inline void sv_value::object_emplace_back(std::pmr::memory_resource* mr, std::string_view key, sv_value&& v) {
  if (!is_object()) throw std::runtime_error("chjson: not object");
  if (u.o.size == u.o.cap) {
    const std::uint32_t next = u.o.cap ? (u.o.cap * 2u) : 8u;
    object_reserve(mr, next);
  }
  new (&u.o.data[u.o.size]) sv_member{key, std::move(v)};
  ++u.o.size;
}

inline const sv_value* sv_value::find(std::string_view key) const noexcept {
  if (!is_object()) return nullptr;
  const sv_member* m = u.o.data;
  const std::size_t n = u.o.size;
  for (std::size_t idx = 0; idx < n; ++idx) {
    if (m[idx].first == key) return &m[idx].second;
  }
  return nullptr;
}

inline sv_value* sv_value::find(std::string_view key) noexcept {
  if (!is_object()) return nullptr;
  sv_member* m = u.o.data;
  const std::size_t n = u.o.size;
  for (std::size_t idx = 0; idx < n; ++idx) {
    if (m[idx].first == key) return &m[idx].second;
  }
  return nullptr;
}

namespace detail {
#if CHJSON_USE_TLS_PARSE_CACHE
inline std::string& document_buffer_stash() {
  thread_local std::string stash;
  return stash;
}

inline pmr::arena_resource& document_arena_stash() {
  thread_local pmr::arena_resource stash;
  return stash;
}

struct mt_parse_tls_cache {
  detail::chjson_byte_ptr backing;
  std::size_t backing_size{0};

  std::pmr::monotonic_buffer_resource* resources{nullptr};
  unsigned resources_count{0};

  ~mt_parse_tls_cache() noexcept {
    if (resources != nullptr) {
      for (unsigned i = 0; i < resources_count; ++i) resources[i].~monotonic_buffer_resource();
      detail::chjson_deallocate(resources, sizeof(std::pmr::monotonic_buffer_resource) * static_cast<std::size_t>(resources_count),
                                alignof(std::pmr::monotonic_buffer_resource));
    }
  }
};

inline mt_parse_tls_cache& document_mt_parse_stash() {
  thread_local mt_parse_tls_cache stash;
  return stash;
}
#endif
} // namespace detail

struct document_parse_result;

class document {
public:
#if CHJSON_USE_TLS_PARSE_CACHE
  document() : mt_parse_resources_(nullptr, mt_parse_resources_deleter{}) {
    // Reuse the last freed buffer on this thread to reduce heap churn for
    // repeated single-shot parse() calls that return a fresh document.
    auto& stash = detail::document_buffer_stash();
    if (stash.capacity() != 0) {
      buffer_.swap(stash);
      stash.clear();
    }

    auto& arena_stash = detail::document_arena_stash();
    if (arena_stash.bytes_committed() != 0) {
      arena_ = std::move(arena_stash);
    }

    // Optional: reuse MT-parse backing/resources from this thread.
    // This reduces heap churn for workloads that repeatedly call parse() where
    // the MT path is taken.
    auto& mt_stash = detail::document_mt_parse_stash();
    if (mt_stash.backing_size != 0 && mt_stash.backing) {
      mt_parse_backing_.swap(mt_stash.backing);
      mt_parse_backing_size_ = mt_stash.backing_size;
      mt_stash.backing_size = 0;
    }

    if (mt_stash.resources != nullptr && mt_stash.resources_count != 0) {
      std::pmr::monotonic_buffer_resource* p = mt_stash.resources;
      const unsigned n = mt_stash.resources_count;
      mt_stash.resources = nullptr;
      mt_stash.resources_count = 0;
      mt_parse_resources_ = decltype(mt_parse_resources_)(p, mt_parse_resources_deleter{n});
    }
  }
#else
  document() = default;
#endif
  explicit document(std::string json) : buffer_(std::move(json)), mt_parse_resources_(nullptr, mt_parse_resources_deleter{}) {}

#if CHJSON_USE_TLS_PARSE_CACHE
  ~document() noexcept {
    // Keep at most one cached buffer per thread (bounded by capacity).
    constexpr std::size_t max_cached_capacity = static_cast<std::size_t>(CHJSON_TLS_PARSE_BUFFER_MAX);
    if (buffer_.capacity() <= max_cached_capacity) {
      auto& stash = detail::document_buffer_stash();
      if (stash.capacity() < buffer_.capacity()) {
        stash.swap(buffer_);
        stash.clear();
      }
    }

    // Similarly, cache arena blocks to avoid repeated heap churn for
    // parse() patterns that construct/destroy documents in a tight loop.
    // If this document used MT parse arenas, don't stash the arena: we'd also
    // need to cache the backing buffer/resources to make it worthwhile.
    if (!mt_parse_resources_) {
      constexpr std::size_t max_cached_arena = static_cast<std::size_t>(CHJSON_TLS_PARSE_ARENA_MAX);
      const std::size_t committed = arena_.bytes_committed();
      if (committed != 0 && committed <= max_cached_arena) {
        auto& arena_stash = detail::document_arena_stash();
        if (arena_stash.bytes_committed() < committed) {
          arena_stash = std::move(arena_);
        }
      }
    }

    // Cache MT parse backing/resources to reduce allocations for repeated MT parses.
    // Keep at most one set per thread.
    if (mt_parse_backing_ || mt_parse_resources_) {
      auto& mt_stash = detail::document_mt_parse_stash();

      if (mt_parse_backing_) {
        constexpr std::size_t max_cached_backing = static_cast<std::size_t>(CHJSON_TLS_PARSE_MT_BACKING_MAX);
        if (mt_parse_backing_size_ != 0 && mt_parse_backing_size_ <= max_cached_backing) {
          if (mt_stash.backing_size < mt_parse_backing_size_) {
            mt_stash.backing.swap(mt_parse_backing_);
            mt_stash.backing_size = mt_parse_backing_size_;
          }
        }
      }

      if (mt_parse_resources_) {
        const unsigned n = mt_parse_resources_.get_deleter().count;
        constexpr unsigned max_cached_n = static_cast<unsigned>(CHJSON_TLS_PARSE_MT_RESOURCES_MAX);
        if (n != 0 && n <= max_cached_n) {
          if (mt_stash.resources_count < n) {
            if (mt_stash.resources != nullptr) {
              for (unsigned i = 0; i < mt_stash.resources_count; ++i) mt_stash.resources[i].~monotonic_buffer_resource();
              detail::chjson_deallocate(mt_stash.resources,
                                        sizeof(std::pmr::monotonic_buffer_resource) * static_cast<std::size_t>(mt_stash.resources_count),
                                        alignof(std::pmr::monotonic_buffer_resource));
              mt_stash.resources = nullptr;
              mt_stash.resources_count = 0;
            }

            // Transfer ownership out of the document without deleting.
            mt_stash.resources = mt_parse_resources_.release();
            mt_stash.resources_count = n;
          }
        }
      }
    }
  }
#else
  ~document() = default;
#endif

  // Reuse this document for another parse. Keeps arena blocks unless you call reset().
  void clear() noexcept {
    root_ = sv_value(nullptr);
    arena_.clear();
    mt_parse_resources_.reset();
    mt_parse_backing_.reset();
    mt_parse_backing_size_ = 0;
  }

  // Replace the underlying buffer content (reusing capacity) and clear arena.
  void assign_buffer(std::string_view json) {
    buffer_.assign(json.data(), json.size());
    clear();
  }

  // Clear the backing buffer contents while keeping capacity.
  // Useful when parsing from a read-only source (no string_views into buffer_).
  void clear_buffer() noexcept { buffer_.clear(); }

  // Ensure buffer capacity ahead of time (useful for benchmarks / repeated parsing).
  void reserve_buffer(std::size_t n) { buffer_.reserve(n); }

  document(const document&) = delete;
  document& operator=(const document&) = delete;

  document(document&&) noexcept = default;
  document& operator=(document&&) noexcept = default;

  std::string_view buffer() const noexcept { return std::string_view(buffer_.data(), buffer_.size()); }
  const sv_value& root() const noexcept { return root_; }
  sv_value& root() noexcept { return root_; }

  chjson::pmr::arena_resource& arena() noexcept { return arena_; }
  const chjson::pmr::arena_resource& arena() const noexcept { return arena_; }

  std::pmr::memory_resource* resource() noexcept { return &arena_; }
  const std::pmr::memory_resource* resource() const noexcept { return &arena_; }

private:
  pmr::arena_resource arena_;
  std::string buffer_;
  sv_value root_;

  // Optional: multithreaded parse backing + per-thread resources.
  // When non-empty, parts of the DOM may be allocated out of these resources.
  struct mt_parse_resources_deleter {
    unsigned count{0};
    void operator()(std::pmr::monotonic_buffer_resource* p) const noexcept {
      if (!p) return;
      for (unsigned i = 0; i < count; ++i) {
        p[i].~monotonic_buffer_resource();
      }
      detail::chjson_deallocate(p, sizeof(std::pmr::monotonic_buffer_resource) * static_cast<std::size_t>(count),
                                alignof(std::pmr::monotonic_buffer_resource));
    }
  };

  std::unique_ptr<std::pmr::monotonic_buffer_resource, mt_parse_resources_deleter> mt_parse_resources_;
  detail::chjson_byte_ptr mt_parse_backing_{nullptr, detail::chjson_sized_byte_deleter{0}};
  std::size_t mt_parse_backing_size_{0};

  friend document_parse_result parse(std::string_view json, parse_options opt);

  friend struct insitu_parser;
};

struct document_parse_result {
  document doc;
  error err;
};

namespace detail {
inline bool parse_u4_insitu(const char* buf, std::size_t size, std::size_t& i, std::uint32_t& out_cp);
inline void write_utf8_insitu(char* out, std::size_t& w, std::uint32_t cp);
} // namespace detail

// -----------------------------
// View + arena DOM (non-mutating, zero-copy for unescaped strings)
// -----------------------------

class view_document {
public:
  view_document() = default;

  // Reuse this document for another parse. Keeps arena blocks unless you call reset().
  void clear() noexcept {
    root_ = sv_value(nullptr);
    arena_.clear();
    source_ = std::string_view{};
  }

  void reset() noexcept {
    root_ = sv_value(nullptr);
    arena_.reset();
    source_ = std::string_view{};
  }

  std::string_view source() const noexcept { return source_; }
  const sv_value& root() const noexcept { return root_; }
  sv_value& root() noexcept { return root_; }

  chjson::pmr::arena_resource& arena() noexcept { return arena_; }
  const chjson::pmr::arena_resource& arena() const noexcept { return arena_; }

  std::pmr::memory_resource* resource() noexcept { return &arena_; }
  const std::pmr::memory_resource* resource() const noexcept { return &arena_; }

private:
  pmr::arena_resource arena_;
  std::string_view source_;
  sv_value root_;

  friend struct view_parser;
};

struct view_document_parse_result {
  view_document doc;
  error err;
};

struct view_parser {
  view_document* doc{nullptr};
  std::string_view s;
  const char* buf{nullptr};
  std::size_t size{0};
  std::size_t i{0};
  parse_options opt;

  error run_inplace(view_document& d, std::string_view json) {
    doc = &d;
    doc->clear();
    doc->source_ = json;
    s = json;
    buf = json.data();
    size = json.size();
    i = 0;

    error err;
    detail::skip_ws(buf, size, i);
    doc->root_ = parse_value(0, err);
    if (err) return err;

    detail::skip_ws(buf, size, i);
    if (opt.require_eof && i != size) {
      set_error(err, error_code::trailing_characters);
      return err;
    }
    return err;
  }

  view_document_parse_result run(std::string_view json) {
    view_document_parse_result r;
    r.err = run_inplace(r.doc, json);
    return r;
  }

  void set_error(error& e, error_code code, std::size_t at = std::numeric_limits<std::size_t>::max()) {
    if (e) return;
    e.code = code;
    e.offset = (at == std::numeric_limits<std::size_t>::max()) ? i : at;
    detail::update_line_col(s, e.offset, e.line, e.column);
  }

  sv_value parse_value(std::size_t depth, error& e) {
    if (depth > opt.max_depth) {
      set_error(e, error_code::nesting_too_deep);
      return nullptr;
    }
    if (i >= size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }

    const char c = buf[i];
    switch (c) {
      case 'n': return parse_literal("null", 4, sv_value(nullptr), e);
      case 't': return parse_literal("true", 4, sv_value(true), e);
      case 'f': return parse_literal("false", 5, sv_value(false), e);
      case '"': {
        std::string_view out;
        if (!parse_string(out, e)) return nullptr;
        return sv_value(out);
      }
      case '[': return parse_array(depth + 1, e);
      case '{': return parse_object(depth + 1, e);
      default: {
        if (c == '-' || (c >= '0' && c <= '9')) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/false)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (num.is_int) return sv_value::integer(num.i);
          return sv_value::number_token(std::string_view(buf + start, i - start));
        }
        set_error(e, error_code::invalid_value);
        return nullptr;
      }
    }
  }

  sv_value parse_literal(const char* lit, std::size_t len, sv_value v, error& e) {
    if (i + len > size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }
    if (std::memcmp(buf + i, lit, len) != 0) {
      set_error(e, error_code::invalid_value);
      return nullptr;
    }
    i += len;
    return v;
  }

  bool parse_string(std::string_view& out, error& e) {
    if (i >= size || buf[i] != '"') {
      set_error(e, error_code::invalid_string);
      return false;
    }
    const std::size_t quote_pos = i;
    ++i;
    const std::size_t start = i;

    // Fast scan for unescaped strings.
#if defined(_M_X64) || defined(__SSE2__)
    {
      const __m128i q = _mm_set1_epi8('"');
      const __m128i bs = _mm_set1_epi8('\\');
      const __m128i k1f = _mm_set1_epi8(0x1F);
      const __m128i zero = _mm_setzero_si128();
      while (i + 16 <= size) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + i));
        const __m128i is_q = _mm_cmpeq_epi8(v, q);
        const __m128i is_bs = _mm_cmpeq_epi8(v, bs);
        const __m128i sub = _mm_subs_epu8(v, k1f);
        const __m128i is_ctrl = _mm_cmpeq_epi8(sub, zero);
        const __m128i any = _mm_or_si128(_mm_or_si128(is_q, is_bs), is_ctrl);
        const int mask = _mm_movemask_epi8(any);
        if (mask != 0) {
#if defined(_MSC_VER)
          unsigned long bit = 0;
          _BitScanForward(&bit, static_cast<unsigned long>(mask));
          i += static_cast<std::size_t>(bit);
#else
          i += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#endif
          const unsigned char uc = static_cast<unsigned char>(buf[i]);
          const char c = buf[i];
          if (c == '"') {
            out = std::string_view(buf + start, i - start);
            ++i;
            return true;
          }
          if (c == '\\') break;
          if (uc <= 0x1F) {
            set_error(e, error_code::invalid_string, i);
            return false;
          }
        }
        i += 16;
      }
    }
#endif
    while (i < size) {
      const unsigned char uc = static_cast<unsigned char>(buf[i]);
      const char c = buf[i];
      if (c == '"') {
        out = std::string_view(buf + start, i - start);
        ++i;
        return true;
      }
      if (c == '\\') break;
      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, i);
        return false;
      }
      ++i;
    }

    // Escaped: two-pass.
    const std::size_t first_escape = i;
    std::size_t end_quote = std::numeric_limits<std::size_t>::max();
    {
      std::size_t scan = first_escape;
      while (scan < size) {
        const unsigned char uc = static_cast<unsigned char>(buf[scan]);
        const char c = buf[scan++];
        if (c == '"') {
          end_quote = scan - 1;
          break;
        }
        if (c == '\\') {
          if (scan >= size) {
            set_error(e, error_code::unexpected_eof, quote_pos);
            return false;
          }
          const char esc = buf[scan++];
          if (esc == 'u') {
            if (scan + 4 > size) {
              set_error(e, error_code::unexpected_eof, quote_pos);
              return false;
            }
            scan += 4;
          }
          continue;
        }
        if (uc <= 0x1F) {
          set_error(e, error_code::invalid_string, scan - 1);
          return false;
        }
      }
    }

    if (end_quote == std::numeric_limits<std::size_t>::max()) {
      set_error(e, error_code::unexpected_eof, quote_pos);
      return false;
    }

    const std::size_t raw_len = end_quote - start;
    auto* mr = doc->resource();
    char* dst = (raw_len != 0) ? static_cast<char*>(mr->allocate(raw_len, alignof(char))) : nullptr;
    std::size_t rpos = start;
    std::size_t wpos = 0;
    while (rpos < end_quote) {
      const unsigned char uc = static_cast<unsigned char>(buf[rpos]);
      const char c = buf[rpos++];
      if (c == '\\') {
        if (rpos >= end_quote) {
          set_error(e, error_code::unexpected_eof, quote_pos);
          return false;
        }
        const char esc = buf[rpos++];
        switch (esc) {
          case '"': dst[wpos++] = '"'; break;
          case '\\': dst[wpos++] = '\\'; break;
          case '/': dst[wpos++] = '/'; break;
          case 'b': dst[wpos++] = '\b'; break;
          case 'f': dst[wpos++] = '\f'; break;
          case 'n': dst[wpos++] = '\n'; break;
          case 'r': dst[wpos++] = '\r'; break;
          case 't': dst[wpos++] = '\t'; break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!detail::parse_u4_insitu(buf, size, rpos, cp)) {
              set_error(e, error_code::invalid_unicode_escape, rpos);
              return false;
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
              if (rpos + 2 > size || buf[rpos] != '\\' || buf[rpos + 1] != 'u') {
                set_error(e, error_code::invalid_utf16_surrogate, rpos);
                return false;
              }
              rpos += 2;
              std::uint32_t low = 0;
              if (!detail::parse_u4_insitu(buf, size, rpos, low)) {
                set_error(e, error_code::invalid_unicode_escape, rpos);
                return false;
              }
              if (low < 0xDC00u || low > 0xDFFFu) {
                set_error(e, error_code::invalid_utf16_surrogate, rpos);
                return false;
              }
              const std::uint32_t hi = cp - 0xD800u;
              const std::uint32_t lo = low - 0xDC00u;
              cp = 0x10000u + ((hi << 10) | lo);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
              set_error(e, error_code::invalid_utf16_surrogate, rpos);
              return false;
            }
            detail::write_utf8_insitu(dst, wpos, cp);
            break;
          }
          default:
            set_error(e, error_code::invalid_escape, rpos - 1);
            return false;
        }
        continue;
      }
      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, rpos - 1);
        return false;
      }
      dst[wpos++] = c;
    }

    out = std::string_view(dst, wpos);
    i = end_quote + 1;
    return true;
  }

  sv_value parse_array(std::size_t depth, error& e) {
    ++i; // '['
    detail::skip_ws(buf, size, i);
    sv_value out;
    out.k = sv_value::kind::array;
    out.u.a = {};

    auto* mr = doc->resource();
    auto reserve = [&](std::uint32_t want) {
      if (want <= out.u.a.cap) return;
      std::uint32_t new_cap = 0;
      if (out.u.a.cap == 0) {
        // First reserve: allocate exactly to avoid overshoot on huge arrays.
        new_cap = want ? want : 1u;
      } else {
        new_cap = out.u.a.cap;
        while (new_cap < want) new_cap = (new_cap < 1024u) ? (new_cap * 2u) : (new_cap + new_cap / 2u);
      }
      sv_value* new_data = static_cast<sv_value*>(mr->allocate(sizeof(sv_value) * new_cap, alignof(sv_value)));
      if (out.u.a.data && out.u.a.size) {
        std::memcpy(new_data, out.u.a.data, sizeof(sv_value) * out.u.a.size);
      }
      out.u.a.data = new_data;
      out.u.a.cap = new_cap;
    };

    auto is_num_start = [](char c) noexcept {
      return c == '-' || (c >= '0' && c <= '9');
    };

    // For top-level arrays that start with a number (e.g. the numbers payload),
    // reserve using remaining/20 without the old 2M cap to avoid realloc+memcpy
    // and monotonic arena waste. For other arrays, keep a conservative cap.
    if (depth <= 1) {
      const std::size_t remaining = (i < size) ? (size - i) : 0;
      const std::size_t guess = remaining / 20u;
      if (guess >= 16) {
        constexpr std::size_t kCap = 2u * 1024u * 1024u;
        const bool starts_number = (i < size) && is_num_start(buf[i]);
        const std::size_t want_sz = starts_number ? guess : std::min<std::size_t>(guess, kCap);
        const std::uint32_t want = (want_sz > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                                     ? std::numeric_limits<std::uint32_t>::max()
                                     : static_cast<std::uint32_t>(want_sz);
        reserve(want);
      }
    } else {
      reserve(16);
    }

    if (i < size && buf[i] == ']') {
      ++i;
      return out;
    }

    // Fast path for number-only arrays (e.g. the numbers payload):
    // parse floats eagerly to avoid lazy conversion during sum.
    {
      detail::skip_ws(buf, size, i);
      if (i < size && is_num_start(buf[i])) {
        while (true) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/true)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (out.u.a.size == out.u.a.cap) reserve(out.u.a.size + 1u);
          if (num.is_int) {
            out.u.a.data[out.u.a.size++] = sv_value::integer(num.i);
          } else {
            out.u.a.data[out.u.a.size++] = sv_value::number_token_with_double(std::string_view(buf + start, i - start), num.d);
          }

          if (i >= size) {
            set_error(e, error_code::unexpected_eof);
            return nullptr;
          }

          // Common case: no whitespace between token and delimiter.
          char c = buf[i];
          if (static_cast<unsigned char>(c) <= static_cast<unsigned char>(' ')) {
            detail::skip_ws(buf, size, i);
            if (i >= size) {
              set_error(e, error_code::unexpected_eof);
              return nullptr;
            }
            c = buf[i];
          }

          if (c == ',') {
            ++i;
            if (i < size && is_num_start(buf[i])) continue;
            detail::skip_ws(buf, size, i);
            if (i < size && is_num_start(buf[i])) continue;
            break;
          }
          if (c == ']') {
            ++i;
            return out;
          }
          set_error(e, error_code::expected_comma_or_end, i);
          return nullptr;
        }
      }
    }

    while (true) {
      sv_value elem = parse_value(depth, e);
      if (e) return nullptr;
      if (out.u.a.size == out.u.a.cap) reserve(out.u.a.size + 1u);
      out.u.a.data[out.u.a.size++] = elem;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') {
        detail::skip_ws(buf, size, i);
        continue;
      }
      if (c == ']') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }

  sv_value parse_object(std::size_t depth, error& e) {
    ++i; // '{'
    detail::skip_ws(buf, size, i);
    sv_value out;
    out.k = sv_value::kind::object;
    out.u.o = {};

    auto* mr = doc->resource();
    auto reserve = [&](std::uint32_t want) {
      if (want <= out.u.o.cap) return;
      std::uint32_t new_cap = out.u.o.cap ? out.u.o.cap : 1u;
      while (new_cap < want) new_cap = (new_cap < 1024u) ? (new_cap * 2u) : (new_cap + new_cap / 2u);
      sv_member* new_data = static_cast<sv_member*>(mr->allocate(sizeof(sv_member) * new_cap, alignof(sv_member)));
      if (out.u.o.data && out.u.o.size) {
        std::memcpy(new_data, out.u.o.data, sizeof(sv_member) * out.u.o.size);
      }
      out.u.o.data = new_data;
      out.u.o.cap = new_cap;
    };

    // Many JSON objects are small and fixed-shape; reserving a few slots helps.
    (void)depth;
    reserve(8);

    if (i < size && buf[i] == '}') {
      ++i;
      return out;
    }

    while (true) {
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (buf[i] != '"') {
        set_error(e, error_code::expected_key_string);
        return nullptr;
      }

      std::string_view key;
      if (!parse_string(key, e)) return nullptr;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (buf[i] != ':') {
        set_error(e, error_code::expected_colon);
        return nullptr;
      }
      ++i;

      detail::skip_ws(buf, size, i);
      sv_value v = parse_value(depth, e);
      if (e) return nullptr;
      if (out.u.o.size == out.u.o.cap) reserve(out.u.o.size + 1u);
      new (&out.u.o.data[out.u.o.size]) sv_member{key, std::move(v)};
      ++out.u.o.size;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') {
        detail::skip_ws(buf, size, i);
        continue;
      }
      if (c == '}') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }
};

// -----------------------------
// Owning DOM parse from read-only input (document + arena)
// -----------------------------

// Parses from a read-only buffer but produces an owning `document`.
// Unlike the in-situ parser, this does not copy the entire JSON text.
// Instead, it copies only strings/keys/number tokens that must outlive the input.
struct owning_view_parser {
  document* doc{nullptr};
  std::string_view s;
  const char* buf{nullptr};
  std::size_t size{0};
  std::size_t i{0};
  parse_options opt;

  error run_inplace(document& d, std::string_view json) {
    doc = &d;
    doc->clear();
    s = json;
    buf = json.data();
    size = json.size();
    i = 0;

    error err;
    detail::skip_ws(buf, size, i);
    doc->root() = parse_value(0, err);
    if (err) return err;

    detail::skip_ws(buf, size, i);
    if (opt.require_eof && i != size) {
      set_error(err, error_code::trailing_characters);
      return err;
    }
    return err;
  }

  document_parse_result run(std::string_view json) {
    document_parse_result r;
    r.err = run_inplace(r.doc, json);
    return r;
  }

  void set_error(error& e, error_code code, std::size_t at = std::numeric_limits<std::size_t>::max()) {
    if (e) return;
    e.code = code;
    e.offset = (at == std::numeric_limits<std::size_t>::max()) ? i : at;
    detail::update_line_col(s, e.offset, e.line, e.column);
  }

  std::string_view copy_span(const char* p, std::size_t n) {
    if (n == 0) return std::string_view{};
    auto* mr = doc->resource();
    char* dst = static_cast<char*>(mr->allocate(n, alignof(char)));
    std::memcpy(dst, p, n);
    return std::string_view(dst, n);
  }

  sv_value parse_value(std::size_t depth, error& e) {
    if (depth > opt.max_depth) {
      set_error(e, error_code::nesting_too_deep);
      return nullptr;
    }
    if (i >= size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }

    const char c = buf[i];
    switch (c) {
      case 'n': return parse_literal("null", 4, sv_value(nullptr), e);
      case 't': return parse_literal("true", 4, sv_value(true), e);
      case 'f': return parse_literal("false", 5, sv_value(false), e);
      case '"': {
        std::string_view out;
        if (!parse_string(out, e)) return nullptr;
        return sv_value(out);
      }
      case '[': return parse_array(depth + 1, e);
      case '{': return parse_object(depth + 1, e);
      default: {
        if (c == '-' || (c >= '0' && c <= '9')) {
          detail::number_value num;
          const std::size_t start = i;
          // Performance: avoid eager float parsing; store the raw token and parse lazily.
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/false)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (num.is_int) return sv_value::integer(num.i);
          const std::string_view tok = copy_span(buf + start, i - start);
          return sv_value::number_token(tok);
        }
        set_error(e, error_code::invalid_value);
        return nullptr;
      }
    }
  }

  sv_value parse_literal(const char* lit, std::size_t len, sv_value v, error& e) {
    if (i + len > size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }
    if (std::memcmp(buf + i, lit, len) != 0) {
      set_error(e, error_code::invalid_value);
      return nullptr;
    }
    i += len;
    return v;
  }

  bool parse_string(std::string_view& out, error& e) {
    if (i >= size || buf[i] != '"') {
      set_error(e, error_code::invalid_string);
      return false;
    }
    const std::size_t quote_pos = i;
    ++i;
    const std::size_t start = i;

    // Fast scan for unescaped strings.
#if defined(_M_X64) || defined(__SSE2__)
    {
      const __m128i q = _mm_set1_epi8('"');
      const __m128i bs = _mm_set1_epi8('\\');
      const __m128i k1f = _mm_set1_epi8(0x1F);
      const __m128i zero = _mm_setzero_si128();
      while (i + 16 <= size) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + i));
        const __m128i is_q = _mm_cmpeq_epi8(v, q);
        const __m128i is_bs = _mm_cmpeq_epi8(v, bs);
        const __m128i sub = _mm_subs_epu8(v, k1f);
        const __m128i is_ctrl = _mm_cmpeq_epi8(sub, zero);
        const __m128i any = _mm_or_si128(_mm_or_si128(is_q, is_bs), is_ctrl);
        const int mask = _mm_movemask_epi8(any);
        if (mask != 0) {
#if defined(_MSC_VER)
          unsigned long bit = 0;
          _BitScanForward(&bit, static_cast<unsigned long>(mask));
          i += static_cast<std::size_t>(bit);
#else
          i += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#endif
          const unsigned char uc = static_cast<unsigned char>(buf[i]);
          const char c = buf[i];
          if (c == '"') {
            out = copy_span(buf + start, i - start);
            ++i;
            return true;
          }
          if (c == '\\') break;
          if (uc <= 0x1F) {
            set_error(e, error_code::invalid_string, i);
            return false;
          }
        }
        i += 16;
      }
    }
#endif
    while (i < size) {
      const unsigned char uc = static_cast<unsigned char>(buf[i]);
      const char c = buf[i];
      if (c == '"') {
        out = copy_span(buf + start, i - start);
        ++i;
        return true;
      }
      if (c == '\\') break;
      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, i);
        return false;
      }
      ++i;
    }

    // Escaped: two-pass decode into the arena.
    const std::size_t first_escape = i;
    std::size_t end_quote = std::numeric_limits<std::size_t>::max();
    {
      std::size_t scan = first_escape;
      while (scan < size) {
        const unsigned char uc = static_cast<unsigned char>(buf[scan]);
        const char c = buf[scan++];
        if (c == '"') {
          end_quote = scan - 1;
          break;
        }
        if (c == '\\') {
          if (scan >= size) {
            set_error(e, error_code::unexpected_eof, quote_pos);
            return false;
          }
          const char esc = buf[scan++];
          if (esc == 'u') {
            if (scan + 4 > size) {
              set_error(e, error_code::unexpected_eof, quote_pos);
              return false;
            }
            scan += 4;
          }
          continue;
        }
        if (uc <= 0x1F) {
          set_error(e, error_code::invalid_string, scan - 1);
          return false;
        }
      }
    }

    if (end_quote == std::numeric_limits<std::size_t>::max()) {
      set_error(e, error_code::unexpected_eof, quote_pos);
      return false;
    }

    const std::size_t raw_len = end_quote - start;
    auto* mr = doc->resource();
    char* dst = (raw_len != 0) ? static_cast<char*>(mr->allocate(raw_len, alignof(char))) : nullptr;
    std::size_t rpos = start;
    std::size_t wpos = 0;
    while (rpos < end_quote) {
      const unsigned char uc = static_cast<unsigned char>(buf[rpos]);
      const char c = buf[rpos++];
      if (c == '\\') {
        if (rpos >= end_quote) {
          set_error(e, error_code::unexpected_eof, quote_pos);
          return false;
        }
        const char esc = buf[rpos++];
        switch (esc) {
          case '"': dst[wpos++] = '"'; break;
          case '\\': dst[wpos++] = '\\'; break;
          case '/': dst[wpos++] = '/'; break;
          case 'b': dst[wpos++] = '\b'; break;
          case 'f': dst[wpos++] = '\f'; break;
          case 'n': dst[wpos++] = '\n'; break;
          case 'r': dst[wpos++] = '\r'; break;
          case 't': dst[wpos++] = '\t'; break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!detail::parse_u4_insitu(buf, size, rpos, cp)) {
              set_error(e, error_code::invalid_unicode_escape, rpos);
              return false;
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
              if (rpos + 2 > size || buf[rpos] != '\\' || buf[rpos + 1] != 'u') {
                set_error(e, error_code::invalid_utf16_surrogate, rpos);
                return false;
              }
              rpos += 2;
              std::uint32_t low = 0;
              if (!detail::parse_u4_insitu(buf, size, rpos, low)) {
                set_error(e, error_code::invalid_unicode_escape, rpos);
                return false;
              }
              if (low < 0xDC00u || low > 0xDFFFu) {
                set_error(e, error_code::invalid_utf16_surrogate, rpos);
                return false;
              }
              const std::uint32_t hi = cp - 0xD800u;
              const std::uint32_t lo = low - 0xDC00u;
              cp = 0x10000u + ((hi << 10) | lo);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
              set_error(e, error_code::invalid_utf16_surrogate, rpos);
              return false;
            }
            detail::write_utf8_insitu(dst, wpos, cp);
            break;
          }
          default:
            set_error(e, error_code::invalid_escape, rpos - 1);
            return false;
        }
        continue;
      }
      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, rpos - 1);
        return false;
      }
      dst[wpos++] = c;
    }

    out = std::string_view(dst, wpos);
    i = end_quote + 1;
    return true;
  }

  sv_value parse_array(std::size_t depth, error& e) {
    ++i; // '['
    detail::skip_ws(buf, size, i);
    auto* mr = doc->resource();
    sv_value out = sv_value::make_array(mr);

    auto is_num_start = [](char c) noexcept {
      return c == '-' || (c >= '0' && c <= '9');
    };

    if (depth <= 1) {
      const std::size_t remaining = (i < size) ? (size - i) : 0;
      const std::size_t guess = remaining / 20u;
      if (guess > 0) {
        constexpr std::size_t kCap = 2u * 1024u * 1024u;
        const bool starts_number = (i < size) && is_num_start(buf[i]);
        const std::size_t want_sz = starts_number ? guess : std::min<std::size_t>(guess, kCap);
        const std::uint32_t want = (want_sz > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                                     ? std::numeric_limits<std::uint32_t>::max()
                                     : static_cast<std::uint32_t>(want_sz);
        out.array_reserve(mr, want);
      }
    } else {
      out.array_reserve(mr, 16u);
    }

    if (i < size && buf[i] == ']') {
      ++i;
      return out;
    }

    // Fast path for number-only arrays (e.g. the numbers payload):
    // eagerly parse floats (no raw token copying) so Parse+sum doesn't pay lazy conversion.
    {
      detail::skip_ws(buf, size, i);
      if (i < size && is_num_start(buf[i])) {
        while (true) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/true)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (num.is_int) {
            out.array_push_back(mr, sv_value::integer(num.i));
          } else {
            out.array_push_back(mr, sv_value::number(num.d));
          }

          if (i >= size) {
            set_error(e, error_code::unexpected_eof);
            return nullptr;
          }

          char c = buf[i];
          if (static_cast<unsigned char>(c) <= static_cast<unsigned char>(' ')) {
            detail::skip_ws(buf, size, i);
            if (i >= size) {
              set_error(e, error_code::unexpected_eof);
              return nullptr;
            }
            c = buf[i];
          }

          if (c == ',') {
            ++i;
            if (i < size && is_num_start(buf[i])) continue;
            detail::skip_ws(buf, size, i);
            if (i < size && is_num_start(buf[i])) continue;
            break;
          }
          if (c == ']') {
            ++i;
            return out;
          }
          set_error(e, error_code::expected_comma_or_end, i);
          return nullptr;
        }
      }
    }

    while (true) {
      detail::skip_ws(buf, size, i);
      sv_value elem = parse_value(depth, e);
      if (e) return nullptr;
      out.array_push_back(mr, std::move(elem));

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') continue;
      if (c == ']') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }

  sv_value parse_object(std::size_t depth, error& e) {
    ++i; // '{'
    detail::skip_ws(buf, size, i);
    auto* mr = doc->resource();
    sv_value out = sv_value::make_object(mr);
    (void)depth;
    out.object_reserve(mr, 8u);

    if (i < size && buf[i] == '}') {
      ++i;
      return out;
    }

    while (true) {
      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (buf[i] != '"') {
        set_error(e, error_code::expected_key_string);
        return nullptr;
      }

      std::string_view key;
      if (!parse_string(key, e)) return nullptr;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (buf[i] != ':') {
        set_error(e, error_code::expected_colon);
        return nullptr;
      }
      ++i;

      detail::skip_ws(buf, size, i);
      sv_value v = parse_value(depth, e);
      if (e) return nullptr;
      out.object_emplace_back(mr, key, std::move(v));

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') continue;
      if (c == '}') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }
};

// -----------------------------
// DOM parse fast path for inputs with no strings (no '"')
// -----------------------------
// This parser reads from a read-only buffer and eagerly parses numbers into
// doubles/ints. It is only used when the input contains no strings, so the
// resulting DOM does not require a backing buffer for string_view lifetimes.
struct no_string_parser {
  document* doc{nullptr};
  std::string_view s;
  const char* buf{nullptr};
  std::size_t size{0};
  std::size_t i{0};
  parse_options opt;

  error run_inplace(document& d, std::string_view json) {
    doc = &d;
    doc->clear();
    doc->clear_buffer();
    s = json;
    buf = json.data();
    size = json.size();
    i = 0;

    error err;
    detail::skip_ws(buf, size, i);
    doc->root() = parse_value(0, err);
    if (err) return err;

    detail::skip_ws(buf, size, i);
    if (opt.require_eof && i != size) {
      set_error(err, error_code::trailing_characters);
      return err;
    }
    return err;
  }

  void set_error(error& e, error_code code, std::size_t at = std::numeric_limits<std::size_t>::max()) {
    if (e) return;
    e.code = code;
    e.offset = (at == std::numeric_limits<std::size_t>::max()) ? i : at;
    detail::update_line_col(s, e.offset, e.line, e.column);
  }

  sv_value parse_value(std::size_t depth, error& e) {
    if (depth > opt.max_depth) {
      set_error(e, error_code::nesting_too_deep);
      return nullptr;
    }
    if (i >= size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }

    const char c = buf[i];
    switch (c) {
      case 'n': return parse_literal("null", 4, sv_value(nullptr), e);
      case 't': return parse_literal("true", 4, sv_value(true), e);
      case 'f': return parse_literal("false", 5, sv_value(false), e);
      case '{': return parse_object(depth + 1, e);
      case '[': return parse_array(depth + 1, e);
      default: {
        if (c == '-' || (c >= '0' && c <= '9')) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/true)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (num.is_int) return sv_value::integer(num.i);
          // This fast path parses from a read-only input buffer that is NOT stored in the document.
          // Do not retain raw token pointers into that buffer.
          return sv_value::number(num.d);
        }
        set_error(e, error_code::invalid_value);
        return nullptr;
      }
    }
  }

  sv_value parse_literal(const char* lit, std::size_t len, sv_value v, error& e) {
    if (i + len > size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }
    if (std::memcmp(buf + i, lit, len) != 0) {
      set_error(e, error_code::invalid_value);
      return nullptr;
    }
    i += len;
    return v;
  }

  sv_value parse_array(std::size_t depth, error& e) {
    ++i; // '['
    detail::skip_ws(buf, size, i);
    sv_value out;
    out.k = sv_value::kind::array;
    out.u.a = {};

    auto* mr = doc->resource();
    auto reserve = [&](std::uint32_t want) {
      if (want <= out.u.a.cap) return;
      std::uint32_t new_cap = 0;
      if (out.u.a.cap == 0) {
        // First reserve: allocate exactly to avoid overshoot on huge arrays.
        new_cap = want ? want : 1u;
      } else {
        new_cap = out.u.a.cap;
        while (new_cap < want) new_cap = (new_cap < 1024u) ? (new_cap * 2u) : (new_cap + new_cap / 2u);
      }
      sv_value* new_data = static_cast<sv_value*>(mr->allocate(sizeof(sv_value) * new_cap, alignof(sv_value)));
      if (out.u.a.data && out.u.a.size) {
        std::memcpy(new_data, out.u.a.data, sizeof(sv_value) * out.u.a.size);
      }
      out.u.a.data = new_data;
      out.u.a.cap = new_cap;
    };

    // For no-string inputs, large arrays are typically number-heavy; use a
    // tighter per-element estimate than the generic /64 heuristic.
    if (depth <= 1) {
      const std::size_t remaining = (i < size) ? (size - i) : 0;
      const std::size_t guess = remaining / 20u;
      if (guess >= 16) reserve(static_cast<std::uint32_t>(std::min<std::size_t>(guess, 2u * 1024u * 1024u)));
    } else {
      reserve(16);
    }

    if (i < size && buf[i] == ']') {
      ++i;
      return out;
    }

    auto is_num_start = [](char c) noexcept {
      return c == '-' || (c >= '0' && c <= '9');
    };

    // Fast path for number-heavy arrays (e.g. the numbers benchmark payload):
    // avoid calling parse_value() per element when the array contains only numbers.
    // If we encounter a non-number element, fall back to the generic loop.
    {
      detail::skip_ws(buf, size, i);
      if (i < size && is_num_start(buf[i])) {
        while (true) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/true)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (out.u.a.size == out.u.a.cap) reserve(out.u.a.size + 1u);
          sv_value* dst = &out.u.a.data[out.u.a.size++];
          dst->k = sv_value::kind::number;
          dst->u.num.raw_data = nullptr;
          if (num.is_int) {
            dst->u.num.raw_size_flags = sv_number_value::k_is_int;
            dst->u.num.set_int_value(num.i);
          } else {
            dst->u.num.raw_size_flags = sv_number_value::k_has_double;
            dst->u.num.set_double_value(num.d);
          }

          if (i >= size) {
            set_error(e, error_code::unexpected_eof);
            return nullptr;
          }

          // Common case for number payloads: no whitespace between token and delimiter.
          char c = buf[i];
          if (static_cast<unsigned char>(c) <= static_cast<unsigned char>(' ')) {
            detail::skip_ws(buf, size, i);
            if (i >= size) {
              set_error(e, error_code::unexpected_eof);
              return nullptr;
            }
            c = buf[i];
          }

          if (c == ',') {
            ++i;
            // Common case: next token starts immediately.
            if (i < size && is_num_start(buf[i])) continue;
            detail::skip_ws(buf, size, i);
            if (i < size && is_num_start(buf[i])) continue;
            // Non-number element follows; fall back to the generic path for the rest.
            break;
          }
          if (c == ']') {
            ++i;
            return out;
          }
          set_error(e, error_code::expected_comma_or_end, i);
          return nullptr;
        }
      }
    }

    while (true) {
      detail::skip_ws(buf, size, i);
      sv_value elem = parse_value(depth, e);
      if (e) return nullptr;
      if (out.u.a.size == out.u.a.cap) reserve(out.u.a.size + 1u);
      out.u.a.data[out.u.a.size++] = elem;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') continue;
      if (c == ']') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }

  sv_value parse_object(std::size_t depth, error& e) {
    (void)depth;
    ++i; // '{'
    detail::skip_ws(buf, size, i);
    if (i >= size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }

    // In the no-string fast path, any non-empty object would necessarily contain
    // a quoted key, which would have disabled this parser. Therefore we only
    // accept empty objects here.
    if (buf[i] != '}') {
      set_error(e, error_code::expected_key_string);
      return nullptr;
    }
    ++i; // '}'
    sv_value out;
    out.k = sv_value::kind::object;
    out.u.o = {};
    return out;
  }
};

inline view_document_parse_result parse_view(std::string_view json, parse_options opt = {}) {
  view_parser p;
  p.opt = opt;
  return p.run(json);
}

inline error parse_view_into(view_document& d, std::string_view json, parse_options opt = {}) {
  view_parser p;
  p.opt = opt;
  return p.run_inplace(d, json);
}

namespace detail {

inline bool parse_u4_insitu(const char* buf, std::size_t size, std::size_t& i, std::uint32_t& out_cp) {
  if (i + 4 > size) return false;
  std::uint32_t v = 0;
  if (!parse_u4_unsafe(buf + i, v)) return false;
  i += 4;
  out_cp = v;
  return true;
}

inline void write_utf8_insitu(char* out, std::size_t& w, std::uint32_t cp) {
  if (cp <= 0x7Fu) {
    out[w++] = static_cast<char>(cp);
  } else if (cp <= 0x7FFu) {
    out[w++] = static_cast<char>(0xC0u | (cp >> 6));
    out[w++] = static_cast<char>(0x80u | (cp & 0x3Fu));
  } else if (cp <= 0xFFFFu) {
    out[w++] = static_cast<char>(0xE0u | (cp >> 12));
    out[w++] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out[w++] = static_cast<char>(0x80u | (cp & 0x3Fu));
  } else {
    out[w++] = static_cast<char>(0xF0u | (cp >> 18));
    out[w++] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
    out[w++] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
    out[w++] = static_cast<char>(0x80u | (cp & 0x3Fu));
  }
}

} // namespace detail

struct insitu_parser {
  document* doc{nullptr};
  std::string_view s;
  char* buf{nullptr};
  std::size_t size{0};
  std::size_t i{0};
  parse_options opt;

  error run_inplace(document& d) {
    doc = &d;
    buf = d.buffer_.empty() ? nullptr : &d.buffer_[0];
    size = d.buffer_.size();
    s = d.buffer();
    i = 0;

    error err;
    detail::skip_ws(buf, size, i);
    d.root_ = parse_value(0, err);
    if (err) return err;

    detail::skip_ws(buf, size, i);
    if (opt.require_eof && i != size) {
      set_error(err, error_code::trailing_characters);
      return err;
    }
    return err;
  }

  document_parse_result run() {
    document_parse_result r;
    r.doc = std::move(*doc);
    // Reset state to point into moved document.
    doc = &r.doc;
    buf = doc->buffer_.empty() ? nullptr : &doc->buffer_[0];
    size = doc->buffer_.size();
    s = doc->buffer();
    i = 0;

    detail::skip_ws(buf, size, i);
    doc->root_ = parse_value(0, r.err);
    if (r.err) return r;

    detail::skip_ws(buf, size, i);
    if (opt.require_eof && i != size) {
      set_error(r.err, error_code::trailing_characters);
      return r;
    }
    return r;
  }

  void set_error(error& e, error_code code, std::size_t at = std::numeric_limits<std::size_t>::max()) {
    if (e) return;
    e.code = code;
    e.offset = (at == std::numeric_limits<std::size_t>::max()) ? i : at;
    detail::update_line_col(s, e.offset, e.line, e.column);
  }

  sv_value parse_value(std::size_t depth, error& e) {
    if (depth > opt.max_depth) {
      set_error(e, error_code::nesting_too_deep);
      return nullptr;
    }
    if (i >= size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }
    const char c = buf[i];
    switch (c) {
      case 'n': return parse_literal("null", 4, sv_value(nullptr), e);
      case 't': return parse_literal("true", 4, sv_value(true), e);
      case 'f': return parse_literal("false", 5, sv_value(false), e);
      case '"': {
        std::string_view out;
        if (!parse_string(out, e)) return nullptr;
        return sv_value(out);
      }
      case '[': return parse_array(depth + 1, e);
      case '{': return parse_object(depth + 1, e);
      default: {
        if (c == '-' || (c >= '0' && c <= '9')) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/false)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (num.is_int) return sv_value::integer(num.i);
          return sv_value::number_token(std::string_view(buf + start, i - start));
        }
        set_error(e, error_code::invalid_value);
        return nullptr;
      }
    }
  }

  sv_value parse_literal(const char* lit, std::size_t len, sv_value v, error& e) {
    if (i + len > size) {
      set_error(e, error_code::unexpected_eof);
      return nullptr;
    }
    if (std::memcmp(buf + i, lit, len) != 0) {
      set_error(e, error_code::invalid_value);
      return nullptr;
    }
    i += len;
    return v;
  }

  bool parse_string(std::string_view& out, error& e) {
    if (i >= size || buf[i] != '"') {
      set_error(e, error_code::invalid_string);
      return false;
    }
    const std::size_t quote_pos = i;
    ++i;
    const std::size_t start = i;

    // Fast scan for unescaped strings.
#if defined(_M_X64) || defined(__SSE2__)
    {
      const __m128i q = _mm_set1_epi8('"');
      const __m128i bs = _mm_set1_epi8('\\');
      const __m128i k1f = _mm_set1_epi8(0x1F);
      const __m128i zero = _mm_setzero_si128();
      while (i + 16 <= size) {
        const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(buf + i));
        const __m128i is_q = _mm_cmpeq_epi8(v, q);
        const __m128i is_bs = _mm_cmpeq_epi8(v, bs);
        const __m128i sub = _mm_subs_epu8(v, k1f);
        const __m128i is_ctrl = _mm_cmpeq_epi8(sub, zero);
        const __m128i any = _mm_or_si128(_mm_or_si128(is_q, is_bs), is_ctrl);
        const int mask = _mm_movemask_epi8(any);
        if (mask != 0) {
#if defined(_MSC_VER)
          unsigned long bit = 0;
          _BitScanForward(&bit, static_cast<unsigned long>(mask));
          i += static_cast<std::size_t>(bit);
#else
          i += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
#endif
          const unsigned char uc = static_cast<unsigned char>(buf[i]);
          const char c = buf[i];
          if (c == '"') {
            out = std::string_view(buf + start, i - start);
            ++i;
            return true;
          }
          if (c == '\\') break;
          if (uc <= 0x1F) {
            set_error(e, error_code::invalid_string, i);
            return false;
          }
        }
        i += 16;
      }
    }
#endif
    while (i < size) {
      const unsigned char uc = static_cast<unsigned char>(buf[i]);
      const char c = buf[i];
      if (c == '"') {
        out = std::string_view(buf + start, i - start);
        ++i;
        return true;
      }
      if (c == '\\') break;
      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, i);
        return false;
      }
      ++i;
    }

    // Escaped: decode in place, always shrinking.
    // Prefix [start, i) is already in-place; begin writing at the first escape.
    std::size_t rpos = i;
    std::size_t wpos = i;
    while (rpos < size) {
      const unsigned char uc = static_cast<unsigned char>(buf[rpos]);
      const char c = buf[rpos++];
      if (c == '"') {
        out = std::string_view(buf + start, wpos - start);
        i = rpos;
        return true;
      }
      if (c == '\\') {
        if (rpos >= size) {
          set_error(e, error_code::unexpected_eof);
          return false;
        }
        const char esc = buf[rpos++];
        switch (esc) {
          case '"': buf[wpos++] = '"'; break;
          case '\\': buf[wpos++] = '\\'; break;
          case '/': buf[wpos++] = '/'; break;
          case 'b': buf[wpos++] = '\b'; break;
          case 'f': buf[wpos++] = '\f'; break;
          case 'n': buf[wpos++] = '\n'; break;
          case 'r': buf[wpos++] = '\r'; break;
          case 't': buf[wpos++] = '\t'; break;
          case 'u': {
            std::uint32_t cp = 0;
            if (!detail::parse_u4_insitu(buf, size, rpos, cp)) {
              set_error(e, error_code::invalid_unicode_escape, rpos);
              return false;
            }
            if (cp >= 0xD800u && cp <= 0xDBFFu) {
              if (rpos + 2 > size || buf[rpos] != '\\' || buf[rpos + 1] != 'u') {
                set_error(e, error_code::invalid_utf16_surrogate, rpos);
                return false;
              }
              rpos += 2;
              std::uint32_t low = 0;
              if (!detail::parse_u4_insitu(buf, size, rpos, low)) {
                set_error(e, error_code::invalid_unicode_escape, rpos);
                return false;
              }
              if (low < 0xDC00u || low > 0xDFFFu) {
                set_error(e, error_code::invalid_utf16_surrogate, rpos);
                return false;
              }
              const std::uint32_t hi = cp - 0xD800u;
              const std::uint32_t lo = low - 0xDC00u;
              cp = 0x10000u + ((hi << 10) | lo);
            } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
              set_error(e, error_code::invalid_utf16_surrogate, rpos);
              return false;
            }
            detail::write_utf8_insitu(buf, wpos, cp);
            break;
          }
          default:
            set_error(e, error_code::invalid_escape, rpos - 1);
            return false;
        }
        continue;
      }
      if (uc <= 0x1F) {
        set_error(e, error_code::invalid_string, rpos - 1);
        return false;
      }
      buf[wpos++] = c;
    }

    // Unterminated string.
    set_error(e, error_code::unexpected_eof, quote_pos);
    return false;
  }

  sv_value parse_array(std::size_t depth, error& e) {
    ++i; // '['
    detail::skip_ws(buf, size, i);
    sv_value out;
    out.k = sv_value::kind::array;
    out.u.a = {};

    auto* mr = doc->resource();
    auto reserve = [&](std::uint32_t want) {
      if (want <= out.u.a.cap) return;
      std::uint32_t new_cap = out.u.a.cap ? out.u.a.cap : 1u;
      while (new_cap < want) new_cap = (new_cap < 1024u) ? (new_cap * 2u) : (new_cap + new_cap / 2u);
      sv_value* new_data = static_cast<sv_value*>(mr->allocate(sizeof(sv_value) * new_cap, alignof(sv_value)));
      if (out.u.a.data && out.u.a.size) {
        std::memcpy(new_data, out.u.a.data, sizeof(sv_value) * out.u.a.size);
      }
      out.u.a.data = new_data;
      out.u.a.cap = new_cap;
    };

    auto is_num_start = [](char c) noexcept {
      return c == '-' || (c >= '0' && c <= '9');
    };

    // For top-level arrays that start with a number, reserve using remaining/20
    // without the old 2M cap to avoid realloc+memcpy and monotonic arena waste.
    // For other arrays, keep a conservative cap.
    if (depth <= 1) {
      const std::size_t remaining = (i < size) ? (size - i) : 0;
      const std::size_t guess = remaining / 20u;
      if (guess >= 16) {
        constexpr std::size_t kCap = 2u * 1024u * 1024u;
        const bool starts_number = (i < size) && is_num_start(buf[i]);
        const std::size_t want_sz = starts_number ? guess : std::min<std::size_t>(guess, kCap);
        const std::uint32_t want = (want_sz > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()))
                                     ? std::numeric_limits<std::uint32_t>::max()
                                     : static_cast<std::uint32_t>(want_sz);
        reserve(want);
      }
    } else {
      reserve(16);
    }

    if (i < size && buf[i] == ']') {
      ++i;
      return out;
    }

    // Fast path for number-only arrays (e.g. the numbers payload):
    // parse floats eagerly to avoid lazy conversion during sum.
    {
      detail::skip_ws(buf, size, i);
      if (i < size && is_num_start(buf[i])) {
        while (true) {
          detail::number_value num;
          const std::size_t start = i;
          if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/true)) {
            set_error(e, error_code::invalid_number, start);
            return nullptr;
          }
          if (out.u.a.size == out.u.a.cap) reserve(out.u.a.size + 1u);
          if (num.is_int) {
            out.u.a.data[out.u.a.size++] = sv_value::integer(num.i);
          } else {
            out.u.a.data[out.u.a.size++] = sv_value::number_token_with_double(std::string_view(buf + start, i - start), num.d);
          }

          if (i >= size) {
            set_error(e, error_code::unexpected_eof);
            return nullptr;
          }

          char c = buf[i];
          if (static_cast<unsigned char>(c) <= static_cast<unsigned char>(' ')) {
            detail::skip_ws(buf, size, i);
            if (i >= size) {
              set_error(e, error_code::unexpected_eof);
              return nullptr;
            }
            c = buf[i];
          }

          if (c == ',') {
            ++i;
            if (i < size && is_num_start(buf[i])) continue;
            detail::skip_ws(buf, size, i);
            if (i < size && is_num_start(buf[i])) continue;
            break;
          }
          if (c == ']') {
            ++i;
            return out;
          }
          set_error(e, error_code::expected_comma_or_end, i);
          return nullptr;
        }
      }
    }

    while (true) {
      detail::skip_ws(buf, size, i);
      sv_value elem = parse_value(depth, e);
      if (e) return nullptr;
      if (out.u.a.size == out.u.a.cap) reserve(out.u.a.size + 1u);
      out.u.a.data[out.u.a.size++] = elem;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') continue;
      if (c == ']') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }

  sv_value parse_object(std::size_t depth, error& e) {
    ++i; // '{'
    detail::skip_ws(buf, size, i);
    sv_value out;
    out.k = sv_value::kind::object;
    out.u.o = {};

    auto* mr = doc->resource();
    auto reserve = [&](std::uint32_t want) {
      if (want <= out.u.o.cap) return;
      std::uint32_t new_cap = out.u.o.cap ? out.u.o.cap : 1u;
      while (new_cap < want) new_cap = (new_cap < 1024u) ? (new_cap * 2u) : (new_cap + new_cap / 2u);
      sv_member* new_data = static_cast<sv_member*>(mr->allocate(sizeof(sv_member) * new_cap, alignof(sv_member)));
      if (out.u.o.data && out.u.o.size) {
        std::memcpy(new_data, out.u.o.data, sizeof(sv_member) * out.u.o.size);
      }
      out.u.o.data = new_data;
      out.u.o.cap = new_cap;
    };

    // Many JSON objects are small and fixed-shape; reserving a few slots helps.
    (void)depth;
    reserve(8);

    if (i < size && buf[i] == '}') {
      ++i;
      return out;
    }

    while (true) {
      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (buf[i] != '"') {
        set_error(e, error_code::expected_key_string);
        return nullptr;
      }

      std::string_view key;
      if (!parse_string(key, e)) return nullptr;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      if (buf[i] != ':') {
        set_error(e, error_code::expected_colon);
        return nullptr;
      }
      ++i;

      detail::skip_ws(buf, size, i);
      sv_value v = parse_value(depth, e);
      if (e) return nullptr;
      if (out.u.o.size == out.u.o.cap) reserve(out.u.o.size + 1u);
      new (&out.u.o.data[out.u.o.size]) sv_member{key, std::move(v)};
      ++out.u.o.size;

      detail::skip_ws(buf, size, i);
      if (i >= size) {
        set_error(e, error_code::unexpected_eof);
        return nullptr;
      }
      const char c = buf[i++];
      if (c == ',') continue;
      if (c == '}') return out;
      set_error(e, error_code::expected_comma_or_end, i - 1);
      return nullptr;
    }
  }
};

inline document_parse_result parse_in_situ(std::string json, parse_options opt = {}) {
  document d(std::move(json));

  insitu_parser p;
  p.doc = &d;
  p.opt = opt;
  return p.run();
}

// Parse into an existing document, reusing its arena blocks and string capacity.
// This avoids repeated allocations in tight loops.
inline error parse_in_situ_into(document& d, std::string_view json, parse_options opt = {}) {
  d.assign_buffer(json);
  insitu_parser p;
  p.opt = opt;
  return p.run_inplace(d);
}

// Forward declaration for parse_many_into (defined later in this header).
inline document_parse_result parse(std::string_view json, parse_options opt = {});

// Parse many independent JSON texts in parallel.
//
// This is a throughput helper for workloads like "parse N documents".
// It does not change single-document parsing semantics.
inline void parse_many_into(document_parse_result* out, const std::string_view* inputs, std::size_t count, parse_many_options options = {}) {
  if (out == nullptr || inputs == nullptr) return;
  if (count == 0) return;

  const unsigned threads = detail::effective_threads(options.max_threads);
  if (threads <= 1 || count == 1) {
    for (std::size_t i = 0; i < count; ++i) out[i] = parse(inputs[i], options.opt);
    return;
  }

  unsigned used = threads;
  if (used > static_cast<unsigned>(count)) used = static_cast<unsigned>(count);

  const std::size_t base = count / used;
  const std::size_t rem = count % used;
  std::unique_ptr<std::future<void>[]> fut(new std::future<void>[used]);

  std::size_t begin = 0;
  for (unsigned t = 0; t < used; ++t) {
    const std::size_t len = base + (t < rem ? 1u : 0u);
    const std::size_t end = begin + len;
    fut[t] = std::async(std::launch::async, [&, begin, end]() {
      for (std::size_t i = begin; i < end; ++i) out[i] = parse(inputs[i], options.opt);
    });
    begin = end;
  }
  for (unsigned t = 0; t < used; ++t) fut[t].get();
}

// -----------------------------
// Owning DOM parse API (read-only input, no full input memcpy)
// -----------------------------

// Parse from a read-only buffer into an owning `document`, without copying the full input.
// Strings/keys are copied into the arena.
// For non-integer numbers, we copy the raw number token into the arena and parse to double lazily
// on first access via as_double()/as_double_unchecked().
inline error parse_owning_view_into(document& d, std::string_view json, parse_options opt = {}) {
  owning_view_parser p;
  p.opt = opt;
  return p.run_inplace(d, json);
}

inline document_parse_result parse_owning_view(std::string_view json, parse_options opt = {}) {
  owning_view_parser p;
  p.opt = opt;
  return p.run(json);
}

inline document parse_owning_view_or_throw(std::string_view json, parse_options opt = {}) {
  auto r = parse_owning_view(json, opt);
  if (r.err) throw std::runtime_error("chjson: parse_owning_view failed");
  return std::move(r.doc);
}

// -----------------------------
// Primary DOM parse API (owning document)
// -----------------------------

// Parses JSON into an owning `document` (arena-backed DOM) and returns it.
// This replaces the old `value`-tree parse as the default for high throughput.
inline document_parse_result parse(std::string_view json, parse_options opt) {
  auto estimate_arena_reserve = [](std::size_t json_bytes, bool no_strings) noexcept -> std::size_t {
    // For parse(dom), most strings/number tokens are views into the internal buffer,
    // so the arena is dominated by array/object storage.
    //
    // However, the no-string fast path does not keep a backing buffer, so it
    // materializes more into the arena (especially number-heavy arrays). Use a
    // larger multiplier to avoid many growth allocations.
    constexpr std::size_t min_reserve = 64u * 1024u;
    constexpr std::size_t max_reserve = static_cast<std::size_t>(CHJSON_PARSE_DOM_ARENA_RESERVE_MAX);

    if (json_bytes == 0) return min_reserve;

    const std::size_t mult = no_strings ? 2u : 4u;
    std::size_t guess = json_bytes;
    if (guess > (std::numeric_limits<std::size_t>::max)() / mult) {
      guess = max_reserve;
    } else {
      guess *= mult;
    }

    if (guess < min_reserve) guess = min_reserve;
    if (guess > max_reserve) guess = max_reserve;
    return guess;
  };

  document_parse_result r;

  // Fast path: if the input contains no strings (no '"'), we don't need to keep
  // a backing buffer for string_view lifetimes. Parsing from the read-only input
  // avoids a full memcpy, which dominates Parse+sum(numbers).
  if (CHJSON_PARSE_DOM_NO_STRING_FASTPATH && json.size() != 0 &&
      std::memchr(json.data(), '"', json.size()) == nullptr) {
    r.doc.arena().reserve_bytes(estimate_arena_reserve(json.size(), /*no_strings=*/true));
    no_string_parser p;
    p.opt = opt;
    r.err = p.run_inplace(r.doc, json);
    return r;
  }

  // Default MT parse path (top-level array/object) that avoids allocator locks:
  // - Copy input into the final document buffer.
  // - Scan top-level element/member spans.
  // - Preallocate one backing buffer, split into per-thread slices.
  // - Parse each slice in parallel, allocating only from that thread's slice.
  // - Write results into preallocated root storage (no deep-copy).
  {
    // Avoid the extra full-buffer scan on small inputs. We'll only attempt the
    // MT path for large top-level containers, and for that case we fuse
    // "copy input into document buffer" + "span scan" into a single pass.
    const unsigned threads = detail::effective_threads(0);
    if (threads > 1 && json.size() >= static_cast<std::size_t>(CHJSON_PARSE_MT_MIN_BYTES)) {
      std::size_t top_i = 0;
      detail::skip_ws(json, top_i);
      const char top = (top_i < json.size()) ? json[top_i] : '\0';

      if (top == '[' || top == '{') {
        // Prepare document for in-situ parse without resetting MT buffers.
        r.doc.reserve_buffer(json.size());
        r.doc.root() = sv_value(nullptr);
        r.doc.arena().clear();
        r.doc.buffer_.resize(json.size());

        char* dst = r.doc.buffer_.data();
        const char* in = json.data();
        const std::size_t nbytes = json.size();
        const std::string_view src(dst, nbytes);

        const std::size_t min_avg = static_cast<std::size_t>(CHJSON_PARSE_MT_MIN_AVG_SPAN_BYTES);
        const std::size_t max_spans_for_avg = (min_avg == 0) ? (std::numeric_limits<std::size_t>::max)()
                         : (std::max<std::size_t>(1u, nbytes / min_avg));

        auto set_err_abs = [&](error_code code, std::size_t abs_off) {
          r.err.code = code;
          r.err.offset = abs_off;
          detail::update_line_col(src, abs_off, r.err.line, r.err.column);
        };

        auto trim_ws_span = [&](std::size_t& b, std::size_t& e) {
          while (b < e && detail::is_ws(src[b])) ++b;
          while (e > b && detail::is_ws(src[e - 1])) --e;
        };

        // Collect spans for top-level elements/members.
        std::vector<std::pair<std::size_t, std::size_t>> spans;
        spans.reserve(128);

        const std::size_t open = top_i;
        std::size_t first = open + 1;
        detail::skip_ws(json, first);
        if (first >= json.size()) {
          // Still copy the buffer for consistent offsets.
          std::memcpy(dst, in, nbytes);
          set_err_abs(error_code::unexpected_eof, json.size());
          return r;
        }

        const char close_ch = (top == '[') ? ']' : '}';

        // Copy + scan in a single pass.
        std::size_t cur = first;
        bool in_str = false;
        int depth = 1;
        bool closed = false;
        std::size_t close_pos = std::numeric_limits<std::size_t>::max();
        bool mt_abort = false;

        // Copy the prefix [0, first) upfront (no scanning work needed there).
        if (first > 0) std::memcpy(dst, in, first);

#if CHJSON_PARSE_MT_SCAN_SIMD && (defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
        #include <emmintrin.h>
#endif

        std::size_t pos = first;
        while (pos < nbytes) {
          if (mt_abort) {
            std::memcpy(dst + pos, in + pos, nbytes - pos);
            break;
          }
          if (closed) {
            // Copy the remainder and stop scanning.
            std::memcpy(dst + pos, in + pos, nbytes - pos);
            break;
          }

          if (in_str) {
            // Fast-skip string content by jumping to the next '"' using memchr.
            // Only take a small slow-path when we see an escaped quote.
            const char* p = in + pos;
            const char* endp = in + nbytes;
            while (true) {
              const void* qv = std::memchr(p, '"', static_cast<std::size_t>(endp - p));
              if (qv == nullptr) {
                std::memcpy(dst + pos, in + pos, nbytes - pos);
                set_err_abs(error_code::unexpected_eof, nbytes);
                return r;
              }
              const char* q = static_cast<const char*>(qv);
              const std::size_t qpos = static_cast<std::size_t>(q - in);

              std::memcpy(dst + pos, in + pos, (qpos + 1) - pos);

              // Count consecutive backslashes immediately preceding the quote.
              std::size_t bs = 0;
              std::size_t k = qpos;
              while (k > 0 && k > open && in[k - 1] == '\\') {
                ++bs;
                --k;
              }

              pos = qpos + 1;
              if ((bs & 1u) == 0u) {
                in_str = false;
                break;
              }

              // Escaped quote: continue scanning from after it.
              p = in + pos;
              if (p >= endp) {
                set_err_abs(error_code::unexpected_eof, nbytes);
                return r;
              }
            }
            continue;
          }

          // Not in string: SIMD-skip blocks with no special characters.
#if CHJSON_PARSE_MT_SCAN_SIMD && (defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
          if (pos + 16 <= nbytes) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(in + pos));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + pos), v);

            const __m128i q = _mm_cmpeq_epi8(v, _mm_set1_epi8('"'));
            const __m128i lb = _mm_cmpeq_epi8(v, _mm_set1_epi8('['));
            const __m128i rb = _mm_cmpeq_epi8(v, _mm_set1_epi8(']'));
            const __m128i lc = _mm_cmpeq_epi8(v, _mm_set1_epi8('{'));
            const __m128i rc = _mm_cmpeq_epi8(v, _mm_set1_epi8('}'));
            const __m128i cm = _mm_cmpeq_epi8(v, _mm_set1_epi8(','));

            const __m128i any = _mm_or_si128(_mm_or_si128(_mm_or_si128(q, lb), _mm_or_si128(rb, lc)), _mm_or_si128(rc, cm));
            const int mask = _mm_movemask_epi8(any);
            if (mask == 0) {
              pos += 16;
              continue;
            }

            // Slow path within this 16-byte block: handle only interesting bytes.
            for (int off = 0; off < 16; ++off) {
              const std::size_t i2 = pos + static_cast<std::size_t>(off);
              const char c = in[i2];

              if (c == '"') {
                in_str = true;
                pos = i2 + 1;
                break;
              }

              if (c == '[' || c == '{') {
                ++depth;
                continue;
              }
              if (c == ']' || c == '}') {
                --depth;
                if (depth == 0 && c == close_ch) {
                  std::size_t b = cur;
                  std::size_t e = i2;
                  trim_ws_span(b, e);
                  if (b == e) {
                    set_err_abs(top == '[' ? error_code::invalid_value : error_code::expected_key_string, b);
                    return r;
                  }
                  spans.emplace_back(b, e);
                  close_pos = i2;
                  closed = true;
                  pos = i2 + 1;
                  break;
                }
                continue;
              }

              if (c == ',' && depth == 1) {
                std::size_t b = cur;
                std::size_t e = i2;
                trim_ws_span(b, e);
                if (b == e) {
                  set_err_abs(top == '[' ? error_code::invalid_value : error_code::expected_key_string, b);
                  return r;
                }
                spans.emplace_back(b, e);
                if (spans.size() > max_spans_for_avg) {
                  // MT thresholds are now impossible (avg_span would be < min_avg).
                  // Stop scanning early and fall back to single-thread parse.
                  mt_abort = true;
                  pos = i2 + 1;
                  break;
                }
                cur = i2 + 1;
                continue;
              }
            }

            if (mt_abort) {
              std::memcpy(dst + pos, in + pos, nbytes - pos);
              break;
            }

            // If we didn't break out (still not in_str and not closed), advance past the block.
            if (!in_str && !closed) pos += 16;
            continue;
          }
#endif

          // Scalar tail / fallback.
          const char c = in[pos];
          dst[pos] = c;
          if (c == '"') {
            in_str = true;
            ++pos;
            continue;
          }

          if (c == '[' || c == '{') {
            ++depth;
            ++pos;
            continue;
          }
          if (c == ']' || c == '}') {
            --depth;
            if (depth == 0 && c == close_ch) {
              std::size_t b = cur;
              std::size_t e = pos;
              trim_ws_span(b, e);
              if (b == e) {
                set_err_abs(top == '[' ? error_code::invalid_value : error_code::expected_key_string, b);
                return r;
              }
              spans.emplace_back(b, e);
              close_pos = pos;
              closed = true;
            }
            ++pos;
            continue;
          }

          if (c == ',' && depth == 1) {
            std::size_t b = cur;
            std::size_t e = pos;
            trim_ws_span(b, e);
            if (b == e) {
              set_err_abs(top == '[' ? error_code::invalid_value : error_code::expected_key_string, b);
              return r;
            }
            spans.emplace_back(b, e);
            if (spans.size() > max_spans_for_avg) {
              mt_abort = true;
              ++pos;
              std::memcpy(dst + pos, in + pos, nbytes - pos);
              break;
            }
            cur = pos + 1;
            ++pos;
            continue;
          }

          ++pos;
        }

        if (mt_abort) {
          // MT thresholds are impossible: parse in-situ on the already-copied buffer.
          r.doc.arena().reserve_bytes(estimate_arena_reserve(json.size(), /*no_strings=*/false));
          insitu_parser p;
          p.opt = opt;
          r.err = p.run_inplace(r.doc);
          return r;
        }

        if (!closed) {
          set_err_abs(error_code::unexpected_eof, src.size());
          return r;
        }

        // Validate trailing characters if required.
        std::size_t end = close_pos + 1;
        detail::skip_ws(src, end);
        if (opt.require_eof && end != src.size()) {
          set_err_abs(error_code::trailing_characters, end);
          return r;
        }

        // Heuristic: only parallelize if there is enough work.
        // MT parse has fixed overheads (task launch, alloc/init), so keep it conservative.
        const std::size_t spans_count = spans.size();
        const std::size_t total_bytes = src.size();
        const std::size_t avg_span = (spans_count == 0) ? 0 : (total_bytes / spans_count);
        if (spans_count >= static_cast<std::size_t>(CHJSON_PARSE_MT_MIN_SPANS) &&
            avg_span >= static_cast<std::size_t>(CHJSON_PARSE_MT_MIN_AVG_SPAN_BYTES)) {
        unsigned used = threads;
        if (used > static_cast<unsigned>(spans_count)) used = static_cast<unsigned>(spans_count);

        // Lock-free-ish MT parse: threads only allocate from pre-sized per-thread slices.
        // No heap fallback inside worker threads.
        constexpr unsigned k_max_mt_retries = 2;
        for (unsigned attempt = 0; attempt < k_max_mt_retries; ++attempt) {
          // Allocate one backing buffer and split it into per-thread slices.
          // Keep it conservative: the DOM arena demand is typically <= ~4x input.
          std::size_t total = estimate_arena_reserve(src.size(), /*no_strings=*/false);
          if (attempt != 0) total *= 2;
          const std::size_t per = (used == 0) ? total : (total / used);
          const std::size_t align = alignof(std::max_align_t);
          const std::size_t slice = ((per + align - 1) / align) * align;
          const std::size_t backing = slice * static_cast<std::size_t>(used);

          // Reuse MT backing/resources if possible to avoid per-parse allocations.
          if (!r.doc.mt_parse_backing_ || r.doc.mt_parse_backing_size_ < backing) {
            r.doc.mt_parse_backing_.reset();
            r.doc.mt_parse_backing_ = detail::chjson_byte_ptr(static_cast<std::byte*>(detail::chjson_allocate(backing, alignof(std::max_align_t))),
                                                             detail::chjson_sized_byte_deleter{backing});
            r.doc.mt_parse_backing_size_ = backing;
          }

          unsigned res_cap = 0;
          if (r.doc.mt_parse_resources_) res_cap = r.doc.mt_parse_resources_.get_deleter().count;
          if (res_cap < used) {
            auto* raw = static_cast<std::pmr::monotonic_buffer_resource*>(
                detail::chjson_allocate(sizeof(std::pmr::monotonic_buffer_resource) * static_cast<std::size_t>(used),
                                        alignof(std::pmr::monotonic_buffer_resource)));
            r.doc.mt_parse_resources_ = decltype(r.doc.mt_parse_resources_)(raw, document::mt_parse_resources_deleter{used});
            res_cap = used;
          }

          auto* res = r.doc.mt_parse_resources_.get();
          for (unsigned t = 0; t < used; ++t) {
            void* base = r.doc.mt_parse_backing_.get() + static_cast<std::size_t>(t) * slice;
            // Reset by rebuilding with a fresh initial buffer.
            res[t].~monotonic_buffer_resource();
            new (res + t) std::pmr::monotonic_buffer_resource(base, slice, std::pmr::null_memory_resource());
          }

          // Root container storage lives in the document arena (single-thread allocate).
          // Publish size only after successful parse to avoid exposing partially-initialized slots.
          if (top == '[') {
            sv_value root = sv_value::make_array(r.doc.resource());
            root.array_reserve(r.doc.resource(), static_cast<std::uint32_t>(spans_count));
            root.u.a.size = 0;
            r.doc.root() = root;
          } else {
            sv_value root = sv_value::make_object(r.doc.resource());
            root.object_reserve(r.doc.resource(), static_cast<std::uint32_t>(spans_count));
            root.u.o.size = 0;
            r.doc.root() = root;
          }

          sv_value* array_out = nullptr;
          sv_member* object_out = nullptr;
          if (top == '[') {
            array_out = r.doc.root().u.a.data;
          } else {
            object_out = r.doc.root().u.o.data;
          }

        // Read-only parser that allocates only on escapes/containers.
        struct readonly_parser {
          std::pmr::memory_resource* mr{nullptr};
          std::string_view s;
          const char* buf{nullptr};
          std::size_t size{0};
          std::size_t i{0};
          parse_options opt;

          void set_error(error& e, error_code code, std::size_t at = std::numeric_limits<std::size_t>::max()) {
            if (e) return;
            e.code = code;
            e.offset = (at == std::numeric_limits<std::size_t>::max()) ? i : at;
            detail::update_line_col(s, e.offset, e.line, e.column);
          }

          sv_value parse_literal(const char* lit, std::size_t len, sv_value v, error& e) {
            if (i + len > size) {
              set_error(e, error_code::unexpected_eof);
              return nullptr;
            }
            if (std::memcmp(buf + i, lit, len) != 0) {
              set_error(e, error_code::invalid_value);
              return nullptr;
            }
            i += len;
            return v;
          }

          bool parse_string(std::string_view& out, error& e) {
            if (i >= size || buf[i] != '"') {
              set_error(e, error_code::invalid_string);
              return false;
            }
            const std::size_t quote_pos = i;
            ++i;
            const std::size_t start = i;

            // Fast scan for unescaped strings.
            while (i < size) {
              const unsigned char uc = static_cast<unsigned char>(buf[i]);
              const char c = buf[i];
              if (c == '"') {
                out = std::string_view(buf + start, i - start);
                ++i;
                return true;
              }
              if (c == '\\') break;
              if (uc <= 0x1F) {
                set_error(e, error_code::invalid_string, i);
                return false;
              }
              ++i;
            }

            // Escaped: two-pass decode into the arena.
            const std::size_t first_escape = i;
            std::size_t end_quote = std::numeric_limits<std::size_t>::max();
            {
              std::size_t scan = first_escape;
              while (scan < size) {
                const unsigned char uc = static_cast<unsigned char>(buf[scan]);
                const char c = buf[scan++];
                if (c == '"') {
                  end_quote = scan - 1;
                  break;
                }
                if (c == '\\') {
                  if (scan >= size) {
                    set_error(e, error_code::unexpected_eof, quote_pos);
                    return false;
                  }
                  const char esc = buf[scan++];
                  if (esc == 'u') {
                    if (scan + 4 > size) {
                      set_error(e, error_code::unexpected_eof, quote_pos);
                      return false;
                    }
                    scan += 4;
                  }
                  continue;
                }
                if (uc <= 0x1F) {
                  set_error(e, error_code::invalid_string, scan - 1);
                  return false;
                }
              }
            }

            if (end_quote == std::numeric_limits<std::size_t>::max()) {
              set_error(e, error_code::unexpected_eof, quote_pos);
              return false;
            }

            const std::size_t raw_len = end_quote - start;
            char* dst = (raw_len != 0) ? static_cast<char*>(mr->allocate(raw_len, alignof(char))) : nullptr;
            std::size_t rpos = start;
            std::size_t wpos = 0;
            while (rpos < end_quote) {
              const unsigned char uc = static_cast<unsigned char>(buf[rpos]);
              const char c = buf[rpos++];
              if (c == '\\') {
                if (rpos >= end_quote) {
                  set_error(e, error_code::unexpected_eof, quote_pos);
                  return false;
                }
                const char esc = buf[rpos++];
                switch (esc) {
                  case '"': dst[wpos++] = '"'; break;
                  case '\\': dst[wpos++] = '\\'; break;
                  case '/': dst[wpos++] = '/'; break;
                  case 'b': dst[wpos++] = '\b'; break;
                  case 'f': dst[wpos++] = '\f'; break;
                  case 'n': dst[wpos++] = '\n'; break;
                  case 'r': dst[wpos++] = '\r'; break;
                  case 't': dst[wpos++] = '\t'; break;
                  case 'u': {
                    std::uint32_t cp = 0;
                    if (!detail::parse_u4_insitu(buf, size, rpos, cp)) {
                      set_error(e, error_code::invalid_unicode_escape, rpos);
                      return false;
                    }
                    if (cp >= 0xD800u && cp <= 0xDBFFu) {
                      if (rpos + 2 > size || buf[rpos] != '\\' || buf[rpos + 1] != 'u') {
                        set_error(e, error_code::invalid_utf16_surrogate, rpos);
                        return false;
                      }
                      rpos += 2;
                      std::uint32_t low = 0;
                      if (!detail::parse_u4_insitu(buf, size, rpos, low)) {
                        set_error(e, error_code::invalid_unicode_escape, rpos);
                        return false;
                      }
                      if (low < 0xDC00u || low > 0xDFFFu) {
                        set_error(e, error_code::invalid_utf16_surrogate, rpos);
                        return false;
                      }
                      const std::uint32_t hi = cp - 0xD800u;
                      const std::uint32_t lo = low - 0xDC00u;
                      cp = 0x10000u + ((hi << 10) | lo);
                    } else if (cp >= 0xDC00u && cp <= 0xDFFFu) {
                      set_error(e, error_code::invalid_utf16_surrogate, rpos);
                      return false;
                    }
                    detail::write_utf8_insitu(dst, wpos, cp);
                    break;
                  }
                  default:
                    set_error(e, error_code::invalid_escape, rpos - 1);
                    return false;
                }
                continue;
              }
              if (uc <= 0x1F) {
                set_error(e, error_code::invalid_string, rpos - 1);
                return false;
              }
              dst[wpos++] = c;
            }

            out = std::string_view(dst, wpos);
            i = end_quote + 1;
            return true;
          }

          sv_value parse_value(std::size_t depth, error& e) {
            if (depth > opt.max_depth) {
              set_error(e, error_code::nesting_too_deep);
              return nullptr;
            }
            if (i >= size) {
              set_error(e, error_code::unexpected_eof);
              return nullptr;
            }

            const char c = buf[i];
            switch (c) {
              case 'n': return parse_literal("null", 4, sv_value(nullptr), e);
              case 't': return parse_literal("true", 4, sv_value(true), e);
              case 'f': return parse_literal("false", 5, sv_value(false), e);
              case '"': {
                std::string_view out;
                if (!parse_string(out, e)) return nullptr;
                return sv_value(out);
              }
              case '[': return parse_array(depth + 1, e);
              case '{': return parse_object(depth + 1, e);
              default: {
                if (c == '-' || (c >= '0' && c <= '9')) {
                  detail::number_value num;
                  const std::size_t start = i;
                  if (!detail::parse_number(buf, size, i, num, /*parse_fp=*/false)) {
                    set_error(e, error_code::invalid_number, start);
                    return nullptr;
                  }
                  if (num.is_int) return sv_value::integer(num.i);
                  return sv_value::number_token(std::string_view(buf + start, i - start));
                }
                set_error(e, error_code::invalid_value);
                return nullptr;
              }
            }
          }

          sv_value parse_array(std::size_t depth, error& e) {
            ++i; // '['
            detail::skip_ws(buf, size, i);
            sv_value out = sv_value::make_array(mr);

            if (i < size && buf[i] == ']') {
              ++i;
              return out;
            }

            // Conservative reserve.
            out.array_reserve(mr, 16u);

            while (true) {
              detail::skip_ws(buf, size, i);
              sv_value elem = parse_value(depth, e);
              if (e) return nullptr;
              out.array_push_back(mr, std::move(elem));

              detail::skip_ws(buf, size, i);
              if (i >= size) {
                set_error(e, error_code::unexpected_eof);
                return nullptr;
              }
              const char c = buf[i++];
              if (c == ',') continue;
              if (c == ']') return out;
              set_error(e, error_code::expected_comma_or_end, i - 1);
              return nullptr;
            }
          }

          sv_value parse_object(std::size_t depth, error& e) {
            ++i; // '{'
            detail::skip_ws(buf, size, i);
            sv_value out = sv_value::make_object(mr);
            (void)depth;
            out.object_reserve(mr, 8u);

            if (i < size && buf[i] == '}') {
              ++i;
              return out;
            }

            while (true) {
              detail::skip_ws(buf, size, i);
              if (i >= size) {
                set_error(e, error_code::unexpected_eof);
                return nullptr;
              }
              if (buf[i] != '"') {
                set_error(e, error_code::expected_key_string);
                return nullptr;
              }

              std::string_view key;
              if (!parse_string(key, e)) return nullptr;

              detail::skip_ws(buf, size, i);
              if (i >= size) {
                set_error(e, error_code::unexpected_eof);
                return nullptr;
              }
              if (buf[i] != ':') {
                set_error(e, error_code::expected_colon);
                return nullptr;
              }
              ++i;

              detail::skip_ws(buf, size, i);
              sv_value v = parse_value(depth, e);
              if (e) return nullptr;
              out.object_emplace_back(mr, key, std::move(v));

              detail::skip_ws(buf, size, i);
              if (i >= size) {
                set_error(e, error_code::unexpected_eof);
                return nullptr;
              }
              const char c = buf[i++];
              if (c == ',') continue;
              if (c == '}') return out;
              set_error(e, error_code::expected_comma_or_end, i - 1);
              return nullptr;
            }
          }
        };

        std::unique_ptr<std::thread[]> thr(new std::thread[used]);
        std::unique_ptr<error[]> errs(new error[used]);
        std::unique_ptr<std::size_t[]> err_abs(new std::size_t[used]);
        std::unique_ptr<unsigned char[]> built(new unsigned char[spans_count]);
        for (unsigned t = 0; t < used; ++t) {
          errs[t] = {};
          err_abs[t] = std::numeric_limits<std::size_t>::max();
        }
        std::memset(built.get(), 0, spans_count);

        const std::size_t n = spans.size();
        const std::size_t base = n / used;
        const std::size_t rem = n % used;
        std::size_t begin = 0;
        for (unsigned t = 0; t < used; ++t) {
          const std::size_t len = base + (t < rem ? 1u : 0u); 
          const std::size_t end = begin + len;
          thr[t] = std::thread([&, t, begin, end]() {
            readonly_parser p;
            p.mr = r.doc.mt_parse_resources_.get() + t;
            p.opt = opt;

            try {
              for (std::size_t idx = begin; idx < end; ++idx) {
                const auto [b, e] = spans[idx];
                const std::string_view slice(src.data() + b, e - b);
                p.s = slice;
                p.buf = slice.data();
                p.size = slice.size();
                p.i = 0;

                error err;
                detail::skip_ws(p.buf, p.size, p.i);
                if (top == '[') {
                  // element is inside the outer array => depth=1
                  sv_value v = p.parse_value(1, err);
                  if (err) {
                    errs[t] = err;
                    err_abs[t] = b + err.offset;
                    return;
                  }
                  detail::skip_ws(p.buf, p.size, p.i);
                  if (opt.require_eof && p.i != p.size) {
                    errs[t].code = error_code::expected_comma_or_end;
                    errs[t].offset = p.i;
                    err_abs[t] = b + p.i;
                    return;
                  }
                  // Write into preallocated root storage.
                  new (array_out + idx) sv_value(std::move(v));
                  built[idx] = 1;
                } else {
                  // member: parse key ':' value
                  if (p.i >= p.size || p.buf[p.i] != '"') {
                    errs[t].code = error_code::expected_key_string;
                    errs[t].offset = p.i;
                    err_abs[t] = b + p.i;
                    return;
                  }
                  std::string_view key;
                  if (!p.parse_string(key, err)) {
                    errs[t] = err;
                    err_abs[t] = b + err.offset;
                    return;
                  }
                  detail::skip_ws(p.buf, p.size, p.i);
                  if (p.i >= p.size || p.buf[p.i] != ':') {
                    errs[t].code = error_code::expected_colon;
                    errs[t].offset = p.i;
                    err_abs[t] = b + p.i;
                    return;
                  }
                  ++p.i;
                  detail::skip_ws(p.buf, p.size, p.i);
                  sv_value v = p.parse_value(1, err);
                  if (err) {
                    errs[t] = err;
                    err_abs[t] = b + err.offset;
                    return;
                  }
                  detail::skip_ws(p.buf, p.size, p.i);
                  if (opt.require_eof && p.i != p.size) {
                    errs[t].code = error_code::expected_comma_or_end;
                    errs[t].offset = p.i;
                    err_abs[t] = b + p.i;
                    return;
                  }
                  new (object_out + idx) sv_member{key, std::move(v)};
                  built[idx] = 1;
                }
              }
            } catch (const std::bad_alloc&) {
              errs[t].code = error_code::out_of_memory;
              errs[t].offset = 0;
              err_abs[t] = 0;
              return;
            } catch (...) {
              errs[t].code = error_code::out_of_memory;
              errs[t].offset = 0;
              err_abs[t] = 0;
              return;
            }
          });
          begin = end;
        }

        for (unsigned t = 0; t < used; ++t) {
          if (thr[t].joinable()) thr[t].join();
        }

        // Report earliest error.
        std::size_t best_off = std::numeric_limits<std::size_t>::max();
        error best_err;
        for (unsigned t = 0; t < used; ++t) {
          if (errs[t] && err_abs[t] < best_off) {
            best_off = err_abs[t];
            best_err = errs[t];
          }
        }
        if (best_off != std::numeric_limits<std::size_t>::max()) {
          // Clean up constructed root slots so retries/fallback don't overwrite live objects.
          if (top == '[') {
            for (std::size_t idx = 0; idx < spans_count; ++idx) {
              if (built[idx]) (array_out + idx)->~sv_value();
            }
          } else {
            for (std::size_t idx = 0; idx < spans_count; ++idx) {
              if (built[idx]) (object_out + idx)->~sv_member();
            }
          }

          if (best_err.code == error_code::out_of_memory && attempt + 1 < k_max_mt_retries) {
            // Retry with larger per-thread slices.
            continue;
          }
          set_err_abs(best_err.code, best_off);
          return r;
        }

        if (top == '[') {
          r.doc.root().u.a.size = static_cast<std::uint32_t>(spans_count);
        } else {
          r.doc.root().u.o.size = static_cast<std::uint32_t>(spans_count);
        }

        r.err = {};
        return r;
        }
      }

        // MT thresholds not met: parse in-situ on the already-copied buffer.
        r.doc.arena().reserve_bytes(estimate_arena_reserve(json.size(), /*no_strings=*/false));
        insitu_parser p;
        p.opt = opt;
        r.err = p.run_inplace(r.doc);
        return r;
      }
    }
  }

  // Fallback: keep existing single-thread behavior.
  r.doc.reserve_buffer(json.size());
  r.doc.arena().reserve_bytes(estimate_arena_reserve(json.size(), /*no_strings=*/false));
  r.err = parse_in_situ_into(r.doc, json, opt);
  return r;
}

inline document parse_or_throw(std::string_view json, parse_options opt = {}) {
  auto r = parse(json, opt);
  if (r.err) throw std::runtime_error("chjson: parse failed");
  return std::move(r.doc);
}

inline document parse_in_situ_or_throw(std::string json, parse_options opt = {}) {
  auto r = parse_in_situ(std::move(json), opt);
  if (r.err) throw std::runtime_error("chjson: parse_in_situ failed");
  return std::move(r.doc);
}

inline void dump_to(std::string& out, const sv_value& v, bool pretty = false, int indent = 0) {
  // Keep the signature but avoid pretty-branching in the hot (compact) path.
  auto dump_number = [&](const sv_number_value& n) {
    if (n.is_int()) {
      detail::dump_int64(out, n.int_value());
      return;
    }
    if (n.has_raw()) {
      out.append(n.raw_data, static_cast<std::size_t>(n.raw_size()));
      return;
    }
    detail::dump_double(out, n.double_value());
  };

  if (!pretty) {
    struct frame {
      const sv_value* v{nullptr};
      std::size_t idx{0};
    };
    frame stack[512];
    std::size_t sp = 0;
    stack[sp++] = frame{&v, 0};

    while (sp != 0) {
      frame& f = stack[sp - 1];
      const sv_value& cur = *f.v;

      switch (cur.type()) {
        case sv_value::kind::null:
          out += "null";
          --sp;
          break;
        case sv_value::kind::boolean:
          out += cur.as_bool() ? "true" : "false";
          --sp;
          break;
        case sv_value::kind::number:
          dump_number(cur.u.num);
          --sp;
          break;
        case sv_value::kind::string:
          detail::dump_escaped(out, cur.as_string_view());
          --sp;
          break;
        case sv_value::kind::array: {
          const auto& a = cur.as_array();
          if (f.idx == 0) {
            out.push_back('[');
            if (a.empty()) {
              out.push_back(']');
              --sp;
              break;
            }
          }
          if (f.idx == a.size()) {
            out.push_back(']');
            --sp;
            break;
          }
          if (f.idx > 0) out.push_back(',');
          stack[sp++] = frame{&a[f.idx++], 0};
          break;
        }
        case sv_value::kind::object: {
          const auto& o = cur.as_object();
          if (f.idx == 0) {
            out.push_back('{');
            if (o.empty()) {
              out.push_back('}');
              --sp;
              break;
            }
          }
          if (f.idx == o.size()) {
            out.push_back('}');
            --sp;
            break;
          }
          if (f.idx > 0) out.push_back(',');
          const auto& kv = o[f.idx++];
          detail::dump_escaped(out, kv.first);
          out.push_back(':');
          stack[sp++] = frame{&kv.second, 0};
          break;
        }
      }
    }
    return;
  }

  switch (v.type()) {
    case sv_value::kind::null:
      out += "null";
      return;
    case sv_value::kind::boolean:
      out += v.as_bool() ? "true" : "false";
      return;
    case sv_value::kind::number:
      dump_number(v.u.num);
      return;
    case sv_value::kind::string:
      detail::dump_escaped(out, v.as_string_view());
      return;
    case sv_value::kind::array: {
      const auto& a = v.as_array();
      out.push_back('[');
      if (!a.empty()) out.push_back('\n');
      for (std::size_t idx = 0; idx < a.size(); ++idx) {
        detail::dump_indent(out, indent + 2);
        dump_to(out, a[idx], /*pretty=*/true, indent + 2);
        if (idx + 1 != a.size()) out.push_back(',');
        out.push_back('\n');
      }
      if (!a.empty()) detail::dump_indent(out, indent);
      out.push_back(']');
      return;
    }
    case sv_value::kind::object: {
      const auto& o = v.as_object();
      out.push_back('{');
      if (!o.empty()) out.push_back('\n');
      for (std::size_t idx = 0; idx < o.size(); ++idx) {
        detail::dump_indent(out, indent + 2);
        detail::dump_escaped(out, o[idx].first);
        out.append(": ", 2);
        dump_to(out, o[idx].second, /*pretty=*/true, indent + 2);
        if (idx + 1 != o.size()) out.push_back(',');
        out.push_back('\n');
      }
      if (!o.empty()) detail::dump_indent(out, indent);
      out.push_back('}');
      return;
    }
  }
}

// Forward declaration: forced-mt dump() wrapper calls this.
inline void dump_to_mt(std::string& out, const sv_value& v, dump_mt_options opt);

inline std::string dump(const sv_value& v, bool pretty = false) {
  struct dump_reserve_hints {
    std::size_t compact{256};
    std::size_t pretty{256};
  };
  thread_local dump_reserve_hints hints;

  std::string out;
  std::size_t& hint = pretty ? hints.pretty : hints.compact;
  out.reserve(hint);

  // Default dump(): use the MT-aware dumper.
  // It will only parallelize when the top-level container is large enough.
  dump_mt_options opt;
  opt.pretty = pretty;
  opt.max_threads = 0;
  opt.min_parallel_items = 1024;
  opt.max_parallel_depth = 1;
  dump_to_mt(out, v, opt);

  constexpr std::size_t min_hint = 256;
  constexpr std::size_t max_hint = 16u * 1024u * 1024u;
  const std::size_t sz = out.size();
  hint = (sz < min_hint) ? min_hint : (sz > max_hint ? max_hint : sz);
  return out;
}

// Multithreaded dump for arena-backed DOM values (sv_value).
inline void dump_to_mt(std::string& out, const sv_value& v, dump_mt_options opt = {}) {
  // Fast reject: only parallelize top-level arrays/objects that are large enough.
  // Avoids per-call async/scheduler overhead for tiny payloads in hot loops.
  if (opt.max_parallel_depth == 0) {
    dump_to(out, v, opt.pretty, 0);
    return;
  }

  if (v.type() == sv_value::kind::array) {
    if (v.u.a.size < opt.min_parallel_items) {
      dump_to(out, v, opt.pretty, 0);
      return;
    }
  } else if (v.type() == sv_value::kind::object) {
    if (v.u.o.size < opt.min_parallel_items) {
      dump_to(out, v, opt.pretty, 0);
      return;
    }
  } else {
    dump_to(out, v, opt.pretty, 0);
    return;
  }

  const unsigned threads = detail::effective_threads(opt.max_threads);
  if (threads <= 1) {
    dump_to(out, v, opt.pretty, 0);
    return;
  }

  struct mt_ctx {
    std::vector<std::string> chunks;
    std::vector<std::future<void>> fut;
  };
  thread_local mt_ctx ctx;

  // Only parallelize top-level array/object; nested parallelism is limited by opt.max_parallel_depth.
  // Primitive values fall back to the existing single-thread code.
  struct mt {
    static void dump_value(std::string& out, const sv_value& v, int indent, unsigned depth, const dump_mt_options& opt, unsigned threads, mt_ctx& ctx) {
      if (!opt.pretty) {
        if (v.type() == sv_value::kind::array) {
          const auto& a = v.as_array();
          if (detail::should_parallelize_container(a.size(), depth, opt, threads)) {
            const unsigned used = threads > static_cast<unsigned>(a.size()) ? static_cast<unsigned>(a.size()) : threads;
            ctx.chunks.resize(used);
            const std::size_t n = a.size();
            const std::size_t base = n / used;
            const std::size_t rem = n % used;
            ctx.fut.resize(used);
            std::size_t begin = 0;
            for (unsigned t = 0; t < used; ++t) {
              const std::size_t len = base + (t < rem ? 1u : 0u);
              const std::size_t end = begin + len;
              ctx.fut[t] = std::async(std::launch::async, [&, t, begin, end]() {
                std::string buf;
                buf.reserve((end - begin) * 80u);
                for (std::size_t i = begin; i < end; ++i) {
                  if (i != begin) buf.push_back(',');
                  dump_to(buf, a[i], /*pretty=*/false, 0);
                }
                ctx.chunks[t] = std::move(buf);
              });
              begin = end;
            }
            for (unsigned t = 0; t < used; ++t) ctx.fut[t].get();
            out.push_back('[');
            bool first = true;
            for (unsigned t = 0; t < used; ++t) {
              if (ctx.chunks[t].empty()) continue;
              if (!first) out.push_back(',');
              out.append(ctx.chunks[t]);
              first = false;
            }
            out.push_back(']');
            return;
          }
        } else if (v.type() == sv_value::kind::object) {
          const auto& o = v.as_object();
          if (detail::should_parallelize_container(o.size(), depth, opt, threads)) {
            const unsigned used = threads > static_cast<unsigned>(o.size()) ? static_cast<unsigned>(o.size()) : threads;
            ctx.chunks.resize(used);
            const std::size_t n = o.size();
            const std::size_t base = n / used;
            const std::size_t rem = n % used;
            ctx.fut.resize(used);
            std::size_t begin = 0;
            for (unsigned t = 0; t < used; ++t) {
              const std::size_t len = base + (t < rem ? 1u : 0u);
              const std::size_t end = begin + len;
              ctx.fut[t] = std::async(std::launch::async, [&, t, begin, end]() {
                std::string buf;
                buf.reserve((end - begin) * 96u);
                for (std::size_t i = begin; i < end; ++i) {
                  if (i != begin) buf.push_back(',');
                  detail::dump_escaped(buf, o[i].first);
                  buf.push_back(':');
                  dump_to(buf, o[i].second, /*pretty=*/false, 0);
                }
                ctx.chunks[t] = std::move(buf);
              });
              begin = end;
            }
            for (unsigned t = 0; t < used; ++t) ctx.fut[t].get();
            out.push_back('{');
            bool first = true;
            for (unsigned t = 0; t < used; ++t) {
              if (ctx.chunks[t].empty()) continue;
              if (!first) out.push_back(',');
              out.append(ctx.chunks[t]);
              first = false;
            }
            out.push_back('}');
            return;
          }
        }

        dump_to(out, v, /*pretty=*/false, 0);
        return;
      }

      // pretty
      if (v.type() == sv_value::kind::array) {
        const auto& a = v.as_array();
        out.push_back('[');
        if (a.empty()) {
          out.push_back(']');
          return;
        }
        out.push_back('\n');
        const bool par = detail::should_parallelize_container(a.size(), depth, opt, threads);
        if (!par) {
          for (std::size_t i = 0; i < a.size(); ++i) {
            detail::dump_indent(out, indent + 2);
            dump_to(out, a[i], /*pretty=*/true, indent + 2);
            if (i + 1 != a.size()) out.push_back(',');
            out.push_back('\n');
          }
          detail::dump_indent(out, indent);
          out.push_back(']');
          return;
        }

        const unsigned used = threads > static_cast<unsigned>(a.size()) ? static_cast<unsigned>(a.size()) : threads;
        ctx.chunks.resize(used);
        const std::size_t n = a.size();
        const std::size_t base = n / used;
        const std::size_t rem = n % used;
        ctx.fut.resize(used);
        std::size_t begin = 0;
        for (unsigned t = 0; t < used; ++t) {
          const std::size_t len = base + (t < rem ? 1u : 0u);
          const std::size_t end = begin + len;
          ctx.fut[t] = std::async(std::launch::async, [&, t, begin, end]() {
            std::string buf;
            buf.reserve((end - begin) * 96u);
            for (std::size_t i = begin; i < end; ++i) {
              detail::dump_indent(buf, indent + 2);
              dump_to(buf, a[i], /*pretty=*/true, indent + 2);
              if (i + 1 != a.size()) buf.push_back(',');
              buf.push_back('\n');
            }
            ctx.chunks[t] = std::move(buf);
          });
          begin = end;
        }
        for (unsigned t = 0; t < used; ++t) ctx.fut[t].get();
        for (unsigned t = 0; t < used; ++t) out.append(ctx.chunks[t]);
        detail::dump_indent(out, indent);
        out.push_back(']');
        return;
      }
      if (v.type() == sv_value::kind::object) {
        const auto& o = v.as_object();
        out.push_back('{');
        if (o.empty()) {
          out.push_back('}');
          return;
        }
        out.push_back('\n');
        const bool par = detail::should_parallelize_container(o.size(), depth, opt, threads);
        if (!par) {
          for (std::size_t i = 0; i < o.size(); ++i) {
            detail::dump_indent(out, indent + 2);
            detail::dump_escaped(out, o[i].first);
            out.append(": ", 2);
            dump_to(out, o[i].second, /*pretty=*/true, indent + 2);
            if (i + 1 != o.size()) out.push_back(',');
            out.push_back('\n');
          }
          detail::dump_indent(out, indent);
          out.push_back('}');
          return;
        }

        const unsigned used = threads > static_cast<unsigned>(o.size()) ? static_cast<unsigned>(o.size()) : threads;
        ctx.chunks.resize(used);
        const std::size_t n = o.size();
        const std::size_t base = n / used;
        const std::size_t rem = n % used;
        ctx.fut.resize(used);
        std::size_t begin = 0;
        for (unsigned t = 0; t < used; ++t) {
          const std::size_t len = base + (t < rem ? 1u : 0u);
          const std::size_t end = begin + len;
          ctx.fut[t] = std::async(std::launch::async, [&, t, begin, end]() {
            std::string buf;
            buf.reserve((end - begin) * 128u);
            for (std::size_t i = begin; i < end; ++i) {
              detail::dump_indent(buf, indent + 2);
              detail::dump_escaped(buf, o[i].first);
              buf.append(": ", 2);
              dump_to(buf, o[i].second, /*pretty=*/true, indent + 2);
              if (i + 1 != o.size()) buf.push_back(',');
              buf.push_back('\n');
            }
            ctx.chunks[t] = std::move(buf);
          });
          begin = end;
        }
        for (unsigned t = 0; t < used; ++t) ctx.fut[t].get();
        for (unsigned t = 0; t < used; ++t) out.append(ctx.chunks[t]);
        detail::dump_indent(out, indent);
        out.push_back('}');
        return;
      }

      dump_to(out, v, /*pretty=*/true, indent);
    }
  };

  mt::dump_value(out, v, /*indent=*/0, /*depth=*/0, opt, threads, ctx);
}

inline std::string dump_mt(const sv_value& v, dump_mt_options opt = {}) {
  std::string out;
  out.reserve(256);
  dump_to_mt(out, v, opt);
  return out;
}

} // namespace chjson
   