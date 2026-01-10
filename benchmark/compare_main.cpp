#include <chjson/chjson.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

// nlohmann/json (from json_cpp)
#include <nlohmann/json.hpp>

// jsoncpp (from jsoncpp)
#include <json/json.h>

// RapidJSON (from rapidjson)
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace {

using clock_type = std::chrono::high_resolution_clock;

template <class T>
inline void do_not_optimize(const T& v) {
#if defined(_MSC_VER)
  volatile const char* p = reinterpret_cast<const char*>(&v);
  (void)p;
#else
  asm volatile("" : : "g"(v) : "memory");
#endif
}

std::string make_payload(std::size_t n_objects, std::size_t str_len) {
  std::mt19937_64 rng(1234567);
  std::uniform_int_distribution<int> ch('a', 'z');

  std::string s;
  s.reserve(n_objects * (str_len + 64));
  s.push_back('[');
  for (std::size_t i = 0; i < n_objects; ++i) {
    if (i) s.push_back(',');
    s += "{\"id\":";
    s += std::to_string(static_cast<std::uint64_t>(i));
    s += ",\"ok\":";
    s += (i % 2 == 0) ? "true" : "false";
    s += ",\"name\":\"";

    for (std::size_t k = 0; k < str_len; ++k) {
      s.push_back(static_cast<char>(ch(rng)));
    }

    if ((i % 16) == 0) {
      s += "\\n";
      s += "\\u4F60\\u597D";
    }

    s += "\",\"val\":";
    s += (i % 3 == 0) ? "3.141592653589793" : "1e-10";
    s += "}";
  }
  s.push_back(']');
  return s;
}

std::string make_numbers_payload(std::size_t n_numbers) {
  // A number-heavy payload to stress float parsing.
  // Keep everything a float (contains '.' or 'e') so it goes through the fp path.
  std::string s;
  s.reserve(n_numbers * 24);
  s.push_back('[');
  for (std::size_t i = 0; i < n_numbers; ++i) {
    if (i) s.push_back(',');
    switch (i & 3u) {
      case 0: s += "3.141592653589793"; break;
      case 1: s += "-0.000000000123456789"; break;
      case 2: s += "1.234567890123456e-200"; break;
      default: s += "2.2250738585072014e-308"; break;
    }
  }
  s.push_back(']');
  return s;
}

std::size_t scale_iters(std::size_t base_iters, std::size_t base_bytes, std::size_t new_bytes) {
  if (base_iters == 0) return 0;
  if (base_bytes == 0 || new_bytes == 0) return base_iters;
  const double ratio = static_cast<double>(base_bytes) / static_cast<double>(new_bytes);
  const double scaled = static_cast<double>(base_iters) * ratio;
  const auto out = static_cast<std::size_t>(scaled + 0.5);
  return (out > 0) ? out : 1;
}

struct bench_result {
  double seconds{0.0};
  std::size_t bytes{0};
};

template <class Fn>
bench_result run_median(std::size_t runs, Fn&& fn) {
  if (runs <= 1) return fn(0);
  std::vector<double> secs;
  secs.reserve(runs);
  std::size_t bytes = 0;
  for (std::size_t r = 0; r < runs; ++r) {
    const auto br = fn(r);
    secs.push_back(br.seconds);
    bytes = br.bytes;
  }
  std::nth_element(secs.begin(), secs.begin() + (secs.size() / 2), secs.end());
  return {secs[secs.size() / 2], bytes};
}

void print_mbps(const char* name, const bench_result& r) {
  const double mib = static_cast<double>(r.bytes) / (1024.0 * 1024.0);
  const double mibps = (r.seconds > 0.0) ? (mib / r.seconds) : 0.0;
  std::cout << name << ": " << mibps << " MiB/s (" << r.seconds << " s)" << "\n";
}

bench_result bench_chjson_parse_dom(std::string_view json, std::size_t iters) {
  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    auto r = chjson::parse(json);
    do_not_optimize(r.err.code);
    do_not_optimize(r.doc.root().type());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), json.size() * iters};
}

bench_result bench_chjson_parse_owning_view(std::string_view json, std::size_t iters, bool print_stats) {
  chjson::document doc;
  // Reserve arena but avoid reserving/storing a full input buffer.
  doc.arena().reserve_bytes(json.size() * 8u);

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    auto err = chjson::parse_owning_view_into(doc, json);
    do_not_optimize(err.code);
    do_not_optimize(doc.root().type());
  }
  const auto t1 = clock_type::now();

  if (print_stats) {
    std::cout << "chjson owning_view arena used: " << doc.arena().bytes_used()
              << ", committed: " << doc.arena().bytes_committed()
              << ", blocks: " << doc.arena().blocks() << "\n";
  }

  return {std::chrono::duration<double>(t1 - t0).count(), json.size() * iters};
}

