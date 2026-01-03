#pragma once

// chjson: a small, header-only C++17 JSON library.
// Goals: fast enough in practice, strict JSON by default, high-quality errors.

#include <cassert>
#include <algorithm>
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
#include <utility>
#include <variant>
#include <vector>

namespace chjson {

// Config: floating-point parsing backend.
// Override by defining CHJSON_USE_FROM_CHARS_DOUBLE to 0/1 before including this header.
#ifndef CHJSON_USE_FROM_CHARS_DOUBLE
  #define CHJSON_USE_FROM_CHARS_DOUBLE 0
#endif

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
  nesting_too_deep
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

struct number_value {
  // Keep both: integers preserve round-trip and allow fast integer APIs.
  // If `is_int == false`, use `d`.
  bool is_int{false};
  std::int64_t i{0};
  double d{0.0};
};

struct owned_number_value {
  // For non-integers, keep the original token for fast dump and lazy double parse.
  // If `raw` is empty, use `d`.
  bool is_int{false};
  std::int64_t i{0};
  mutable bool has_double{false};
  mutable double d{0.0};
  std::string raw{};
};

inline double parse_double(std::string_view token) {
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
  return std::strtod(std::string(token).c_str(), nullptr);
}

inline bool parse_number(const char* buf, std::size_t size, std::size_t& i, number_value& out, bool parse_fp = true) {
  // JSON number grammar:
  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  const std::size_t start = i;
  if (i >= size) return false;

  bool neg = false;
  if (buf[i] == '-') {
    neg = true;
    ++i;
    if (i >= size) return false;
  }

  std::uint64_t acc = 0;
  bool overflow = false;

  if (buf[i] == '0') {
    ++i;
    if (i < size && is_digit(buf[i])) return false;
  } else {
    const char c0 = buf[i];
    if (c0 < '1' || c0 > '9') return false;
    acc = static_cast<std::uint64_t>(c0 - '0');
    ++i;
    while (i < size && is_digit(buf[i])) {
      const std::uint64_t d = static_cast<std::uint64_t>(buf[i] - '0');
      if (!overflow) {
        if (acc > (std::numeric_limits<std::uint64_t>::max() - d) / 10u) {
          overflow = true;
        } else {
          acc = acc * 10u + d;
        }
      }
      ++i;
    }
  }

  bool is_int = true;
  if (i < size && buf[i] == '.') {
    is_int = false;
    ++i;
    if (i >= size || !is_digit(buf[i])) return false;
    while (i < size && is_digit(buf[i])) ++i;
  }

  if (i < size && (buf[i] == 'e' || buf[i] == 'E')) {
    is_int = false;
    ++i;
    if (i >= size) return false;
    if (buf[i] == '+' || buf[i] == '-') {
      ++i;
      if (i >= size) return false;
    }
    if (!is_digit(buf[i])) return false;
    while (i < size && is_digit(buf[i])) ++i;
  }

  const std::size_t end = i;
  const std::string_view token(buf + start, end - start);

  if (!is_int) {
    out.is_int = false;
    if (parse_fp) out.d = parse_double(token);
    return true;
  }

  if (overflow) {
    out.is_int = false;
    if (parse_fp) out.d = parse_double(token);
    return true;
  }

  if (neg) {
    const std::uint64_t limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1ull;
    if (acc > limit) {
      out.is_int = false;
      if (parse_fp) out.d = parse_double(token);
      return true;
    }
    out.is_int = true;
    out.i = (acc == limit) ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(acc);
    return true;
  }

  if (acc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    out.is_int = false;
    if (parse_fp) out.d = parse_double(token);
    return true;
  }
  out.is_int = true;
  out.i = static_cast<std::int64_t>(acc);
  return true;
}

inline bool parse_number(const char* buf, std::size_t size, std::size_t& i, owned_number_value& out, bool parse_fp = true) {
  const std::size_t start = i;
  if (i >= size) return false;

  bool neg = false;
  if (buf[i] == '-') {
    neg = true;
    ++i;
    if (i >= size) return false;
  }

  std::uint64_t acc = 0;
  bool overflow = false;

  if (buf[i] == '0') {
    ++i;
    if (i < size && is_digit(buf[i])) return false;
  } else {
    const char c0 = buf[i];
    if (c0 < '1' || c0 > '9') return false;
    acc = static_cast<std::uint64_t>(c0 - '0');
    ++i;
    while (i < size && is_digit(buf[i])) {
      const std::uint64_t d = static_cast<std::uint64_t>(buf[i] - '0');
      if (!overflow) {
        if (acc > (std::numeric_limits<std::uint64_t>::max() - d) / 10u) {
          overflow = true;
        } else {
          acc = acc * 10u + d;
        }
      }
      ++i;
    }
  }

  bool is_int = true;
  if (i < size && buf[i] == '.') {
    is_int = false;
    ++i;
    if (i >= size || !is_digit(buf[i])) return false;
    while (i < size && is_digit(buf[i])) ++i;
  }

  if (i < size && (buf[i] == 'e' || buf[i] == 'E')) {
    is_int = false;
    ++i;
    if (i >= size) return false;
    if (buf[i] == '+' || buf[i] == '-') {
      ++i;
      if (i >= size) return false;
    }
    if (!is_digit(buf[i])) return false;
    while (i < size && is_digit(buf[i])) ++i;
  }

  const std::size_t end = i;
  const std::string_view token(buf + start, end - start);

  auto set_raw = [&]() {
    out.raw.assign(token.data(), token.size());
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
    out.raw.clear();
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
  out.raw.clear();
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
    n.raw.clear();
    value v;
    v.data_ = n;
    return v;
  }

  static value number(double d) {
    detail::owned_number_value n;
    n.is_int = false;
    n.has_double = true;
    n.d = d;
    n.raw.clear();
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
    if (!n.raw.empty()) {
      if (!n.has_double) {
        n.d = detail::parse_double(n.raw);
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

struct parse_result {
  value val;
  error err;
};

struct parser {
  std::string_view s;
  std::size_t i{0};
  parse_options opt;

  parse_result run() {
    parse_result r;
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

inline parse_result parse(std::string_view json, parse_options opt = {}) {
  parser p;
  p.s = json;
  p.opt = opt;
  return p.run();
}

inline value parse_or_throw(std::string_view json, parse_options opt = {}) {
  auto r = parse(json, opt);
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
          } else if (!n.raw.empty()) {
            out.append(n.raw.data(), n.raw.size());
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
  dump_to(out, v, pretty, 0);

  constexpr std::size_t min_hint = 256;
  constexpr std::size_t max_hint = 16u * 1024u * 1024u;
  const std::size_t sz = out.size();
  hint = (sz < min_hint) ? min_hint : (sz > max_hint ? max_hint : sz);
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
    blocks_ = std::move(other.blocks_);
    initial_block_size_ = other.initial_block_size_;
    current_block_ = other.current_block_;
    other.blocks_.clear();
    other.current_block_ = 0;
    return *this;
  }

  ~arena_resource() override { release(); }

  // Clear allocations but keep committed blocks for reuse.
  // This is the typical "bump allocator reset" operation.
  void clear() noexcept {
    for (auto& b : blocks_) b.used = 0;
    current_block_ = 0;
  }

  // Reset to empty (frees all blocks).
  void reset() noexcept { release(); }

  std::size_t blocks() const noexcept { return blocks_.size(); }
  std::size_t bytes_committed() const noexcept {
    std::size_t sum = 0;
    for (const auto& b : blocks_) sum += b.size;
    return sum;
  }
  std::size_t bytes_used() const noexcept {
    std::size_t sum = 0;
    for (const auto& b : blocks_) sum += b.used;
    return sum;
  }

  void release() noexcept {
    for (auto& b : blocks_) {
      ::operator delete(b.ptr);
    }
    blocks_.clear();
    current_block_ = 0;
  }

private:
  struct block {
    std::byte* ptr{nullptr};
    std::size_t size{0};
    std::size_t used{0};
  };

  std::vector<block> blocks_;
  std::size_t initial_block_size_{64 * 1024};
  std::size_t current_block_{0};

  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    if (bytes == 0) bytes = 1;
    if (alignment == 0) alignment = alignof(std::max_align_t);
    if ((alignment & (alignment - 1)) != 0) {
      // alignment must be power-of-two per pmr contract
      throw std::bad_alloc();
    }

    auto try_block = [&](block& b) -> void* {
      std::uintptr_t base = reinterpret_cast<std::uintptr_t>(b.ptr) + b.used;
      std::uintptr_t aligned = (base + (alignment - 1)) & ~(static_cast<std::uintptr_t>(alignment) - 1u);
      const std::size_t padding = static_cast<std::size_t>(aligned - base);
      if (b.used + padding + bytes <= b.size) {
        b.used += padding + bytes;
        return reinterpret_cast<void*>(aligned);
      }
      return nullptr;
    };

    if (!blocks_.empty()) {
      if (current_block_ >= blocks_.size()) current_block_ = blocks_.size() - 1;
      // Try current and subsequent existing blocks first.
      for (std::size_t idx = current_block_; idx < blocks_.size(); ++idx) {
        if (void* p = try_block(blocks_[idx])) {
          current_block_ = idx;
          return p;
        }
      }
    }

    // Need a new block.
    const std::size_t min_block = initial_block_size_ ? initial_block_size_ : (64 * 1024);
    const std::size_t block_size = (bytes + alignment <= min_block) ? min_block : (bytes + alignment);

    block nb;
    nb.size = block_size;
    nb.used = 0;
    nb.ptr = static_cast<std::byte*>(::operator new(nb.size));
    blocks_.push_back(nb);
    current_block_ = blocks_.size() - 1;

    // Try again from the fresh block.
    return do_allocate(bytes, alignment);
  }

  void do_deallocate(void*, std::size_t, std::size_t) override {
    // monotonic: no-op
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }
};

} // namespace pmr

struct sv_number_value {
  bool is_int{false};
  std::int64_t i{0};
  // For non-integers, we prefer to keep the original token for fast parse/dump.
  // If `raw` is empty, use `d`.
  mutable bool has_double{false};
  mutable double d{0.0};
  std::string_view raw{};
};

class sv_value {
public:
  using array = std::pmr::vector<sv_value>;
  using object = std::pmr::vector<std::pair<std::string_view, sv_value>>;

  enum class kind { null, boolean, number, string, array, object };

  sv_value() noexcept : data_(std::monostate{}) {}
  sv_value(std::nullptr_t) noexcept : data_(std::monostate{}) {}
  explicit sv_value(bool b) : data_(b) {}
  explicit sv_value(std::string_view s) : data_(s) {}

  static sv_value integer(std::int64_t i) {
    sv_number_value n;
    n.is_int = true;
    n.i = i;
    sv_value v;
    v.data_ = n;
    return v;
  }

  static sv_value number(double d) {
    sv_number_value n;
    n.is_int = false;
    n.has_double = true;
    n.d = d;
    sv_value v;
    v.data_ = n;
    return v;
  }

  static sv_value number_token(std::string_view token) {
    sv_number_value n;
    n.is_int = false;
    n.raw = token;
    n.has_double = false;
    n.d = 0.0;
    sv_value v;
    v.data_ = n;
    return v;
  }

  static sv_value make_array(std::pmr::memory_resource* mr) {
    sv_value v;
    v.data_ = array{mr};
    return v;
  }

  static sv_value make_object(std::pmr::memory_resource* mr) {
    sv_value v;
    v.data_ = object{mr};
    return v;
  }

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
  bool is_number() const noexcept { return std::holds_alternative<sv_number_value>(data_); }
  bool is_string() const noexcept { return std::holds_alternative<std::string_view>(data_); }
  bool is_array() const noexcept { return std::holds_alternative<array>(data_); }
  bool is_object() const noexcept { return std::holds_alternative<object>(data_); }

  bool as_bool() const { return std::get<bool>(data_); }

  bool is_int() const noexcept {
    if (!is_number()) return false;
    return std::get<sv_number_value>(data_).is_int;
  }

  std::int64_t as_int() const {
    const auto& n = std::get<sv_number_value>(data_);
    if (!n.is_int) throw std::runtime_error("chjson: number is not int");
    return n.i;
  }

  double as_double() const {
    const auto& n = std::get<sv_number_value>(data_);
    if (n.is_int) return static_cast<double>(n.i);
    if (n.raw.data() != nullptr && !n.raw.empty()) {
      if (!n.has_double) {
        n.d = detail::parse_double(n.raw);
        n.has_double = true;
      }
      return n.d;
    }
    return n.d;
  }

  std::string_view as_string_view() const { return std::get<std::string_view>(data_); }
  const array& as_array() const { return std::get<array>(data_); }
  const object& as_object() const { return std::get<object>(data_); }

  array& as_array() { return std::get<array>(data_); }
  object& as_object() { return std::get<object>(data_); }

  const sv_value* find(std::string_view key) const noexcept {
    if (!is_object()) return nullptr;
    const auto& o = std::get<object>(data_);
    for (const auto& kv : o) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

  sv_value* find(std::string_view key) noexcept {
    if (!is_object()) return nullptr;
    auto& o = std::get<object>(data_);
    for (auto& kv : o) {
      if (kv.first == key) return &kv.second;
    }
    return nullptr;
  }

private:
  std::variant<std::monostate, bool, sv_number_value, std::string_view, array, object> data_;

  friend class document;
  friend void dump_to(std::string&, const sv_value&, bool, int);
};

class document {
public:
  document() = default;
  explicit document(std::string json) : buffer_(std::move(json)) {}

  // Reuse this document for another parse. Keeps arena blocks unless you call reset().
  void clear() noexcept {
    root_ = sv_value(nullptr);
    arena_.clear();
  }

  // Replace the underlying buffer content (reusing capacity) and clear arena.
  void assign_buffer(std::string_view json) {
    buffer_.assign(json.data(), json.size());
    clear();
  }

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
    sv_value out = sv_value::make_array(doc->resource());
    auto& a = out.as_array();

    // Heuristic reserve to reduce reallocations in large arrays.
    if (depth <= 1) {
      const std::size_t remaining = (i < size) ? (size - i) : 0;
      const std::size_t guess = remaining / 64u;
      if (guess > 0) a.reserve(std::min<std::size_t>(guess, 4096u));
    } else {
      a.reserve(16);
    }

    if (i < size && buf[i] == ']') {
      ++i;
      return out;
    }

    while (true) {
      sv_value elem = parse_value(depth, e);
      if (e) return nullptr;
      a.emplace_back(std::move(elem));

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
    sv_value out = sv_value::make_object(doc->resource());
    auto& o = out.as_object();

    // Many JSON objects are small and fixed-shape; reserving a few slots helps.
    (void)depth;
    o.reserve(8);

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
      o.emplace_back(key, std::move(v));

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
    sv_value out = sv_value::make_array(doc->resource());
    auto& a = out.as_array();

    // Heuristic reserve to reduce reallocations in large arrays.
    if (depth <= 1) {
      const std::size_t remaining = (i < size) ? (size - i) : 0;
      const std::size_t guess = remaining / 64u;
      if (guess > 0) a.reserve(std::min<std::size_t>(guess, 4096u));
    } else {
      a.reserve(16);
    }

    if (i < size && buf[i] == ']') {
      ++i;
      return out;
    }

    while (true) {
      detail::skip_ws(buf, size, i);
      sv_value elem = parse_value(depth, e);
      if (e) return nullptr;
      a.emplace_back(std::move(elem));

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
    sv_value out = sv_value::make_object(doc->resource());
    auto& o = out.as_object();

    // Many JSON objects are small and fixed-shape; reserving a few slots helps.
    (void)depth;
    o.reserve(8);

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
      o.emplace_back(key, std::move(v));

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

inline document parse_in_situ_or_throw(std::string json, parse_options opt = {}) {
  auto r = parse_in_situ(std::move(json), opt);
  if (r.err) throw std::runtime_error("chjson: parse_in_situ failed");
  return std::move(r.doc);
}

inline void dump_to(std::string& out, const sv_value& v, bool pretty = false, int indent = 0) {
  // Keep the signature but avoid pretty-branching in the hot (compact) path.
  auto dump_number = [&](const sv_number_value& n) {
    if (n.is_int) {
      detail::dump_int64(out, n.i);
      return;
    }
    if (n.raw.data() != nullptr && !n.raw.empty()) {
      out.append(n.raw.data(), n.raw.size());
      return;
    }
    detail::dump_double(out, n.d);
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
          dump_number(std::get<sv_number_value>(cur.data_));
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
      dump_number(std::get<sv_number_value>(v.data_));
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

inline std::string dump(const sv_value& v, bool pretty = false) {
  struct dump_reserve_hints {
    std::size_t compact{256};
    std::size_t pretty{256};
  };
  thread_local dump_reserve_hints hints;

  std::string out;
  std::size_t& hint = pretty ? hints.pretty : hints.compact;
  out.reserve(hint);
  dump_to(out, v, pretty, 0);

  constexpr std::size_t min_hint = 256;
  constexpr std::size_t max_hint = 16u * 1024u * 1024u;
  const std::size_t sz = out.size();
  hint = (sz < min_hint) ? min_hint : (sz > max_hint ? max_hint : sz);
  return out;
}

} // namespace chjson
