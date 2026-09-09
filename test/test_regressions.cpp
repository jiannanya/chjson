#include "test_common.hpp"

#include <clocale>
#include <future>
#include <thread>
#include <vector>

using namespace chjson;

static void check_modes(std::string_view json, bool valid, parse_options opt = {}) {
  auto dom = parse(json, opt);
  auto view = parse_view(json, opt);
  auto owning = parse_owning_view(json, opt);
  auto insitu = parse_in_situ(std::string(json), opt);
  auto legacy = parse_value(json, opt);
  if (static_cast<bool>(dom.err) == valid || static_cast<bool>(view.err) == valid ||
      static_cast<bool>(owning.err) == valid || static_cast<bool>(insitu.err) == valid ||
      static_cast<bool>(legacy.err) == valid) {
    std::cerr << "mode mismatch for " << json << " codes=" << int(dom.err.code) << ','
              << int(view.err.code) << ',' << int(owning.err.code) << ',' << int(insitu.err.code)
              << ',' << int(legacy.err.code) << '\n';
    CHJSON_CHECK(false);
  }
  if (valid) {
    const auto canonical = dump(legacy.val);
    CHJSON_CHECK(dump(dom.doc.root()) == canonical);
    CHJSON_CHECK(dump(view.doc.root()) == canonical);
    CHJSON_CHECK(dump(owning.doc.root()) == canonical);
    CHJSON_CHECK(dump(insitu.doc.root()) == canonical);
  }
}

static void test_utf8_and_boundaries() {
  const std::vector<std::string> valid = {"\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80",
      "\xED\x9F\xBF", "\xEF\xBF\xBF", "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"};
  const std::vector<std::string> invalid = {"\x80", "\xC0\x80", "\xC1\xBF", "\xC2",
      "\xE0\x80\x80", "\xED\xA0\x80", "\xF0\x80\x80\x80", "\xF4\x90\x80\x80",
      "\xF5\x80\x80\x80", "\xFF", "\xE2\x82", "\xC2\x20"};
  for (std::size_t prefix = 0; prefix != 33; ++prefix) {
    const std::string padding(prefix, 'a');
    for (const auto& bytes : valid) {
      check_modes('"' + padding + bytes + '"', true);
      check_modes("\"\\n" + padding + bytes + "\\t\"", true);
      check_modes("{\"" + padding + bytes + "\":0}", true);
    }
    for (const auto& bytes : invalid) {
      check_modes('"' + padding + bytes + '"', false);
      check_modes("\"\\n" + padding + bytes + '"', false);
      check_modes("{\"" + padding + bytes + "\":0}", false);
    }
    // No readable padding or terminating NUL is part of the supplied view.
    auto text = std::make_unique<char[]>(prefix + 1);
    text[0] = '"';
    std::fill_n(text.get() + 1, prefix, 'x');
    check_modes(std::string_view(text.get(), prefix + 1), false);
  }
  check_modes(std::string_view{}, false);
  check_modes("\"ok\" \xFF", true, {256, false});
}

static void test_document_moves_and_reuse() {
  CHJSON_CHECK(sv_value("text").is_string());
  CHJSON_CHECK(sv_value("text").as_string_view() == "text");
  CHJSON_CHECK(sv_value(nullptr).is_null());
  for (const char* json : {"\"x\"", "{\"k\":\"v\"}", "[\"a\",1.25]", "[1,2,3]"}) {
    auto parsed = parse_in_situ(std::string(json));
    CHJSON_CHECK(!parsed.err);
    const std::string expected = dump(parsed.doc.root());
    document moved(std::move(parsed.doc));
    CHJSON_CHECK(parsed.doc.root().is_null());
    parsed.doc.assign_buffer(std::string(200, 'z'));
    CHJSON_CHECK(dump(moved.root()) == expected);
    document target;
    target = std::move(moved);
    moved.assign_buffer(std::string(200, 'q'));
    CHJSON_CHECK(dump(target.root()) == expected);
    CHJSON_CHECK(!parse_in_situ_into(target, target.buffer()));
    CHJSON_CHECK(dump(target.root()) == expected);
    target.reset();
    CHJSON_CHECK(target.root().is_null() && target.buffer().empty());
    CHJSON_CHECK(target.arena().bytes_committed() == 0);
    CHJSON_CHECK(!parse_in_situ_into(target, "[true]"));
  }
  std::vector<document> documents;
  for (int i = 0; i < 100; ++i) documents.push_back(parse_or_throw("{\"k\":\"v\"}"));
  release_thread_caches();
  for (const auto& doc : documents) CHJSON_CHECK(doc.root().find("k")->as_string_view() == "v");
}