bench_result bench_chjson_dom_parse_and_sum_numbers(std::string_view json, std::size_t iters) {
  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t iter = 0; iter < iters; ++iter) {
    auto r = chjson::parse(json);
    if (r.err || r.doc.root().type() != chjson::sv_value::kind::array) {
      std::cerr << "chjson: input parse failed (dom +sum)\n";
      std::exit(1);
    }
    const auto arr = r.doc.root().as_array();
    double sum = 0.0;
    for (std::size_t k = 0; k < arr.size(); ++k) {
      sum += arr[k].as_double_unchecked();
    }
    do_not_optimize(sum);
    bytes += json.size();
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_chjson_owning_view_parse_and_sum_numbers(std::string_view json, std::size_t iters) {
  chjson::document doc;
  doc.arena().reserve_bytes(json.size() * 8u);

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t iter = 0; iter < iters; ++iter) {
    auto err = chjson::parse_owning_view_into(doc, json);
    if (err || doc.root().type() != chjson::sv_value::kind::array) {
      std::cerr << "chjson: input parse failed (owning_view +sum)\n";
      std::exit(1);
    }
    const auto arr = doc.root().as_array();
    double sum = 0.0;
    for (std::size_t k = 0; k < arr.size(); ++k) {
      sum += arr[k].as_double_unchecked();
    }
    do_not_optimize(sum);
    bytes += json.size();
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_chjson_insitu_parse_and_sum_numbers(std::string_view json, std::size_t iters) {
  chjson::document doc;
  doc.reserve_buffer(json.size());

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t iter = 0; iter < iters; ++iter) {
    auto err = chjson::parse_in_situ_into(doc, json);
    if (err || doc.root().type() != chjson::sv_value::kind::array) {
      std::cerr << "chjson: input parse failed (in_situ +sum)\n";
      std::exit(1);
    }
    const auto arr = doc.root().as_array();
    double sum = 0.0;
    for (std::size_t k = 0; k < arr.size(); ++k) {
      sum += arr[k].as_double_unchecked();
    }
    do_not_optimize(sum);
    bytes += json.size();
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_chjson_view_parse_and_sum_numbers(std::string_view json, std::size_t iters) {
  chjson::view_document doc;

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t iter = 0; iter < iters; ++iter) {
    auto err = chjson::parse_view_into(doc, json);
    if (err || doc.root().type() != chjson::sv_value::kind::array) {
      std::cerr << "chjson: input parse failed (view +sum)\n";
      std::exit(1);
    }
    const auto arr = doc.root().as_array();
    double sum = 0.0;
    for (std::size_t k = 0; k < arr.size(); ++k) {
      sum += arr[k].as_double_unchecked();
    }
    do_not_optimize(sum);
    bytes += json.size();
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_chjson_parse_insitu(std::string_view json, std::size_t iters, bool print_stats) {
  chjson::document doc;
  doc.reserve_buffer(json.size());

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    auto err = chjson::parse_in_situ_into(doc, json);
    do_not_optimize(err.code);
    do_not_optimize(doc.root().type());
  }
  const auto t1 = clock_type::now();

  if (print_stats) {
    // Report memory stats outside the timed loop to avoid skewing throughput.
    std::cout << "chjson arena used: " << doc.arena().bytes_used()
              << ", committed: " << doc.arena().bytes_committed()
              << ", blocks: " << doc.arena().blocks() << "\n";
  }

  return {std::chrono::duration<double>(t1 - t0).count(), json.size() * iters};
}

bench_result bench_chjson_parse_view(std::string_view json, std::size_t iters, bool print_stats) {
  chjson::view_document doc;

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    auto err = chjson::parse_view_into(doc, json);
    do_not_optimize(err.code);
    do_not_optimize(doc.root().type());
  }
  const auto t1 = clock_type::now();

  if (print_stats) {
    // Report memory stats outside the timed loop to avoid skewing throughput.
    std::cout << "chjson view arena used: " << doc.arena().bytes_used()
              << ", committed: " << doc.arena().bytes_committed()
              << ", blocks: " << doc.arena().blocks() << "\n";
  }

  return {std::chrono::duration<double>(t1 - t0).count(), json.size() * iters};
}

bench_result bench_chjson_dump_dom(std::string_view json, std::size_t iters) {
  auto r = chjson::parse(json);
  if (r.err) {
    std::cerr << "chjson: input parse failed\n";
    std::exit(1);
  }

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < iters; ++i) {
    auto out = chjson::dump(r.doc.root());
    bytes += out.size();
    do_not_optimize(out.size());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_nlohmann_parse(std::string_view json_text, std::size_t iters) {
  using nlohmann::json;

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    json j = json::parse(json_text, /*callback=*/nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/false);
    do_not_optimize(j.is_discarded());
    do_not_optimize(j.type());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), json_text.size() * iters};
}

bench_result bench_nlohmann_dump(std::string_view json_text, std::size_t iters) {
  using nlohmann::json;

  json j = json::parse(json_text, nullptr, false, false);
  if (j.is_discarded()) {
    std::cerr << "nlohmann: input parse failed\n";
    std::exit(1);
  }

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < iters; ++i) {
    std::string out = j.dump();
    bytes += out.size();
    do_not_optimize(out.size());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_jsoncpp_parse(std::string_view json, std::size_t iters) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    Json::Value root;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const bool ok = reader->parse(json.data(), json.data() + json.size(), &root, &errs);
    do_not_optimize(ok);
    do_not_optimize(root.type());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), json.size() * iters};
}

