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
    do_not_optimize(r.val.type());
  }
  const auto t1 = clock_type::now();
  return {std::chrono::duration<double>(t1 - t0).count(), json.size() * iters};
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
              << ", committed: " << doc.arena().bytes_committed() << "\n";
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
              << ", committed: " << doc.arena().bytes_committed() << "\n";
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
    auto out = chjson::dump(r.val);
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

  const std::string payload = make_payload(n_objects, 24);
  std::cout << "payload bytes: " << payload.size() << "\n";

  // Warm-up
  {
    auto r = chjson::parse(payload);
    do_not_optimize(r.val.type());
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

  std::cout << "\n== Parse ==\n";
  print_mbps("chjson parse(dom)", run_median(runs, [&](std::size_t) { return bench_chjson_parse_dom(payload, iters); }));
  print_mbps("chjson parse(in_situ)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_insitu(payload, iters, /*print_stats=*/r == 0); }));
  print_mbps("chjson parse(view)", run_median(runs, [&](std::size_t r) { return bench_chjson_parse_view(payload, iters, /*print_stats=*/r == 0); }));
  print_mbps("nlohmann parse", run_median(runs, [&](std::size_t) { return bench_nlohmann_parse(payload, iters); }));
  print_mbps("jsoncpp parse", run_median(runs, [&](std::size_t) { return bench_jsoncpp_parse(payload, iters); }));
  print_mbps("rapidjson parse", run_median(runs, [&](std::size_t) { return bench_rapidjson_parse(payload, iters); }));

  std::cout << "\n== Dump ==\n";
  print_mbps("chjson dump(dom)", run_median(runs, [&](std::size_t) { return bench_chjson_dump_dom(payload, iters); }));
  print_mbps("nlohmann dump", run_median(runs, [&](std::size_t) { return bench_nlohmann_dump(payload, iters); }));
  print_mbps("jsoncpp dump", run_median(runs, [&](std::size_t) { return bench_jsoncpp_dump(payload, iters); }));
  print_mbps("rapidjson dump", run_median(runs, [&](std::size_t) { return bench_rapidjson_dump(payload, iters); }));

  return 0;
}
