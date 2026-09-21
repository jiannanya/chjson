// Differential tests for the portable SWAR scanning primitives and for the
// number/string scanners built on top of them.
//
// The SWAR helpers use cross-lane borrow/carry tricks whose correctness is not
// obvious by inspection. These tests therefore compare every helper against a
// plainly-written byte-at-a-time reference implementation, over random data,
// over every possible byte value, and over every lane alignment.

#include "test_common.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace chjson;

// ---------------------------------------------------------------------------
// Reference (scalar) implementations.
// ---------------------------------------------------------------------------

namespace {

std::size_t ref_scan_string_special(const char* data, std::size_t size, std::size_t pos) {
  while (pos < size) {
    const unsigned char c = static_cast<unsigned char>(data[pos]);
    if (c == '"' || c == '\\' || c < 0x20 || c >= 0x80) break;
    ++pos;
  }
  return pos;
}

std::size_t ref_skip_ws(const char* buf, std::size_t size, std::size_t i) {
  while (i < size) {
    const char c = buf[i];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
    ++i;
  }
  return i;
}

std::size_t ref_scan_digits(const char* buf, std::size_t size, std::size_t p) {
  while (p < size && buf[p] >= '0' && buf[p] <= '9') ++p;
  return p;
}

std::size_t ref_find_first_escape(std::string_view s) {
  for (std::size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"' || c == '\\' || c <= 0x1F) return i;
  }
  return s.size();
}

bool ref_needs_escaping(std::string_view s) { return ref_find_first_escape(s) != s.size(); }

// ---------------------------------------------------------------------------
// 1) Per-lane predicate masks.
// ---------------------------------------------------------------------------

void test_eq_mask_is_exact_per_lane() {
  for (unsigned value = 0; value <= 0xFFu; ++value) {
    const unsigned char needle = static_cast<unsigned char>(value);
    for (int trial = 0; trial < 64; ++trial) {
      std::array<unsigned char, 8> bytes{};
      for (auto& b : bytes) b = static_cast<unsigned char>(trial * 31 + value);
      if (trial % 4 == 0) bytes[static_cast<std::size_t>(trial % 8)] = needle;

      std::uint64_t w = 0;
      std::memcpy(&w, bytes.data(), 8);

      const std::uint64_t mask = chjson::detail::swar_eq(w, needle);
      std::size_t first_true = 8;
      for (std::size_t lane = 0; lane < 8; ++lane) {
        const bool matches = bytes[lane] == needle;
        // Every lane's high bit must exactly report equality.
        const bool bit = ((mask >> (lane * 8 + 7)) & 1u) != 0u;
        CHJSON_CHECK(bit == matches);
        if (matches && first_true == 8) first_true = lane;
      }
      if (first_true == 8) {
        CHJSON_CHECK(mask == 0u);
      } else {
        CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == first_true);
      }
    }
  }
}

void test_lt_mask_is_exact_per_lane() {
  // Uniform words: every lane reports identically.
  for (unsigned n = 1; n <= 128; ++n) {
    const unsigned char bound = static_cast<unsigned char>(n);
    for (int value = 0; value <= 0xFF; ++value) {
      std::array<unsigned char, 8> bytes{};
      for (std::size_t i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(value);
      std::uint64_t w = 0;
      std::memcpy(&w, bytes.data(), 8);

      const std::uint64_t mask = chjson::detail::swar_lt(w, bound);
      if (value < static_cast<int>(n)) {
        CHJSON_CHECK(mask == chjson::detail::swar_high_bits);
        CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == 0);
      } else {
        CHJSON_CHECK(mask == 0u);
      }
    }
  }

  // Mixed lanes: every lane must be individually exact.
  std::mt19937_64 rng(4242);
  for (int trial = 0; trial < 200000; ++trial) {
    std::array<unsigned char, 8> bytes{};
    for (auto& b : bytes) b = static_cast<unsigned char>(rng() & 0xFFu);
    if (trial % 3 == 0) bytes[static_cast<std::size_t>(rng() % 8)] = static_cast<unsigned char>(rng() % 64);

    std::uint64_t w = 0;
    std::memcpy(&w, bytes.data(), 8);

    const unsigned char bound = static_cast<unsigned char>(1 + (rng() % 128));
    const std::uint64_t mask = chjson::detail::swar_lt(w, bound);

    std::size_t expected = 8;
    std::size_t matches = 0;
    for (std::size_t lane = 0; lane < 8; ++lane) {
      const bool truth = bytes[lane] < bound;
      const bool bit = ((mask >> (lane * 8 + 7)) & 1u) != 0u;
      CHJSON_CHECK(bit == truth);
      if (truth) {
        ++matches;
        if (expected == 8) expected = lane;
      }
    }
    CHJSON_CHECK(matches == chjson::detail::swar_popcount64(mask));
    if (expected == 8) {
      CHJSON_CHECK(mask == 0u);
    } else {
      CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == expected);
    }
  }
}