bench_result bench_jsoncpp_dump(std::string_view json_text, std::size_t iters) {
  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;

  Json::Value root;
  {
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    const bool ok = reader->parse(json_text.data(), json_text.data() + json_text.size(), &root, &errs);
    if (!ok) {
      std::cerr << "jsoncpp: input parse failed: " << errs << "\n";
      std::exit(1);
    }
  }

  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  wb["emitUTF8"] = true;
  wb["useSpecialFloats"] = false;
  wb["precision"] = 17;
  wb["precisionType"] = "significant";

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < iters; ++i) {
    std::string out = Json::writeString(wb, root);
    bytes += out.size();
    do_not_optimize(out.size());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_rapidjson_parse(std::string_view json_text, std::size_t iters) {
  using namespace rapidjson;

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    Document d;
    d.Parse(json_text.data(), json_text.size());
    do_not_optimize(d.HasParseError());
    do_not_optimize(d.GetType());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), json_text.size() * iters};
}

bench_result bench_rapidjson_parse_and_sum_numbers(std::string_view json_text, std::size_t iters) {
  using namespace rapidjson;

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < iters; ++i) {
    Document d;
    d.Parse(json_text.data(), json_text.size());
    if (d.HasParseError() || !d.IsArray()) {
      std::cerr << "rapidjson: input parse failed (numbers)\n";
      std::exit(1);
    }
    double sum = 0.0;
    for (auto& v : d.GetArray()) {
      // The numbers payload is floats; GetDouble() is the relevant access pattern.
      sum += v.GetDouble();
    }
    do_not_optimize(sum);
    bytes += json_text.size();
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

bench_result bench_rapidjson_dump(std::string_view json_text, std::size_t iters) {
  using namespace rapidjson;

  Document d;
  d.Parse(json_text.data(), json_text.size());
  if (d.HasParseError()) {
    std::cerr << "rapidjson: input parse failed\n";
    std::exit(1);
  }

  const auto t0 = clock_type::now();
  std::size_t bytes = 0;
  for (std::size_t i = 0; i < iters; ++i) {
    StringBuffer sb;
    Writer<StringBuffer> w(sb);
    d.Accept(w);
    bytes += sb.GetSize();
    do_not_optimize(sb.GetSize());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), bytes};
}

} // namespace

