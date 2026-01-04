#include "test_common.hpp"

#include <limits>
#include <string>

using namespace chjson;

static void test_integer_boundaries() {
  {
    auto r = parse("0");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.val.is_number());
    CHJSON_CHECK(r.val.is_int());
    CHJSON_CHECK(r.val.as_int() == 0);
  }
  {
    auto r = parse("-0");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.val.is_number());
    CHJSON_CHECK(r.val.is_int());
    CHJSON_CHECK(r.val.as_int() == 0);
    // Note: negative-zero is canonicalized for integer storage.
    CHJSON_CHECK(dump(r.val) == "0");
  }
  {
    auto r = parse("9223372036854775807");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.val.is_int());
    CHJSON_CHECK(r.val.as_int() == (std::numeric_limits<std::int64_t>::max)());
  }
  {
    auto r = parse("-9223372036854775808");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.val.is_int());
    CHJSON_CHECK(r.val.as_int() == (std::numeric_limits<std::int64_t>::min)());
  }
  {
    // One above int64 max: should parse as non-int number token (raw preserved).
    const std::string token = "9223372036854775808";
    auto r = parse(token);
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.val.is_number());
    CHJSON_CHECK(!r.val.is_int());
    CHJSON_CHECK(dump(r.val) == token);
    (void)r.val.as_double();
  }
}

static void test_fraction_and_exponent_tokens_preserved() {
  const char* cases[] = {
      "1.25",
      "0.0",
      "10.000",
      "1e10",
      "1E+10",
      "1e-10",
      "-3.14159e-2",
  };
  for (const char* token : cases) {
    auto r = parse(token);
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.val.is_number());
    CHJSON_CHECK(!r.val.is_int());
    CHJSON_CHECK(dump(r.val) == std::string(token));
    (void)r.val.as_double();
  }
}

static void test_invalid_numbers() {
  const char* bad[] = {
      "",
      "-",
      "+1",
      "00",
      "01",
      "-01",
      "1.",
      ".1",
      "1e",
      "1e+",
      "1e-",
      "--1",
      "0x10",
      "NaN",
      "Infinity",
  };
  for (const char* s : bad) {
    auto r = parse(s);
    CHJSON_CHECK(r.err);
  }
}

static void test_programmatic_nan_inf_dump_throws() {
  {
    value v = value::number(std::numeric_limits<double>::quiet_NaN());
    CHJSON_EXPECT_THROW(dump(v));
  }
  {
    value v = value::number(std::numeric_limits<double>::infinity());
    CHJSON_EXPECT_THROW(dump(v));
  }
}

void test_numbers() {
  test_integer_boundaries();
  test_fraction_and_exponent_tokens_preserved();
  test_invalid_numbers();
  test_programmatic_nan_inf_dump_throws();
}