void test_gt_mask_is_exact_per_lane() {
  for (unsigned n = 0; n <= 127; ++n) {
    const unsigned char bound = static_cast<unsigned char>(n);
    for (int value = 0; value <= 0xFF; ++value) {
      std::array<unsigned char, 8> bytes{};
      for (std::size_t i = 0; i < 8; ++i) bytes[i] = static_cast<unsigned char>(value);
      std::uint64_t w = 0;
      std::memcpy(&w, bytes.data(), 8);

      const std::uint64_t mask = chjson::detail::swar_gt(w, bound);
      if (value > static_cast<int>(n)) {
        CHJSON_CHECK(mask == chjson::detail::swar_high_bits);
        CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == 0);
      } else {
        CHJSON_CHECK(mask == 0u);
      }
    }
  }

  std::mt19937_64 rng(9911);
  for (int trial = 0; trial < 200000; ++trial) {
    std::array<unsigned char, 8> bytes{};
    for (auto& b : bytes) b = static_cast<unsigned char>(rng() & 0xFFu);
    if (trial % 3 == 0) bytes[static_cast<std::size_t>(rng() % 8)] = static_cast<unsigned char>(rng() & 0xFFu);

    std::uint64_t w = 0;
    std::memcpy(&w, bytes.data(), 8);

    const unsigned char bound = static_cast<unsigned char>(rng() % 128);
    const std::uint64_t mask = chjson::detail::swar_gt(w, bound);

    std::size_t expected = 8;
    for (std::size_t lane = 0; lane < 8; ++lane) {
      const bool truth = bytes[lane] > bound;
      const bool bit = ((mask >> (lane * 8 + 7)) & 1u) != 0u;
      CHJSON_CHECK(bit == truth);
      if (truth && expected == 8) expected = lane;
    }
    if (expected == 8) {
      CHJSON_CHECK(mask == 0u);
    } else {
      CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == expected);
    }
  }
}