static void test_memory_and_capacity() {
  release_thread_caches();
  for (const char* text : {"null", "42", "[]", "{}"}) {
    auto result = parse(text);
    CHJSON_CHECK(!result.err);
    CHJSON_CHECK(result.doc.arena().bytes_used() == 0);
  }
  view_document doc;
  CHJSON_CHECK(!parse_view_into(doc, "[1,2,3,4]"));
  CHJSON_CHECK(doc.arena().bytes_committed() <= 4096);
  doc.clear();
  auto array = sv_value::make_array(doc.resource());
  array.array_reserve(doc.resource(), 4);
  auto* before = array.u.a.data;
  array.array_reserve(doc.resource(), 16);
  CHJSON_CHECK(before == array.u.a.data);
  for (int i = 0; i != 4096; ++i) array.array_push_back(doc.resource(), sv_value::integer(i));
  for (int i = 0; i != 4096; ++i) CHJSON_CHECK(array.as_array()[i].as_int() == i);
  CHJSON_CHECK(sv_value::make_array(doc.resource()).as_array().begin() ==
               sv_value::make_array(doc.resource()).as_array().end());
  CHJSON_EXPECT_THROW(doc.resource()->allocate((std::numeric_limits<std::size_t>::max)(), 64));
  CHJSON_EXPECT_THROW(detail::chjson_allocate_n<sv_value>((std::numeric_limits<std::size_t>::max)()));
  CHJSON_EXPECT_THROW(detail::chjson_allocate((std::numeric_limits<std::size_t>::max)(), 64));
  auto large = parse(std::string("[\"") + std::string(1024 * 1024, 'x') + "\"]");
  CHJSON_CHECK(!large.err);
  CHJSON_CHECK(dump(large.doc.root()).size() > 1024 * 1024);
  const auto small = dump(sv_value(nullptr));
  CHJSON_CHECK(small == "null" && small.capacity() < 1024);
  release_thread_caches();
}

static void test_numeric_modes_and_locale() {
  for (const char* text : {"[1.00,1E+10,9223372036854775808,1e9999,1e-9999]",
                           "[0,1.2300,{\"n\":10.00}]", "{\"n\":-0.0}"}) check_modes(text, true);
  const std::string original = std::setlocale(LC_NUMERIC, nullptr);
  bool selected = false;
  for (const char* locale : {"de_DE.UTF-8", "German_Germany.1252", "de-DE", "fr_FR.UTF-8"}) {
    if (std::setlocale(LC_NUMERIC, locale)) { selected = true; break; }
  }
  if (selected) {
    CHJSON_CHECK(detail::parse_double("1.25") == 1.25);
    CHJSON_CHECK(detail::strtod_c_locale("1.25") == 1.25);
    CHJSON_CHECK(std::isinf(detail::parse_double("1e9999")));
    CHJSON_CHECK(detail::parse_double("1e-9999") == 0.0);
    CHJSON_CHECK(dump(value::number(1.25)) == "1.25");
  }
  std::setlocale(LC_NUMERIC, original.c_str());
  for (std::size_t depth = 0; depth != 5; ++depth) {
    check_modes("0", depth > 0, {depth, true});
    check_modes("[0]", depth > 1, {depth, true});
    check_modes("[[0]]", depth > 2, {depth, true});
    check_modes("{\"a\":0}", depth > 1, {depth, true});
  }
}

