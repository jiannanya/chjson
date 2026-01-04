#include "test_common.hpp"

#include <string>

using namespace chjson;

static void test_common_syntax_errors() {
  {
    auto r = parse("{\"a\":1,}");
    CHJSON_CHECK(r.err);
    CHJSON_CHECK(r.err.code == error_code::expected_key_string || r.err.code == error_code::expected_comma_or_end);
  }
  {
    auto r = parse("[1 2]");
    chjson_test::check_err(r.err, error_code::expected_comma_or_end);
  }
  {
    auto r = parse("\"unterminated");
    chjson_test::check_err(r.err, error_code::unexpected_eof);
  }
  {
    auto r = parse("01");
    chjson_test::check_err(r.err, error_code::invalid_number);
  }
}

static void test_error_line_column_tracking() {
  // Ensure offset/line/column are populated and plausible.
  const char* json = "{\n  \"a\": 1,\n  \"b\": 01\n}";
  auto r = parse(json);
  chjson_test::check_err(r.err, error_code::invalid_number);
  CHJSON_CHECK(r.err.offset < std::strlen(json));
  CHJSON_CHECK(r.err.line >= 1);
  CHJSON_CHECK(r.err.column >= 1);
  // The bad token is on line 3.
  CHJSON_CHECK(r.err.line == 3);
}

static void test_expected_colon_and_key_string() {
  {
    auto r = parse(R"({"a" 1})");
    chjson_test::check_err(r.err, error_code::expected_colon);
  }
  {
    auto r = parse(R"({a:1})");
    chjson_test::check_err(r.err, error_code::expected_key_string);
  }
}

void test_errors() {
  test_common_syntax_errors();
  test_error_line_column_tracking();
  test_expected_colon_and_key_string();
}
