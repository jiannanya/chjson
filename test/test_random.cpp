#include "test_common.hpp"

#include <random>
#include <string>
#include <vector>

using namespace chjson;

namespace {

struct rng {
  std::uint64_t s{0x9E3779B97F4A7C15ull};
  std::uint64_t next_u64() {
    // xorshift64*
    std::uint64_t x = s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    s = x;
    return x * 2685821657736338717ull;
  }
  std::uint32_t next_u32() { return static_cast<std::uint32_t>(next_u64() >> 32); }
  std::size_t range(std::size_t n) { return n ? static_cast<std::size_t>(next_u64() % n) : 0u; }
  bool coin() { return (next_u64() & 1ull) != 0; }
};

static std::string random_string(rng& r, std::size_t max_len) {
  const std::size_t len = r.range(max_len + 1);
  std::string out;
  out.reserve(len);
  for (std::size_t i = 0; i < len; ++i) {
    const std::uint32_t pick = r.next_u32() % 16u;
    switch (pick) {
      case 0: out.push_back('\"'); break;
      case 1: out.push_back('\\'); break;
      case 2: out.push_back('\n'); break;
      case 3: out.push_back('\t'); break;
      default: {
        // printable ASCII excluding control chars
        char c = static_cast<char>(' ' + (r.next_u32() % 95u));
        out.push_back(c);
        break;
      }
    }
  }
  return out;
}

static value random_value(rng& r, int depth);

static value random_array(rng& r, int depth) {
  value::array a;
  const std::size_t n = r.range(8);
  a.reserve(n);
  for (std::size_t i = 0; i < n; ++i) a.emplace_back(random_value(r, depth - 1));
  return value(std::move(a));
}

static value random_object(rng& r, int depth) {
  value::object o;
  const std::size_t n = r.range(8);
  o.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    std::string key = random_string(r, 10);
    o.emplace_back(std::move(key), random_value(r, depth - 1));
  }
  return value(std::move(o));
}

static value random_number(rng& r) {
  if (r.coin()) {
    // int
    const std::int64_t v = static_cast<std::int64_t>(static_cast<std::int32_t>(r.next_u32()));
    return value::integer(v);
  }
  // finite double with limited magnitude, to avoid corner formatting cases.
  const double base = static_cast<double>(static_cast<std::int32_t>(r.next_u32() % 2000000u) - 1000000);
  const double d = base / 1000.0;
  return value::number(d);
}

static value random_value(rng& r, int depth) {
  if (depth <= 0) {
    const std::uint32_t k = r.next_u32() % 4u;
    switch (k) {
      case 0: return value(nullptr);
      case 1: return value(r.coin());
      case 2: return random_number(r);
      default: return value(random_string(r, 20));
    }
  }

  const std::uint32_t k = r.next_u32() % 6u;
  switch (k) {
    case 0: return value(nullptr);
    case 1: return value(r.coin());
    case 2: return random_number(r);
    case 3: return value(random_string(r, 20));
    case 4: return random_array(r, depth);
    default: return random_object(r, depth);
  }
}

static void deep_equal(const value& a, const value& b) {
  CHJSON_CHECK(a.type() == b.type());
  switch (a.type()) {
    case value::kind::null: return;
    case value::kind::boolean: CHJSON_CHECK(a.as_bool() == b.as_bool()); return;
    case value::kind::number: {
      if (a.is_int() && b.is_int()) {
        CHJSON_CHECK(a.as_int() == b.as_int());
        return;
      }
      const double da = a.as_double();
      const double db = b.as_double();
      CHJSON_CHECK(std::isfinite(da) && std::isfinite(db));
      CHJSON_CHECK(chjson_test::nearly_equal(da, db, 1e-9, 1e-9));
      return;
    }
    case value::kind::string: CHJSON_CHECK(a.as_string() == b.as_string()); return;
    case value::kind::array: {
      const auto& aa = a.as_array();
      const auto& ab = b.as_array();
      CHJSON_CHECK(aa.size() == ab.size());
      for (std::size_t i = 0; i < aa.size(); ++i) deep_equal(aa[i], ab[i]);
      return;
    }
    case value::kind::object: {
      const auto& oa = a.as_object();
      const auto& ob = b.as_object();
      CHJSON_CHECK(oa.size() == ob.size());
      for (std::size_t i = 0; i < oa.size(); ++i) {
        CHJSON_CHECK(oa[i].first == ob[i].first);
        deep_equal(oa[i].second, ob[i].second);
      }
      return;
    }
  }
}

} // namespace

void test_random() {
  rng r;
  // Deterministic pseudo-fuzz: generate random DOM values, dump, parse, and compare.
  for (int iter = 0; iter < 2000; ++iter) {
    value v = random_value(r, 4);
    const std::string s = dump(v);
    auto pr = parse(s);
    CHJSON_CHECK(!pr.err);
    deep_equal(v, pr.val);

    // Ensure dump output is stable (idempotent under parse/dump).
    const std::string s2 = dump(pr.val);
    auto pr2 = parse(s2);
    CHJSON_CHECK(!pr2.err);
    CHJSON_CHECK(s2 == dump(pr2.val));
  }
}