static void test_original_error_positions() {
  for (const char* text : {"{\"a\":\"\\n\",\"b\":01}", "[\"\\u000Aabc\",\n01]",
                           "[\"\\n\\u4F60\\u597D\",\n01]", "[\"\\nabc\\v\"]",
                           "[\"\\nabc\\u12G4\"]", "[\"\\nabc\n\"]",
                           "\"\\nabc", "\n\"\\u000Aabc", "\n  \"\\n\\n\\u000Aabc",
                           "\"\\nabc\\"}) {
    const auto reference = parse_view(text).err;
    const auto insitu = parse_in_situ(std::string(text)).err;
    CHJSON_CHECK(reference && insitu);
    CHJSON_CHECK(insitu.offset == reference.offset);
    CHJSON_CHECK(insitu.line == reference.line);
    CHJSON_CHECK(insitu.column == reference.column);
  }
}

static void test_parallel_and_deep_dump() {
  std::string json = "[";
  constexpr int count = 320;
  for (int i = 0; i < count; ++i) {
    if (i) json += ',';
    json += "{\"id\":" + std::to_string(i) + ",\"text\":\"" + std::string(4096, 'x') + "\"}";
  }
  json += ']';
  check_modes(json, true);
  check_modes(json, false, {0, true});
  check_modes(json, false, {1, true});
  std::string bad_utf8 = json;
  bad_utf8[bad_utf8.find(std::string(32, 'x')) + 7] = static_cast<char>(0xFF);
  check_modes(bad_utf8, false);
  check_modes(json + " trailing", true, {256, false});
  auto malformed = json;
  malformed.insert(malformed.find(",\"text\""), " true");
  check_modes(malformed, false, {256, false});
  for (int repeat = 0; repeat < 3; ++repeat) {
    auto result = parse(json);
    CHJSON_CHECK(!result.err);
    for (bool pretty : {false, true}) {
      std::string serial;
      dump_to(serial, result.doc.root(), pretty);
      CHJSON_CHECK(dump_mt(result.doc.root(), {pretty, 4, 1, 1}) == serial);
    }
  }
  std::vector<std::string_view> inputs(64, "{\"k\":\"v\"}");
  inputs[31] = "[1,]";
  std::vector<document_parse_result> results(inputs.size());
  parse_many_into(results.data(), inputs.data(), inputs.size(), {{}, 4});
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    CHJSON_CHECK(static_cast<bool>(results[i].err) == (i == 31));
    if (i != 31) CHJSON_CHECK(results[i].doc.root().find("k")->as_string_view() == "v");
  }
  inputs.assign(4, json);
  parse_many_into(results.data(), inputs.data(), inputs.size(), {{}, 4});
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    CHJSON_CHECK(!results[i].err);
    CHJSON_CHECK(results[i].doc.root().as_array().size() == count);
  }
  // Exceptions in an early worker must not leave other workers using dead captures.
  value::array invalid(2048, value(std::string(512, 'x')));
  invalid[0] = value::number(std::numeric_limits<double>::infinity());
  value legacy(std::move(invalid));
  document doc;
  doc.root() = sv_value::make_array(doc.resource());
  doc.root().array_push_back(doc.resource(), sv_value::number(std::numeric_limits<double>::infinity()));
  for (int i = 0; i < 2048; ++i) doc.root().array_push_back(doc.resource(), sv_value(std::string_view("text")));
  for (bool pretty : {false, true}) {
    for (int repeat = 0; repeat != 8; ++repeat) {
      CHJSON_EXPECT_THROW(dump_mt(legacy, {pretty, 4, 1, 1}));
      CHJSON_EXPECT_THROW(dump_mt(doc.root(), {pretty, 4, 1, 1}));
    }
  }
  const std::string deep = std::string(600, '[') + "0" + std::string(600, ']');
  check_modes(deep, true, {1024, true});
  std::string automatic = "[";
  for (int i = 0; i < 1100; ++i) {
    if (i) automatic += ',';
    automatic += '"' + std::string(1024, 'x') + '"';
  }
  automatic += ']';
  auto automatic_dom = parse(automatic);
  auto automatic_legacy = parse_value(automatic);
  CHJSON_CHECK(!automatic_dom.err && !automatic_legacy.err);
  CHJSON_CHECK(dump(automatic_dom.doc.root()) == automatic);
  CHJSON_CHECK(dump(automatic_legacy.val) == automatic);
}

