// This executable deliberately fails one allocation at a time. Keep it isolated
// from other tests and disable caches so every parse exercises actual allocation.
#define CHJSON_USE_INTERNAL_ALLOCATOR 0
#define CHJSON_USE_TLS_PARSE_CACHE 0
#include "test_common.hpp"
#include <new>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

static thread_local long fail_after = -1;

#if defined(__GNUC__) && !defined(__clang__)
#define CHJSON_TEST_NOINLINE __attribute__((noinline))
#else
#define CHJSON_TEST_NOINLINE
#endif

CHJSON_TEST_NOINLINE void* operator new(std::size_t size) {
  if (fail_after == 0) throw std::bad_alloc();
  if (fail_after > 0) --fail_after;
  if (void* p = std::malloc(size ? size : 1)) return p;
  throw std::bad_alloc();
}
CHJSON_TEST_NOINLINE void operator delete(void* p) noexcept { std::free(p); }
CHJSON_TEST_NOINLINE void* operator new[](std::size_t size) { return ::operator new(size); }
CHJSON_TEST_NOINLINE void operator delete[](void* p) noexcept { ::operator delete(p); }
CHJSON_TEST_NOINLINE void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
CHJSON_TEST_NOINLINE void operator delete[](void* p, std::size_t) noexcept { ::operator delete(p); }

int main() {
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
  const std::string inputs[] = {
    "{\"text\":\"\\n" + std::string(4096, 'x') + "\",\"array\":[0,1,2,3,4,5,6,7,8,9]}",
    "1" + std::string(4096, '0') + "e9999"
  };
  for (const auto& input : inputs) {
  for (int mode = 0; mode != 8; ++mode) {
    unsigned failures = 0;
    bool succeeded = false;
    for (long attempt = 0; attempt != 128; ++attempt) {
      chjson::document doc;
      chjson::view_document view;
      std::string copy = input;
      chjson::error error;
      fail_after = attempt;
      try {
        switch (mode) {
          case 0: error = chjson::parse(input).err; break;
          case 1: error = chjson::parse_value(input).err; break;
          case 2: error = chjson::parse_view(input).err; break;
          case 3: error = chjson::parse_owning_view(input).err; break;
          case 4: error = chjson::parse_in_situ(std::move(copy)).err; break;
          case 5: error = chjson::parse_in_situ_into(doc, input); break;
          case 6: error = chjson::parse_view_into(view, input); break;
          case 7: error = chjson::parse_owning_view_into(doc, input); break;
        }
      } catch (...) {
        fail_after = -1;
        CHJSON_CHECK(false);
      }
      fail_after = -1;
      if (!error) { succeeded = true; break; }
      CHJSON_CHECK(error.code == chjson::error_code::out_of_memory);
      if (mode == 5 || mode == 7) {
        CHJSON_CHECK(doc.root().is_null());
        CHJSON_CHECK(!chjson::parse_in_situ_into(doc, "[1]"));
      }
      if (mode == 6) {
        CHJSON_CHECK(view.root().is_null());
        CHJSON_CHECK(!chjson::parse_view_into(view, "[1]"));
      }
      ++failures;
    }
    CHJSON_CHECK(succeeded);
    if (input.front() == '{') CHJSON_CHECK(failures > 0);
  }
  }
  chjson::detail::owned_number_value number;
  number.set_raw("1.00000000000000000000000000000000000000001");
  const auto saved = std::string(number.raw_token());
  auto other = number;
  fail_after = 0;
  bool threw = false;
  try { number = other; } catch (const std::bad_alloc&) { threw = true; }
  fail_after = -1;
  CHJSON_CHECK(threw && number.raw_token() == saved);
  const std::string huge_token = "1" + std::string(300000, '0') + "e9999";
  auto lazy = chjson::sv_value::number_token(huge_token);
  fail_after = 0;
  threw = false;
  try { (void)lazy.as_double(); } catch (const std::bad_alloc&) { threw = true; }
  fail_after = -1;
  CHJSON_CHECK(threw);
  std::cout << "allocation failure tests passed\n";
}
