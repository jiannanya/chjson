#include <chjson/chjson.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

template <class T>
inline void do_not_optimize(const T& v) {
#if defined(_MSC_VER)
  const volatile unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
  (void)*p;
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
      // Keep it ASCII (no escaping) to make pure parse cost visible.
      s.push_back(static_cast<char>(ch(rng)));
    }

    // Add some escapes/unicode occasionally.
    if ((i % 16) == 0) {
      s += "\\n";
      s += "\\u4F60\\u597D";
    }

    s += "\",\"val\":";
    // Keep this field float-heavy to stress number parsing.
    s += (i % 3 == 0) ? "3.141592653589793" : "1e-10";
    s += "}";
  }
  s.push_back(']');
  return s;
}

std::string make_workload(std::string_view mode, std::size_t count, std::size_t str_len) {
  if (mode == "objects") return make_payload(count, str_len);
  if (mode == "scalar") return "123";
  std::string out = "[";
  for (std::size_t i = 0; i < count; ++i) {
    if (i) out += ',';
    if (mode == "integers") out += std::to_string(i);
    else if (mode == "floats") out += (i % 2) ? "3.141592653589793" : "1.23456789e-10";
    else if (mode == "strings") out += '"' + std::string(str_len, 'x') + '"';
    else if (mode == "escaped_strings") out += "\"\\n" + std::string(str_len, 'x') + "\\u4F60\\u597D\"";
    else if (mode == "utf8_strings") {
      out += '"';
      for (std::size_t j = 0; j < str_len / 6; ++j) out += "\xE4\xBD\xA0\xE5\xA5\xBD";
      out += std::string(str_len % 6, 'x') + '"';
    }
    else if (mode == "empty") out += "{\"a\":[],\"b\":{}}";
    else throw std::invalid_argument("unknown workload; use objects, integers, floats, strings, escaped_strings, utf8_strings, empty, or scalar");
  }
  out += ']';
  return out;
}

struct bench_result {
  double seconds{0.0};
  std::size_t bytes{0};
};

template <class Fn>
bench_result run_median(std::size_t runs, Fn&& fn) {
  if (runs <= 1) return fn();
  std::vector<double> secs;
  secs.reserve(runs);
  std::size_t bytes = 0;
  for (std::size_t r = 0; r < runs; ++r) {
    const auto br = fn();
    secs.push_back(br.seconds);
    bytes = br.bytes;
  }
  std::nth_element(secs.begin(), secs.begin() + (secs.size() / 2), secs.end());
  return {secs[secs.size() / 2], bytes};
}

bench_result bench_parse_dom(std::string_view json, std::size_t iters) {
  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    auto r = chjson::parse(json);
    if (r.err) throw std::runtime_error("benchmark input did not parse");
    do_not_optimize(r.err.code);
    do_not_optimize(r.doc.root().type());
  }
  const auto t1 = clock_type::now();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  return {sec, json.size() * iters};
}

bench_result bench_parse_insitu(std::string_view json, std::size_t iters, std::size_t& used_bytes_out, std::size_t& committed_bytes_out,
                               std::size_t& blocks_out) {
  used_bytes_out = 0;
  committed_bytes_out = 0;
  blocks_out = 0;

  chjson::document doc;
  doc.reserve_buffer(json.size());

  const auto t0 = clock_type::now();
  for (std::size_t i = 0; i < iters; ++i) {
    auto err = chjson::parse_in_situ_into(doc, json);
    if (err) throw std::runtime_error("benchmark input did not parse");
    do_not_optimize(err.code);
    do_not_optimize(doc.root().type());
  }
  const auto t1 = clock_type::now();
  // Read stats after timing to avoid affecting the measured parse throughput.
  used_bytes_out = doc.arena().bytes_used();
  committed_bytes_out = doc.arena().bytes_committed();
  blocks_out = doc.arena().blocks();
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  return {sec, json.size() * iters};
}

bench_result bench_dump_dom(std::string_view json, std::size_t iters) {
  auto r = chjson::parse(json);
  if (r.err) {
    std::cerr << "input parse failed\n";
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
  const double sec = std::chrono::duration<double>(t1 - t0).count();
  return {sec, bytes};
}

void print_mbps(const char* name, const bench_result& r) {
  const double mb = static_cast<double>(r.bytes) / (1024.0 * 1024.0);
  const double mbps = (r.seconds > 0.0) ? (mb / r.seconds) : 0.0;
  std::cout << name << ": " << mbps << " MiB/s (" << r.seconds << " s)" << "\n";
}

} // namespace

int main(int argc, char** argv) {
  std::size_t n_objects = 2000;
  std::size_t str_len = 24;
  std::size_t iters = 200;
  std::size_t runs = 5;

  if (argc >= 2) n_objects = static_cast<std::size_t>(std::stoull(argv[1]));
  if (argc >= 3) iters = static_cast<std::size_t>(std::stoull(argv[2]));
  if (argc >= 4) runs = static_cast<std::size_t>(std::stoull(argv[3]));
  const std::string_view mode = argc >= 5 ? argv[4] : "objects";
  if (argc >= 6) str_len = static_cast<std::size_t>(std::stoull(argv[5]));
  if (iters == 0 || runs == 0) {
    std::cerr << "iterations and runs must be positive\n";
    return 2;
  }

  const std::string payload = make_workload(mode, n_objects, str_len);
  std::cout << "workload: " << mode << "\n";
  std::cout << "payload bytes: " << payload.size() << "\n";

  // A fresh thread has no inherited parse caches. Measure cold document storage
  // separately from the reused arena below; neither is process RSS.
  std::size_t cold_used = 0, cold_committed = 0;
  std::thread measure([&] {
    auto parsed = chjson::parse(payload);
    if (parsed.err) std::abort();
    cold_used = parsed.doc.arena().bytes_used();
    cold_committed = parsed.doc.arena().bytes_committed();
  });
  measure.join();
  std::cout << "cold arena used bytes: " << cold_used << ", committed bytes: " << cold_committed << '\n';

  // Warm-up
  {
    auto r = chjson::parse(payload);
    do_not_optimize(r.doc.root().type());
  }
  {
    auto r = chjson::parse_in_situ(std::string(payload));
    do_not_optimize(r.doc.root().type());
  }

  print_mbps("parse(dom)", run_median(runs, [&] { return bench_parse_dom(payload, iters); }));

  std::size_t used_sum = 0;
  std::size_t committed_sum = 0;
  std::size_t blocks_sum = 0;
  auto insitu_parse = run_median(runs, [&] { return bench_parse_insitu(payload, iters, used_sum, committed_sum, blocks_sum); });
  print_mbps("parse(in_situ)", insitu_parse);
  std::cout << "arena used bytes: " << used_sum
            << ", committed bytes: " << committed_sum
            << ", blocks: " << blocks_sum << "\n";

  print_mbps("dump(dom)", run_median(runs, [&] { return bench_dump_dom(payload, iters); }));

  return 0;
}
