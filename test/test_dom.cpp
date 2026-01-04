#include "test_common.hpp"

#include <string>
#include <string_view>

using namespace chjson;

static void test_insitu_string_and_key_views_within_buffer() {
  std::string json = R"({"plain":"ok","esc":"a\nB","u":"\u4F60\u597D","arr":["x","y"],"obj":{"k":"v"}})";
  auto r = parse_in_situ(std::move(json));
  CHJSON_CHECK(!r.err);

  const auto buf = r.doc.buffer();
  const auto* begin = buf.data();
  const auto* end = buf.data() + buf.size();

  const sv_value& root = r.doc.root();
  CHJSON_CHECK(root.is_object());

  // Keys should be string_views pointing into the in-situ buffer.
  for (const auto& kv : root.as_object()) {
    CHJSON_CHECK(!kv.first.empty());
    CHJSON_CHECK(kv.first.data() >= begin && kv.first.data() <= end);
    CHJSON_CHECK(kv.first.data() + kv.first.size() <= end);
  }

  const sv_value* esc = root.find("esc");
  CHJSON_CHECK(esc && esc->is_string());
  const std::string_view esc_sv = esc->as_string_view();
  CHJSON_CHECK(esc_sv == std::string_view("a\nB", 3));
  CHJSON_CHECK(esc_sv.data() >= begin && esc_sv.data() <= end);
  CHJSON_CHECK(esc_sv.data() + esc_sv.size() <= end);

  const auto dumped = dump(root);
  auto r2 = parse(dumped);
  CHJSON_CHECK(!r2.err);
}

static void test_view_parse_string_views() {
  std::string json = R"({"plain":"ok","esc":"a\nB","u":"\u4F60\u597D","k":"\"q\""})";
  auto r = parse_view(json);
  CHJSON_CHECK(!r.err);

  const auto* begin = json.data();
  const auto* end = json.data() + json.size();

  const sv_value& root = r.doc.root();
  CHJSON_CHECK(root.is_object());

  const sv_value* plain = root.find("plain");
  CHJSON_CHECK(plain && plain->is_string());
  const std::string_view plain_sv = plain->as_string_view();
  CHJSON_CHECK(plain_sv == "ok");
  CHJSON_CHECK(plain_sv.data() >= begin && plain_sv.data() <= end);
  CHJSON_CHECK(plain_sv.data() + plain_sv.size() <= end);

  const sv_value* esc = root.find("esc");
  CHJSON_CHECK(esc && esc->is_string());
  const std::string_view esc_sv = esc->as_string_view();
  CHJSON_CHECK(esc_sv == std::string_view("a\nB", 3));
  // Decoded strings cannot point into the original JSON source.
  CHJSON_CHECK(!(esc_sv.data() >= begin && esc_sv.data() < end));

  const sv_value* k = root.find("k");
  CHJSON_CHECK(k && k->is_string());
  const std::string_view k_sv = k->as_string_view();
  CHJSON_CHECK(k_sv == "\"q\"");
  CHJSON_CHECK(!(k_sv.data() >= begin && k_sv.data() < end));
}

static void test_view_document_reuse_clear() {
  view_document doc;
  {
    auto r = parse_view("{\"a\":\"x\"}");
    CHJSON_CHECK(!r.err);
  }

  // Directly exercise view_document APIs.
  doc.clear();
  CHJSON_CHECK(doc.source().empty());
  CHJSON_CHECK(doc.root().is_null());
}

void test_dom() {
  test_insitu_string_and_key_views_within_buffer();
  test_view_parse_string_views();
  test_view_document_reuse_clear();
}