void test_fused_predicates_match_reference() {
  auto is_special_or_utf8 = [](unsigned char c) {
    return c == '"' || c == '\\' || c < 0x20 || c >= 0x80;
  };
  auto needs_escape = [](unsigned char c) { return c == '"' || c == '\\' || c <= 0x1F; };
  auto is_ws_ref = [](unsigned char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };

  std::mt19937_64 rng(20240921);
  for (int trial = 0; trial < 300000; ++trial) {
    std::array<unsigned char, 8> bytes{};
    for (auto& b : bytes) {
      // Concentrate on interesting byte ranges.
      const unsigned pick = static_cast<unsigned>(rng() % 5);
      if (pick == 0) b = static_cast<unsigned char>(rng() % 0x21);
      else if (pick == 1) b = static_cast<unsigned char>(0x7E + (rng() % 4));
      else if (pick == 2) b = '"';
      else if (pick == 3) b = '\\';
      else b = static_cast<unsigned char>(rng() & 0xFFu);
    }
    std::uint64_t w = 0;
    std::memcpy(&w, bytes.data(), 8);

    // --- string scanning predicate (fast variant, lowest-lane contract) ---
    {
      const std::uint64_t mask = chjson::detail::swar_is_string_special_fast(w);
      // Pin the documented composition: '"', '\\', control bytes and UTF-8 bytes.
      CHJSON_CHECK(mask == (chjson::detail::swar_eq_fast(w, '"') |
                            chjson::detail::swar_eq_fast(w, '\\') |
                            chjson::detail::swar_lt_fast(w, 0x20u) |
                            chjson::detail::swar_high_bytes(w)));
      std::size_t expected = 8;
      for (std::size_t lane = 0; lane < 8; ++lane)
        if (is_special_or_utf8(bytes[lane])) {
          expected = lane;
          break;
        }
      if (expected == 8) CHJSON_CHECK(mask == 0u);
      else CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == expected);
    }

    // --- serialization escape predicate (fast variant) ---
    {
      const std::uint64_t mask = chjson::detail::swar_needs_escape_mask(w);
      std::size_t expected = 8;
      for (std::size_t lane = 0; lane < 8; ++lane)
        if (needs_escape(bytes[lane])) {
          expected = lane;
          break;
        }
      if (expected == 8) CHJSON_CHECK(mask == 0u);
      else CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == expected);
    }

    // --- whitespace predicate (exact: the complement must be trustworthy) ---
    {
      const std::uint64_t ws = chjson::detail::swar_is_json_ws_exact(w);
      std::size_t expected = 8;
      for (std::size_t lane = 0; lane < 8; ++lane)
        if (!is_ws_ref(bytes[lane])) {
          expected = lane;
          break;
        }
      const std::uint64_t non_ws = (~ws) & chjson::detail::swar_high_bits;
      if (expected == 8) CHJSON_CHECK(non_ws == 0u);
      else CHJSON_CHECK(chjson::detail::swar_first_byte(non_ws) == expected);

      // Also verify the exact mask lane-by-lane, not just its first match.
      for (std::size_t lane = 0; lane < 8; ++lane) {
        const bool bit = ((ws >> (lane * 8 + 7)) & 1u) != 0u;
        CHJSON_CHECK(bit == is_ws_ref(bytes[lane]));
      }
    }

    // --- digit predicate (fast variant, lowest-lane contract) ---
    {
      const std::uint64_t non_digit = chjson::detail::swar_is_non_digit_fast(w);
      std::size_t expected = 8;
      for (std::size_t lane = 0; lane < 8; ++lane)
        if (!(bytes[lane] >= '0' && bytes[lane] <= '9')) {
          expected = lane;
          break;
        }
      if (expected == 8) CHJSON_CHECK(non_digit == 0u);
      else CHJSON_CHECK(chjson::detail::swar_first_byte(non_digit) == expected);
    }

    // --- flat-array stop/comma predicate (exact: popcount must be trustworthy) ---
    {
      const std::uint64_t stop = chjson::detail::swar_eq(w, '"') | chjson::detail::swar_eq(w, '[') |
                                chjson::detail::swar_eq(w, '{') | chjson::detail::swar_eq(w, '}') |
                                chjson::detail::swar_eq(w, ']');
      for (std::size_t lane = 0; lane < 8; ++lane) {
        const bool truth = bytes[lane] == '"' || bytes[lane] == '[' || bytes[lane] == '{' ||
                           bytes[lane] == '}' || bytes[lane] == ']';
        CHJSON_CHECK((((stop >> (lane * 8 + 7)) & 1u) != 0u) == truth);
      }
      const std::uint64_t commas = chjson::detail::swar_eq(w, ',');
      std::size_t ref_commas = 0;
      for (std::size_t lane = 0; lane < 8; ++lane)
        if (bytes[lane] == ',') ++ref_commas;
      CHJSON_CHECK(chjson::detail::swar_popcount64(commas) == ref_commas);
    }
  }
}

