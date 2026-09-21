#include "test_common.hpp"

#include <string>
#include <string_view>
#include <vector>

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

static void test_programmatic_growth_with_arena_hint() {
  document d;
  auto* mr = d.resource();
  auto* arena = &d.arena();

  // The hinted overloads must behave exactly like the default ones: values are
  // preserved, and growth may be served in place by the arena.
  sv_value arr = sv_value::make_array(mr);
  for (std::int64_t i = 0; i < 9; ++i) arr.array_push_back(mr, sv_value::integer(i), arena);
  const std::uint32_t cap_after_push = arr.u.a.cap;
  arr.array_reserve(mr, cap_after_push + 8, arena);
  CHJSON_CHECK(arr.u.a.cap >= cap_after_push + 8);
  CHJSON_CHECK(arr.as_array().size() == 9);
  for (std::int64_t i = 0; i < 9; ++i) CHJSON_CHECK(arr.as_array()[static_cast<std::size_t>(i)].as_int() == i);

  // Interleave an unrelated arena allocation so the next growth cannot be
  // served in place: the copy path must keep every element intact.
  (void)mr->allocate(32, alignof(std::max_align_t));
  const std::uint32_t before = arr.u.a.cap;
  for (std::int64_t i = 9; i < 64; ++i) arr.array_push_back(mr, sv_value::integer(i), arena);
  CHJSON_CHECK(arr.u.a.cap > before);
  for (std::int64_t i = 0; i < 64; ++i) CHJSON_CHECK(arr.as_array()[static_cast<std::size_t>(i)].as_int() == i);

  sv_value obj = sv_value::make_object(mr);
  // sv_value stores string_views, so keep the key storage alive and stable.
  std::vector<std::string> keys;
  keys.reserve(12);
  for (int i = 0; i < 12; ++i) keys.push_back("k" + std::to_string(i));
  for (int i = 0; i < 12; ++i) obj.object_emplace_back(mr, keys[static_cast<std::size_t>(i)], sv_value::integer(i), arena);
  CHJSON_CHECK(obj.as_object().size() == 12);
  for (int i = 0; i < 12; ++i) {
    const sv_value* v = obj.find(keys[static_cast<std::size_t>(i)]);
    CHJSON_CHECK(v && v->is_int() && v->as_int() == i);
  }

  d.root() = std::move(arr);
  auto r = parse(dump(d.root()));
  CHJSON_CHECK(!r.err && r.doc.root().as_array().size() == 64);
}

void test_dom() {
  test_insitu_string_and_key_views_within_buffer();
  test_view_parse_string_views();
  test_view_document_reuse_clear();
  test_programmatic_growth_with_arena_hint();
}
