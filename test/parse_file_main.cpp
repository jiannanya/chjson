#include <chjson/chjson.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

static std::string slurp_file(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static int parse_one(const char* path) {
  std::string s = slurp_file(path);
  if (s.empty()) {
    // empty could be a valid JSON file only if it contains whitespace+value; treat empty as failure.
    return 2;
  }

  chjson::parse_options opt;
  opt.require_eof = true;

  auto r = chjson::parse(std::string_view{s.data(), s.size()}, opt);
  return r.err ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && std::string_view{argv[1]} == "--list") {
    std::ifstream in(argv[2]);
    if (!in) {
      std::cerr << "failed to read list file: " << argv[2] << "\n";
      return 2;
    }

    bool any_fail = false;
    bool any_io_fail = false;
    std::string path;
    while (std::getline(in, path)) {
      if (path.empty()) continue;
      const int rc = parse_one(path.c_str());
      if (rc == 0) {
        std::cout << path << "\tOK\n";
      } else {
        std::cout << path << "\tFAIL\n";
        any_fail = true;
        if (rc == 2) any_io_fail = true;
      }
    }
    return any_io_fail ? 2 : (any_fail ? 1 : 0);
  }

  if (argc != 2) {
    std::cerr << "usage: chjson_parse_file <file.json>\n";
    std::cerr << "       chjson_parse_file --list <paths.txt>\n";
    return 2;
  }

  const char* path = argv[1];
  const int rc = parse_one(path);
  if (rc == 0) return 0;

  if (rc == 2) {
    std::cerr << "failed to read file or file is empty: " << path << "\n";
    return 2;
  }

  // Re-run to get a detailed error message.
  std::string s = slurp_file(path);
  chjson::parse_options opt;
  opt.require_eof = true;
  auto r = chjson::parse(std::string_view{s.data(), s.size()}, opt);
  std::cerr << "parse failed: " << path << "\n";
  std::cerr << "  code=" << static_cast<int>(r.err.code)
            << " offset=" << r.err.offset
            << " line=" << r.err.line
            << " column=" << r.err.column << "\n";
  return 1;
}
