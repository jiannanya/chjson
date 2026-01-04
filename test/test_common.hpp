#pragma once

#include <chjson/chjson.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace chjson_test {

[[noreturn]] inline void fail(const char* expr, const char* file, int line, const char* msg = nullptr) {
  std::cerr << "TEST FAILED: " << (expr ? expr : "") << "\n  at " << file << ":" << line;
  if (msg && *msg) std::cerr << "\n  " << msg;
  std::cerr << "\n";
  std::abort();
}

inline void check(bool ok, const char* expr, const char* file, int line) {
  if (!ok) fail(expr, file, line);
}

template <class Fn>
inline void expect_throw(Fn&& fn, const char* expr, const char* file, int line) {
  try {
    fn();
  } catch (...) {
    return;
  }
  fail(expr, file, line, "expected exception, got none");
}

inline bool nearly_equal(double a, double b, double abs_eps = 1e-12, double rel_eps = 1e-12) {
  const double diff = std::fabs(a - b);
  if (diff <= abs_eps) return true;
  const double aa = std::fabs(a);
  const double bb = std::fabs(b);
  const double m = (aa > bb) ? aa : bb;
  return diff <= rel_eps * m;
}

} // namespace chjson_test

#define CHJSON_CHECK(expr) ::chjson_test::check(!!(expr), #expr, __FILE__, __LINE__)
#define CHJSON_EXPECT_THROW(expr) ::chjson_test::expect_throw([&] { (void)(expr); }, #expr, __FILE__, __LINE__)

namespace chjson_test {

inline void check_err(const chjson::error& e, chjson::error_code code) {
  ::chjson_test::check(static_cast<bool>(e), "static_cast<bool>(e)", __FILE__, __LINE__);
  ::chjson_test::check(e.code == code, "e.code == code", __FILE__, __LINE__);
}

} // namespace chjson_test
