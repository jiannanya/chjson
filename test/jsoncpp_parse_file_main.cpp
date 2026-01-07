#include <json/json.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace {

bool read_all(const char* path, std::string& out) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) return false;
  ifs.seekg(0, std::ios::end);
  const auto end = ifs.tellg();
  if (end < 0) return false;
  out.resize(static_cast<std::size_t>(end));
  ifs.seekg(0, std::ios::beg);
  if (!out.empty()) {
    if (!ifs.read(out.data(), static_cast<std::streamsize>(out.size()))) return false;
  }
  return true;
}

} // namespace

static int parse_one(const char* path) {
  std::string text;
  if (!read_all(path, text)) return 2;

  Json::CharReaderBuilder builder;
  builder["collectComments"] = false;
  builder["allowComments"] = false;
  builder["allowTrailingCommas"] = false;
  builder["strictRoot"] = true;

  Json::Value root;
  std::string errs;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  const bool ok = reader->parse(text.data(), text.data() + text.size(), &root, &errs);
  return ok ? 0 : 1;
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
    std::cerr << "usage: jsoncpp_parse_file <file.json>\n";
    std::cerr << "       jsoncpp_parse_file --list <paths.txt>\n";
    return 2;
  }

  const int rc = parse_one(argv[1]);
  if (rc == 2) {
    std::cerr << "read failed: " << argv[1] << "\n";
  }
  return rc == 2 ? 2 : rc;
}
