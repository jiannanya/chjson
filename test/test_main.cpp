#include <chjson/chjson.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

static void test_primitives() {
  using namespace chjson;

  {
    auto r = parse("null");
    assert(!r.err);
    assert(r.val.is_null());
  }
  {
    auto r = parse("true");
    assert(!r.err);
    assert(r.val.is_bool());
    assert(r.val.as_bool() == true);
  }
  {
    auto r = parse("false");
    assert(!r.err);
    assert(r.val.is_bool());
    assert(r.val.as_bool() == false);
  }
  {
    auto r = parse("123");
    assert(!r.err);
    assert(r.val.is_number());
    assert(r.val.is_int());
    assert(r.val.as_int() == 123);
  }
  {
    auto r = parse("-9223372036854775808");
    assert(!r.err);
    assert(r.val.is_number());
    assert(r.val.is_int());
    assert(r.val.as_int() == (std::numeric_limits<std::int64_t>::min)());
  }
  {
    auto r = parse("1.25");
    assert(!r.err);
    assert(r.val.is_number());
    assert(!r.val.is_int());
    assert(std::fabs(r.val.as_double() - 1.25) < 1e-12);
  }
}

static void test_string_escapes() {
  using namespace chjson;

  {
    auto r = parse("\"hello\\nworld\"");
    assert(!r.err);
    assert(r.val.is_string());
    assert(r.val.as_string() == std::string("hello\nworld"));
  }
  {
    auto r = parse("\"\\u4F60\\u597D\""); // 你好
    assert(!r.err);
    assert(r.val.is_string());
    assert(!r.val.as_string().empty());
  }
  {
    auto r = parse("\"\\uD83D\\uDE03\""); // 😃
    assert(!r.err);
    assert(r.val.is_string());
    assert(!r.val.as_string().empty());
  }
}

static void test_array_object() {
  using namespace chjson;

  const char* json = R"(
  {
    "a": [1, 2, 3],
    "b": {"x": true, "y": null},
    "s": "ok"
  }
  )";

  auto r = parse(json);
  assert(!r.err);
  assert(r.val.is_object());

  const value* a = r.val.find("a");
  assert(a && a->is_array());
  assert(a->as_array().size() == 3);
  assert(a->as_array()[0].as_int() == 1);

  const value* b = r.val.find("b");
  assert(b && b->is_object());
  const value* x = b->find("x");
  const value* y = b->find("y");
  assert(x && x->as_bool() == true);
  assert(y && y->is_null());
}

static void test_roundtrip_dump() {
  using namespace chjson;

  const char* json = R"({"k":"v","n":42,"a":[true,false,null,3.5,0.1,1e-10]})";
  auto r = parse(json);
  assert(!r.err);
  auto out = dump(r.val);

  auto r2 = parse(out);
  assert(!r2.err);
  assert(dump(r2.val) == out);
}

static void test_errors() {
  using namespace chjson;

  {
    auto r = parse("{\"a\":1,}");
    assert(r.err);
  }
  {
    auto r = parse("[1 2]");
    assert(r.err);
  }
  {
    auto r = parse("\"unterminated");
    assert(r.err);
  }
  {
    auto r = parse("01");
    assert(r.err);
  }
}

static void test_insitu_zero_copy() {
  using namespace chjson;

  std::string json = R"({"s":"a\nB","u":"\u4F60\u597D","arr":["x","y"],"obj":{"k":"v"}})";
  auto r = parse_in_situ(std::move(json));
  assert(!r.err);

  const auto buf = r.doc.buffer();
  const auto* begin = buf.data();
  const auto* end = buf.data() + buf.size();

  const sv_value& root = r.doc.root();
  assert(root.is_object());

  const sv_value* s = root.find("s");
  assert(s && s->is_string());
  const std::string_view sv = s->as_string_view();
  assert(sv == std::string_view("a\nB", 3));
  assert(sv.data() >= begin && sv.data() <= end);
  assert(sv.data() + sv.size() <= end);

  const sv_value* u = root.find("u");
  assert(u && u->is_string());
  const std::string_view uv = u->as_string_view();
  assert(!uv.empty());
  assert(uv.data() >= begin && uv.data() <= end);
  assert(uv.data() + uv.size() <= end);

  // Also verify dump works on sv_value.
  const auto dumped = dump(root);
  auto r2 = parse(dumped);
  assert(!r2.err);
}

static void test_view_parse_zero_copy() {
  using namespace chjson;

  std::string json = R"({"plain":"ok","esc":"a\nB","u":"\u4F60\u597D","n":123})";
  auto r = parse_view(json);
  assert(!r.err);

  const auto* begin = json.data();
  const auto* end = json.data() + json.size();

  const sv_value& root = r.doc.root();
  assert(root.is_object());

  const sv_value* plain = root.find("plain");
  assert(plain && plain->is_string());
  const std::string_view plain_sv = plain->as_string_view();
  assert(plain_sv == "ok");
  // Unescaped strings should be referenced directly from the input.
  assert(plain_sv.data() >= begin && plain_sv.data() <= end);
  assert(plain_sv.data() + plain_sv.size() <= end);

  const sv_value* esc = root.find("esc");
  assert(esc && esc->is_string());
  const std::string_view esc_sv = esc->as_string_view();
  assert(esc_sv == std::string_view("a\nB", 3));
  // Decoded strings cannot point into the original JSON text (it does not contain raw newlines inside strings).
  assert(!(esc_sv.data() >= begin && esc_sv.data() < end));

  const sv_value* u = root.find("u");
  assert(u && u->is_string());
  assert(!u->as_string_view().empty());
}

int main() {
  test_primitives();
  test_string_escapes();
  test_array_object();
  test_roundtrip_dump();
  test_errors();
  test_insitu_zero_copy();
  test_view_parse_zero_copy();

  std::cout << "chjson tests passed\n";
  return 0;
}
