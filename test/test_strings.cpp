#include "test_common.hpp"

#include <string>
#include <string_view>

using namespace chjson;

static void test_escape_sequences() {
  {
    auto r = parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_string());
    CHJSON_CHECK(r.doc.root().as_string_view() == std::string_view("\"\\/\b\f\n\r\t"));
  }
}

static void test_unicode_escapes_and_surrogates() {
  {
    auto r = parse("\"\\u4F60\\u597D\"");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_string());
    CHJSON_CHECK(!r.doc.root().as_string_view().empty());
  }
  {
    auto r = parse("\"\\uD83D\\uDE03\"");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_string());
    CHJSON_CHECK(!r.doc.root().as_string_view().empty());
  }
}

static void test_invalid_string_escapes() {
  {
    auto r = parse("\"\\v\"");
    chjson_test::check_err(r.err, error_code::invalid_escape);
  }
  {
    auto r = parse("\"\\u12G4\"");
    chjson_test::check_err(r.err, error_code::invalid_unicode_escape);
  }
  {
    auto r = parse("\"\\uD800\"");
    chjson_test::check_err(r.err, error_code::invalid_utf16_surrogate);
  }
  {
    auto r = parse("\"\\uDE03\"");
    chjson_test::check_err(r.err, error_code::invalid_utf16_surrogate);
  }
}

static void test_unescaped_control_character_rejected() {
  // newline inside JSON string literal is invalid JSON
  auto r = parse("\"a\n\"");
  chjson_test::check_err(r.err, error_code::invalid_string);
}

static void test_dump_escapes_and_roundtrip() {
  value v(std::string("x\n\"y\\z"));
  const std::string s = dump(v);
  auto r = parse(s);
  CHJSON_CHECK(!r.err);
  CHJSON_CHECK(r.doc.root().is_string());
  CHJSON_CHECK(r.doc.root().as_string_view() == std::string_view(v.as_string()));
}

void test_strings() {
  test_escape_sequences();
  test_unicode_escapes_and_surrogates();
  test_invalid_string_escapes();
  test_unescaped_control_character_rejected();
  test_dump_escapes_and_roundtrip();
}
