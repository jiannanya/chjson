#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

static int parse_one(const char* path) {
  std::string text;
  {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return 2;
    ifs.seekg(0, std::ios::end);
    const auto end = ifs.tellg();
    if (end < 0) return 2;
    text.resize(static_cast<std::size_t>(end));
    ifs.seekg(0, std::ios::beg);
    if (!text.empty()) {
      if (!ifs.read(text.data(), static_cast<std::streamsize>(text.size()))) return 2;
    }
  }

  using nlohmann::json;
  json j = json::parse(text, /*callback=*/nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/false);
  return j.is_discarded() ? 1 : 0;
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
    std::cerr << "usage: nlohmann_parse_file <file.json>\n";
    std::cerr << "       nlohmann_parse_file --list <paths.txt>\n";
    return 2;
  }

  const int rc = parse_one(argv[1]);
  if (rc == 2) {
    std::cerr << "read failed: " << argv[1] << "\n";
  }
  return rc == 2 ? 2 : rc;
}