static void test_bulk_strings_and_flat_arrays() {
  const std::string utf8 = "\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x98\x80";
  std::string unicode;
  for (int i = 0; i < 64; ++i) unicode += utf8;
  for (std::size_t prefix = 0; prefix < 33; ++prefix) {
    const std::string text = '"' + std::string(prefix, 'x') + "\\n" +
        std::string(256, 'a') + unicode + "\\u000A" + std::string(257, 'b') + '"';
    check_modes(text, true);
    check_modes(text.substr(0, text.size() - 1), false);
    auto invalid = text;
    invalid.insert(invalid.size() - 2, 1, static_cast<char>(0xFF));
    check_modes(invalid, false);
    invalid = text;
    invalid.insert(invalid.size() - 2, 1, '\n');
    check_modes(invalid, false);
    const auto original = parse_view(invalid).err;
    const auto decoded = parse_in_situ(invalid).err;
    CHJSON_CHECK(original.offset == decoded.offset && original.line == decoded.line && original.column == decoded.column);
  }
  // Long UTF-8 strings without escapes remain views into the owning buffer,
  // including when the multithreaded parser is selected.
  std::string strings = "[";
  for (int i = 0; i < 320; ++i) {
    if (i) strings += ',';
    strings += '"' + unicode + std::string(4096, 'x') + '"';
  }
  strings += ']';
  auto parsed = parse(strings);
  CHJSON_CHECK(!parsed.err);
  const auto span = parsed.doc.root().as_array()[0].as_string_view();
  CHJSON_CHECK(span == unicode + std::string(4096, 'x'));
  const auto begin = reinterpret_cast<std::uintptr_t>(parsed.doc.buffer().data());
  const auto address = reinterpret_cast<std::uintptr_t>(span.data());
  CHJSON_CHECK(address >= begin && address - begin < parsed.doc.buffer().size());

  std::string ints = "[", floats = "[";
  for (int i = 0; i < 4096; ++i) {
    if (i) { ints += ','; floats += ','; }
    ints += std::to_string(i);
    floats += "1.23456789e-10";
  }
  ints += ']'; floats += ']';
  for (const auto& text : {ints, floats}) {
    check_modes(text, true);
    // Exercise separators, nested values, trailing input and incomplete tokens
    // near every SIMD lane. Sizing must not replace syntax validation.
    for (std::size_t offset = 1; offset <= 32; ++offset) {
      auto nested = text;
      nested.insert(nested.find(',', offset) + 1, "[1,{\"x\":2}],");
      check_modes(nested, true);
    }
    check_modes(text + " junk", true, {256, false});
    check_modes(text.substr(0, text.size() - 1) + ",]", false);
    check_modes(text.substr(0, text.size() - 1), false);
  }
  release_thread_caches();
  auto integer_doc = parse(ints);
  CHJSON_CHECK(!integer_doc.err);
  CHJSON_CHECK(integer_doc.doc.arena().bytes_committed() <= 4096 * sizeof(sv_value) + 1024);
  release_thread_caches();
  auto float_doc = parse(floats);
  CHJSON_CHECK(!float_doc.err);
  CHJSON_CHECK(float_doc.doc.arena().bytes_committed() <= 4096 * sizeof(sv_value) + floats.size() + 1024);
}

void test_regressions() {
  test_utf8_and_boundaries();
  test_document_moves_and_reuse();
  test_memory_and_capacity();
  test_numeric_modes_and_locale();
  test_original_error_positions();
  test_parallel_and_deep_dump();
  test_bulk_strings_and_flat_arrays();
}