// The reduced-operation masks promise only the lowest-lane contract. Verify that
// promise holds (and that nothing stronger is accidentally assumed) by checking
// that the lowest reported lane is always the first genuine match.
void test_fast_masks_lowest_lane_contract() {
  struct pred_t {
    std::uint64_t (*mask)(std::uint64_t);
    bool (*truth)(unsigned char);
  };
  const pred_t preds[] = {
      {[](std::uint64_t w) { return chjson::detail::swar_eq_fast(w, 'x'); },
       [](unsigned char c) { return c == 'x'; }},
      {[](std::uint64_t w) { return chjson::detail::swar_lt_fast(w, 0x40u); },
       [](unsigned char c) { return c < 0x40; }},
      {[](std::uint64_t w) { return chjson::detail::swar_gt_fast(w, 0x40u); },
       [](unsigned char c) { return c > 0x40; }},
  };

  std::mt19937_64 rng(13579);
  for (int trial = 0; trial < 400000; ++trial) {
    std::array<unsigned char, 8> bytes{};
    for (auto& b : bytes) {
      const unsigned pick = static_cast<unsigned>(rng() % 4);
      if (pick == 0) b = 0x40;                            // exact bound
      else if (pick == 1) b = static_cast<unsigned char>(0x3F + (rng() % 3));
      else if (pick == 2) b = 'x';
      else b = static_cast<unsigned char>(rng() & 0xFFu);
    }
    std::uint64_t w = 0;
    std::memcpy(&w, bytes.data(), 8);

    for (const auto& p : preds) {
      const std::uint64_t mask = p.mask(w);
      std::size_t ref = 8;
      bool any = false;
      for (std::size_t lane = 0; lane < 8; ++lane)
        if (p.truth(bytes[lane])) {
          any = true;
          if (ref == 8) ref = lane;
        }
      if (!any) {
        // No genuine match => no borrow/carry source => no spurious bit at all.
        CHJSON_CHECK(mask == 0u);
      } else {
        CHJSON_CHECK(chjson::detail::swar_first_byte(mask) == ref);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 2) Scanners: byte-for-byte differential testing at every alignment.
// ---------------------------------------------------------------------------

void test_scanners_agree_with_reference() {
  std::mt19937_64 rng(777001);
  const std::size_t kMaxLen = 96;

  for (int trial = 0; trial < 40000; ++trial) {
    const std::size_t len = rng() % (kMaxLen + 1);
    std::string buf(len, 'a');
    for (std::size_t i = 0; i < len; ++i) {
      const unsigned pick = static_cast<unsigned>(rng() % 8);
      switch (pick) {
        case 0: buf[i] = static_cast<char>('a' + (rng() % 26)); break;
        case 1: buf[i] = '"'; break;
        case 2: buf[i] = '\\'; break;
        case 3: buf[i] = static_cast<char>(rng() % 0x20); break;
        case 4: buf[i] = static_cast<char>(0x80 + (rng() % 0x80)); break;
        case 5: buf[i] = ' '; break;
        case 6: buf[i] = static_cast<char>('0' + (rng() % 10)); break;
        default: buf[i] = static_cast<char>(rng() & 0x7F); break;
      }
    }

    // scan_string_special at every possible start offset, including offsets
    // that leave a tail shorter than one word.
    for (std::size_t start = 0; start <= len; ++start) {
      const std::size_t got = chjson::detail::scan_string_special(buf.data(), len, start);
      const std::size_t want = ref_scan_string_special(buf.data(), len, start);
      CHJSON_CHECK(got == want);
    }

    for (std::size_t start = 0; start <= len; ++start) {
      std::size_t got = start;
      chjson::detail::skip_ws(buf.data(), len, got);
      CHJSON_CHECK(got == ref_skip_ws(buf.data(), len, start));
    }

    for (std::size_t start = 0; start <= len; ++start) {
      CHJSON_CHECK(chjson::detail::scan_digits(buf.data(), len, start) == ref_scan_digits(buf.data(), len, start));
    }

    const std::string_view sv(buf.data(), len);
    CHJSON_CHECK(chjson::detail::needs_escaping(sv) == ref_needs_escaping(sv));
    CHJSON_CHECK(chjson::detail::find_first_escape(sv) == ref_find_first_escape(sv));
  }
}

void test_scanners_on_padded_tail_lengths() {
  // Explicitly exercise every length 0..24 with a single interesting byte at
  // every position, which covers the word-loop/scalar-tail boundary.
  for (std::size_t len = 0; len <= 24; ++len) {
    for (std::size_t pos = 0; pos < len; ++pos) {
      for (char special : {'"', '\\', '\n', '\x1F', '\x7F', '\x80'}) {
        std::string buf(len, 'z');
        buf[pos] = special;
        for (std::size_t start = 0; start <= pos; ++start) {
          CHJSON_CHECK(chjson::detail::scan_string_special(buf.data(), len, start) ==
                       ref_scan_string_special(buf.data(), len, start));
          CHJSON_CHECK(chjson::detail::find_first_escape(std::string_view(buf.data(), len)) ==
                       ref_find_first_escape(std::string_view(buf.data(), len)));
        }
      }
      std::string ws(len, 'z');
      ws[pos] = ' ';
      for (std::size_t start = 0; start <= pos; ++start) {
        std::size_t got = start;
        chjson::detail::skip_ws(ws.data(), len, got);
        CHJSON_CHECK(got == ref_skip_ws(ws.data(), len, start));
      }
    }
  }
}

// ---------------------------------------------------------------------------
// 3) Number token scanning: grammar acceptance and values.
// ---------------------------------------------------------------------------

std::string scan_number_token_source(
    const char* token, std::size_t n,
    chjson::detail::number_scan_result& out) {
  out = chjson::detail::scan_number_token(token, n, 0);
  return std::string();
}

void test_number_token_grammar() {
  struct case_t {
    const char* text;
    bool ok;
    bool is_int;
    std::size_t end;
  };
  const case_t cases[] = {
      {"0", true, true, 1},
      {"-0", true, true, 2},
      {"123", true, true, 3},
      {"-123", true, true, 4},
      {"1.5", true, false, 3},
      {"-1.5e10", true, false, 7},
      {"1E+3", true, false, 4},
      {"1e-3", true, false, 4},
      {"0.5", true, false, 3},
      {"01", false, true, 0},
      {"-", false, true, 0},
      {"1.", false, false, 0},
      {".5", false, true, 0},
      {"1e", false, false, 0},
      {"1e+", false, false, 0},
      {"+1", false, true, 0},
      {"1a", true, true, 1},
      {"12,", true, true, 2},
      {"99999999999999999999999999", true, true, 26},
  };

  for (const auto& c : cases) {
    const std::size_t n = std::strlen(c.text);
    chjson::detail::number_scan_result r;
    scan_number_token_source(c.text, n, r);
    CHJSON_CHECK(r.ok == c.ok);
    if (c.ok) {
      CHJSON_CHECK(r.end == c.end);
      CHJSON_CHECK(r.is_int == c.is_int);
    }
  }
}

void test_integer_conversion_boundaries() {
  struct case_t {
    const char* text;
    bool fits_int;
    std::int64_t value;
  };
  const case_t cases[] = {
      {"0", true, 0},
      {"-0", true, 0},
      {"7", true, 7},
      {"12", true, 12},
      {"999999999999999999", true, 999999999999999999LL},          // 18 digits
      {"9223372036854775807", true, (std::numeric_limits<std::int64_t>::max)()},
      {"-9223372036854775808", true, (std::numeric_limits<std::int64_t>::min)()},
      {"9223372036854775808", false, 0},
      {"-9223372036854775809", false, 0},
      {"9999999999999999999", false, 0},  // 19 digits, > int64 max
      {"10000000000000000000", false, 0},
      {"123456789012345678901234567890", false, 0},
      {"-123456789012345678901234567890", false, 0},
  };

  for (const auto& c : cases) {
    // Direct helper check.
    const std::size_t n = std::strlen(c.text);
    bool neg = false;
    std::size_t begin = 0;
    if (c.text[0] == '-') {
      neg = true;
      begin = 1;
    }
    std::int64_t value = 0;
    const bool fits = chjson::detail::convert_integer_token(c.text, begin, n, neg, value);
    CHJSON_CHECK(fits == c.fits_int);
    if (fits) CHJSON_CHECK(value == c.value);

    // End-to-end check through the public API.
    auto r = parse(c.text);
    CHJSON_CHECK(!r.err);
    const auto& root = r.doc.root();
    CHJSON_CHECK(root.is_number());
    if (c.fits_int) {
      CHJSON_CHECK(root.is_int());
      CHJSON_CHECK(root.as_int() == c.value);
      // Integer JSON is canonicalized on dump; "-0" is intentionally "0".
      const std::string expected = (std::strcmp(c.text, "-0") == 0) ? "0" : c.text;
      CHJSON_CHECK(dump(root) == expected);
    } else {
      // Out-of-range integers degrade to doubles, and the raw token is kept.
      CHJSON_CHECK(!root.is_int());
      CHJSON_CHECK(root.as_double() == std::strtod(c.text, nullptr));
    }
  }
}

void test_integer_conversion_exhaustive_lengths() {
  // All-digit tokens of every length 1..21, for a range of digit patterns.
  std::mt19937_64 rng(31415926);
  for (std::size_t len = 1; len <= 21; ++len) {
    for (int pattern = 0; pattern < 40; ++pattern) {
      std::string digits(len, '0');
      for (std::size_t i = 0; i < len; ++i) {
        unsigned char d;
        switch (pattern % 5) {
          case 0: d = 9; break;
          case 1: d = 0; break;
          case 2: d = (i == 0) ? 1 : 0; break;
          case 3: d = (i == 0) ? 9 : 0; break;
          default: d = static_cast<unsigned char>(rng() % 10); break;
        }
        digits[i] = static_cast<char>('0' + d);
      }
      if (digits[0] == '0' && len > 1) digits[0] = '1';

      for (int sign = 0; sign < 2; ++sign) {
        std::string text = (sign ? "-" : "") + digits;
        const std::size_t n = text.size();
        std::int64_t value = 0;
        const bool fits = chjson::detail::convert_integer_token(text.data(), sign ? 1u : 0u, n, sign != 0, value);

        // Reference: compare with strtoll and an explicit range test.
        errno = 0;
        char* endp = nullptr;
        const long long ref = std::strtoll(text.c_str(), &endp, 10);
        const bool ref_fits = (errno != ERANGE);
        CHJSON_CHECK(fits == ref_fits);
        if (fits) CHJSON_CHECK(value == static_cast<std::int64_t>(ref));

        auto r = parse(text);
        CHJSON_CHECK(!r.err);
        CHJSON_CHECK(r.doc.root().is_int() == fits);
        if (fits) CHJSON_CHECK(r.doc.root().as_int() == value);
        // "-0" is canonicalized to "0" by the integer representation.
        CHJSON_CHECK(dump(r.doc.root()) == ((sign && digits == "0") ? std::string("0") : text));
      }
    }
  }
}

void test_long_number_runs_parse_identically() {
  // Long integers/floats exercise the word-at-a-time digit scanner several times.
  for (std::size_t extra = 0; extra <= 64; ++extra) {
    std::string digits = "1";
    digits.append(extra, '3');
    for (const std::string& text : {digits, "-" + digits, std::string("0.") + digits,
                                   digits + ".5", digits + "e" + digits, digits + "E-" + digits}) {
      const std::string doc = "[" + text + "]";
      auto r = parse(doc);
      CHJSON_CHECK(!r.err);
      const auto& elem = r.doc.root().as_array()[0];
      CHJSON_CHECK(elem.is_number());
      CHJSON_CHECK(elem.as_double() == std::strtod(text.c_str(), nullptr));
      CHJSON_CHECK(dump(r.doc.root()) == "[" + text + "]");
    }
  }
}

// swar_first_byte() resolves the lane -> memory-offset mapping with either a
// count-trailing-zeros (little-endian) or a count-leading-zeros (big-endian).
// Only one of the two branches ever runs on a given host, so reproduce the
// big-endian branch's arithmetic here: compose words in big-endian lane order
// (lane k occupies bits 8*(7-k)..) and check that `clz >> 3` yields the *memory*
// offset of the first matching byte. This is the exact expression the big-endian
// branch uses; reversing the lanes there would fail this test everywhere.
void test_first_byte_big_endian_layout() {
  std::mt19937_64 rng(987654);
  for (int trial = 0; trial < 100000; ++trial) {
    std::array<unsigned char, 8> bytes{};
    for (auto& b : bytes) b = static_cast<unsigned char>(rng() & 0xFFu);
    if (trial % 2 == 0) bytes[static_cast<std::size_t>(rng() % 8)] = 'x';

    std::uint64_t be = 0;
    for (std::size_t k = 0; k < 8; ++k) be = (be << 8) | bytes[k];

    const std::uint64_t mask = chjson::detail::swar_eq(be, 'x');
    std::size_t want = 8;
    for (std::size_t k = 0; k < 8; ++k)
      if (bytes[k] == 'x') {
        want = k;
        break;
      }

    if (want == 8) {
      CHJSON_CHECK(mask == 0u);
    } else {
      CHJSON_CHECK((chjson::detail::swar_clz64(mask) >> 3) == want);
    }
  }
}

} // namespace

void test_swar() {
  test_eq_mask_is_exact_per_lane();
  test_lt_mask_is_exact_per_lane();
  test_gt_mask_is_exact_per_lane();
  test_fused_predicates_match_reference();
  test_fast_masks_lowest_lane_contract();
  test_first_byte_big_endian_layout();
  test_scanners_agree_with_reference();
  test_scanners_on_padded_tail_lengths();
  test_number_token_grammar();
  test_integer_conversion_boundaries();
  test_integer_conversion_exhaustive_lengths();
  test_long_number_runs_parse_identically();
}
