#include "test_common.hpp"

#include <string>

using namespace chjson;

static void test_primitives_and_whitespace() {
  {
    auto r = parse(" \t\r\nnull\n\t ");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_null());
  }
  {
    auto r = parse(" true ");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_bool());
    CHJSON_CHECK(r.doc.root().as_bool() == true);
  }
  {
    auto r = parse("false");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_bool());
    CHJSON_CHECK(r.doc.root().as_bool() == false);
  }
}

static void test_arrays_objects_and_order() {
  const char* json = R"(
  {
    "a": [1, 2, 3],
    "b": {"x": true, "y": null},
    "s": "ok"
  }
  )";

  auto r = parse(json);
  CHJSON_CHECK(!r.err);
  CHJSON_CHECK(r.doc.root().is_object());

  const sv_value* a = r.doc.root().find("a");
  CHJSON_CHECK(a && a->is_array());
  CHJSON_CHECK(a->as_array().size() == 3);
  CHJSON_CHECK(a->as_array()[0].as_int() == 1);

  const sv_value* b = r.doc.root().find("b");
  CHJSON_CHECK(b && b->is_object());
  const sv_value* x = b->find("x");
  const sv_value* y = b->find("y");
  CHJSON_CHECK(x && x->as_bool() == true);
  CHJSON_CHECK(y && y->is_null());

  // Insertion order is preserved in dump.
  const std::string dumped = dump(r.doc.root());
  CHJSON_CHECK(dumped.find("\"a\"") < dumped.find("\"b\""));
  CHJSON_CHECK(dumped.find("\"b\"") < dumped.find("\"s\""));
}

static void test_duplicate_keys_find_first() {
  auto r = parse(R"({"k":1,"k":2})");
  CHJSON_CHECK(!r.err);
  const sv_value* k = r.doc.root().find("k");
  CHJSON_CHECK(k != nullptr);
  CHJSON_CHECK(k->is_int());
  CHJSON_CHECK(k->as_int() == 1);
}

static void test_roundtrip_dump_pretty_and_compact() {
  const char* json = R"({"k":"v","n":42,"a":[true,false,null,3.5,0.1,1e-10],"o":{}})";
  auto r = parse(json);
  CHJSON_CHECK(!r.err);

  const auto compact = dump(r.doc.root(), /*pretty=*/false);
  const auto pretty = dump(r.doc.root(), /*pretty=*/true);
  CHJSON_CHECK(!compact.empty());
  CHJSON_CHECK(!pretty.empty());
  CHJSON_CHECK(pretty.find('\n') != std::string::npos);

  auto r2 = parse(compact);
  CHJSON_CHECK(!r2.err);
  CHJSON_CHECK(dump(r2.doc.root()) == compact);

  auto r3 = parse(pretty);
  CHJSON_CHECK(!r3.err);
}

static void test_require_eof_option() {
  {
    auto r = parse("true 123");
    chjson_test::check_err(r.err, error_code::trailing_characters);
  }
  {
    parse_options opt;
    opt.require_eof = false;
    auto r = parse("true 123", opt);
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_bool());
    CHJSON_CHECK(r.doc.root().as_bool() == true);
  }
}

static void test_max_depth_option() {
  parse_options opt;
  opt.max_depth = 2;
  auto r = parse("[[[0]]]", opt);
  chjson_test::check_err(r.err, error_code::nesting_too_deep);
}

static void test_more_cases() {
  // 1) Empty array
  {
    auto r = parse("[]");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_array());
    CHJSON_CHECK(r.doc.root().as_array().empty());
    CHJSON_CHECK(dump(r.doc.root()) == "[]");
  }

  // 2) Empty object
  {
    auto r = parse("{}\n");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_object());
    CHJSON_CHECK(r.doc.root().as_object().empty());
    CHJSON_CHECK(dump(r.doc.root()) == "{}");
  }

  // 3) Mixed empty containers + find
  {
    auto r = parse(R"({"a":[],"b":{}})");
    CHJSON_CHECK(!r.err);
    const sv_value* a = r.doc.root().find("a");
    const sv_value* b = r.doc.root().find("b");
    CHJSON_CHECK(a && a->is_array() && a->as_array().empty());
    CHJSON_CHECK(b && b->is_object() && b->as_object().empty());
  }

  // 4) Array with lots of whitespace
  {
    auto r = parse("[\n\t 1 \r\n ,\t2, 3\n]");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_array());
    CHJSON_CHECK(r.doc.root().as_array().size() == 3);
    CHJSON_CHECK(r.doc.root().as_array()[2].as_int() == 3);
  }

  // 5) Unexpected EOF: object not closed
  {
    auto r = parse("{\"a\":1");
    chjson_test::check_err(r.err, error_code::unexpected_eof);
  }

  // 6) Invalid value at top-level
  {
    auto r = parse("]");
    chjson_test::check_err(r.err, error_code::invalid_value);
  }

  // 7) Trailing characters after literal
  {
    auto r = parse("nullx");
    chjson_test::check_err(r.err, error_code::trailing_characters);
  }

  // 8) Trailing comma in array should fail
  {
    auto r = parse("[1,]");
    chjson_test::check_err(r.err, error_code::invalid_value);
  }

  // 9) Missing comma between object members
  {
    auto r = parse(R"({"a":1 "b":2})");
    chjson_test::check_err(r.err, error_code::expected_comma_or_end);
  }

  // 10) Embedded NUL via \u0000 survives roundtrip
  {
    auto r = parse("\"\\u0000\"");
    CHJSON_CHECK(!r.err);
    CHJSON_CHECK(r.doc.root().is_string());
    CHJSON_CHECK(r.doc.root().as_string_view().size() == 1);
    CHJSON_CHECK(static_cast<unsigned char>(r.doc.root().as_string_view()[0]) == 0u);
    const std::string s = dump(r.doc.root());
    auto r2 = parse(s);
    CHJSON_CHECK(!r2.err);
    CHJSON_CHECK(r2.doc.root().is_string());
    CHJSON_CHECK(r2.doc.root().as_string_view() == r.doc.root().as_string_view());
  }
}

void test_structure() {
  test_primitives_and_whitespace();
  test_arrays_objects_and_order();
  test_duplicate_keys_find_first();
  test_roundtrip_dump_pretty_and_compact();
  test_require_eof_option();
  test_max_depth_option();
  test_more_cases();
}