int main(int argc, char** argv) {
  std::size_t n_objects = 2000;
  std::size_t iters = 200;
  std::size_t runs = 5;

  if (argc >= 2) n_objects = static_cast<std::size_t>(std::stoull(argv[1]));
  if (argc >= 3) iters = static_cast<std::size_t>(std::stoull(argv[2]));
  if (argc >= 4) runs = static_cast<std::size_t>(std::stoull(argv[3]));

  std::cout << "sizeof(chjson::sv_value): " << sizeof(chjson::sv_value) << "\n";
  std::cout << "sizeof(rapidjson::Value): " << sizeof(rapidjson::Value) << "\n";

  const std::string payload = make_payload(n_objects, 24);
  std::cout << "payload bytes: " << payload.size() << "\n";

  // A second payload focused on numeric parsing.
  std::size_t numbers_count = std::max<std::size_t>(1, n_objects * 64);
  const bool custom_numbers_count = (argc >= 5);
  if (custom_numbers_count) numbers_count = static_cast<std::size_t>(std::stoull(argv[4]));
  const std::string numbers_payload = make_numbers_payload(numbers_count);
  const std::size_t numbers_min_iters = custom_numbers_count ? 1 : 50;
  const std::size_t numbers_iters = std::max<std::size_t>(scale_iters(iters, payload.size(), numbers_payload.size()), numbers_min_iters);
  std::cout << "numbers payload bytes: " << numbers_payload.size() << " (" << numbers_count << " numbers, iters=" << numbers_iters << ")\n";

  // Warm-up
  {
    auto r = chjson::parse(payload);
    do_not_optimize(r.doc.root().type());
  }
  {
    chjson::document doc;
    (void)chjson::parse_in_situ_into(doc, payload);
    do_not_optimize(doc.root().type());
  }
  {
    chjson::view_document doc;
    (void)chjson::parse_view_into(doc, payload);
    do_not_optimize(doc.root().type());
  }
  {
    auto j = nlohmann::json::parse(payload, nullptr, false, false);
    do_not_optimize(j.type());
  }
  {
    Json::CharReaderBuilder builder;
    Json::Value root;
    std::string errs;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    (void)reader->parse(payload.data(), payload.data() + payload.size(), &root, &errs);
    do_not_optimize(root.type());
  }
  {
    rapidjson::Document d;
    d.Parse(payload.data(), payload.size());
    do_not_optimize(d.GetType());
  }

  // Warm-up for number-heavy payload
  {
    auto r = chjson::parse(numbers_payload);
    do_not_optimize(r.doc.root().type());
  }
  {
    rapidjson::Document d;
    d.Parse(numbers_payload.data(), numbers_payload.size());
    do_not_optimize(d.GetType());
  }

  std::cout << "\n== Parse ==\n";
  print_mbps("chjson parse(dom)", run_median(runs, [&](std::size_t) { return bench_chjson_parse_dom(payload, iters); }));
  print_mbps("chjson parse(owning_view)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_owning_view(payload, iters, /*print_stats=*/false); }));
  print_mbps("chjson parse(in_situ)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_insitu(payload, iters, /*print_stats=*/false); }));
  print_mbps("chjson parse(view)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_view(payload, iters, /*print_stats=*/false); }));
  // print_mbps("nlohmann parse", run_median(runs, [&](std::size_t) { return bench_nlohmann_parse(payload, iters); }));
  // print_mbps("jsoncpp parse", run_median(runs, [&](std::size_t) { return bench_jsoncpp_parse(payload, iters); }));
  print_mbps("rapidjson parse", run_median(runs, [&](std::size_t) { return bench_rapidjson_parse(payload, iters); }));

  std::cout << "\n== Parse (numbers) ==\n";
  print_mbps("chjson parse(dom)", run_median(runs, [&](std::size_t) { return bench_chjson_parse_dom(numbers_payload, numbers_iters); }));
  print_mbps("chjson parse(owning_view)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_owning_view(numbers_payload, numbers_iters, /*print_stats=*/false); }));
  print_mbps("chjson parse(in_situ)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_insitu(numbers_payload, numbers_iters, /*print_stats=*/false); }));
  print_mbps("chjson parse(view)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_view(numbers_payload, numbers_iters, /*print_stats=*/false); }));
  print_mbps("rapidjson parse", run_median(runs, [&](std::size_t) { return bench_rapidjson_parse(numbers_payload, numbers_iters); }));

  std::cout << "\n== Parse+sum (numbers) ==\n";
  print_mbps("chjson dom +sum", run_median(runs, [&](std::size_t) { return bench_chjson_dom_parse_and_sum_numbers(numbers_payload, numbers_iters); }));
  print_mbps("chjson owning_view +sum", run_median(runs, [&](std::size_t) { return bench_chjson_owning_view_parse_and_sum_numbers(numbers_payload, numbers_iters); }));
  print_mbps("chjson in_situ +sum", run_median(runs, [&](std::size_t) { return bench_chjson_insitu_parse_and_sum_numbers(numbers_payload, numbers_iters); }));
  print_mbps("chjson view +sum", run_median(runs, [&](std::size_t) { return bench_chjson_view_parse_and_sum_numbers(numbers_payload, numbers_iters); }));
  print_mbps("rapidjson +sum", run_median(runs, [&](std::size_t) { return bench_rapidjson_parse_and_sum_numbers(numbers_payload, numbers_iters); }));

  std::cout << "\n== Dump ==\n";
  print_mbps("chjson dump(dom)", run_median(runs, [&](std::size_t) { return bench_chjson_dump_dom(payload, iters); }));
  print_mbps("nlohmann dump", run_median(runs, [&](std::size_t) { return bench_nlohmann_dump(payload, iters); }));
  print_mbps("jsoncpp dump", run_median(runs, [&](std::size_t) { return bench_jsoncpp_dump(payload, iters); }));
  print_mbps("rapidjson dump", run_median(runs, [&](std::size_t) { return bench_rapidjson_dump(payload, iters); }));

  return 0;
}
