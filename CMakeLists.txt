cmake_minimum_required(VERSION 3.15)
project(chjson LANGUAGES CXX)

option(CHJSON_BUILD_CONFIG_TESTS "Test alternative allocator/cache/number configurations" OFF)
option(CHJSON_ENABLE_SANITIZERS "Enable address and undefined behavior sanitizers" OFF)

option(CHJSON_BUILD_TESTS "Build chjson tests" ON)
option(CHJSON_BUILD_BENCHMARKS "Build chjson benchmarks" OFF)
option(CHJSON_BUILD_COMPARE_BENCH "Build comparison benchmark vs json_cpp and jsoncpp" OFF)
set(CHJSON_BASELINE_INCLUDE "" CACHE PATH "Optional previous chjson include directory for an identical-flags baseline benchmark")

# Paths to bundled third-party libraries (repo root siblings).
set(_CHJSON_NLOHMANN_INC "${CMAKE_CURRENT_LIST_DIR}/../json_cpp/include")
set(_CHJSON_JSONCPP_DIR  "${CMAKE_CURRENT_LIST_DIR}/../jsoncpp")
set(_CHJSON_RAPIDJSON_INC "${CMAKE_CURRENT_LIST_DIR}/../rapidjson/include")

# jsoncpp needs to be built as a library when we want to link against it (compare bench or
# JSONTestSuite helper tools).
if((CHJSON_BUILD_COMPARE_BENCH OR CHJSON_BUILD_TESTS) AND EXISTS "${_CHJSON_JSONCPP_DIR}/CMakeLists.txt")
  if(NOT TARGET jsoncpp_static AND NOT TARGET jsoncpp_lib)
    # Avoid jsoncpp's tests and post-build unittests inside our build.
    set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(JSONCPP_WITH_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)

    add_subdirectory("${_CHJSON_JSONCPP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/jsoncpp-build" EXCLUDE_FROM_ALL)
  endif()
endif()

find_package(Threads REQUIRED)
add_library(chjson INTERFACE)
add_library(chjson::chjson ALIAS chjson)
target_link_libraries(chjson INTERFACE Threads::Threads)
target_include_directories(chjson INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/another/chfloat/include
)
target_compile_features(chjson INTERFACE cxx_std_17)

if(CHJSON_BUILD_TESTS)
  enable_testing()
  set(CHJSON_TEST_SOURCES
    test/test_main.cpp
    test/test_numbers.cpp
    test/test_strings.cpp
    test/test_structure.cpp
    test/test_errors.cpp
    test/test_dom.cpp
    test/test_random.cpp
    test/test_regressions.cpp
  )
  add_executable(chjson_tests ${CHJSON_TEST_SOURCES})
  target_link_libraries(chjson_tests PRIVATE chjson)
  target_compile_features(chjson_tests PRIVATE cxx_std_17)
  add_test(NAME chjson_tests COMMAND chjson_tests)
  set_tests_properties(chjson_tests PROPERTIES TIMEOUT 120)
  add_executable(chjson_allocation_tests test/test_allocation_failure.cpp)
  target_link_libraries(chjson_allocation_tests PRIVATE chjson)
  # MSVC's debug iterator proxies may allocate inside noexcept STL operations.
  # Disable those extra allocations only for the deliberate OOM injection test.
  target_compile_definitions(chjson_allocation_tests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_ITERATOR_DEBUG_LEVEL=0>)
  add_test(NAME chjson_allocation_tests COMMAND chjson_allocation_tests)
  set_tests_properties(chjson_allocation_tests PROPERTIES TIMEOUT 120)

  if(CHJSON_BUILD_CONFIG_TESTS)
    foreach(config IN ITEMS no_cache pool strtod small_mt)
      add_executable(chjson_tests_${config} ${CHJSON_TEST_SOURCES})
      target_link_libraries(chjson_tests_${config} PRIVATE chjson)
      add_test(NAME chjson_tests_${config} COMMAND chjson_tests_${config})
      set_tests_properties(chjson_tests_${config} PROPERTIES TIMEOUT 120)
    endforeach()
    target_compile_definitions(chjson_tests_no_cache PRIVATE CHJSON_USE_TLS_PARSE_CACHE=0 CHJSON_PARSE_DOM_NO_STRING_FASTPATH=0)
    target_compile_definitions(chjson_tests_pool PRIVATE CHJSON_USE_INTERNAL_ALLOCATOR=1 CHJSON_PARSE_MT_SCAN_SIMD=1)
    target_compile_definitions(chjson_tests_strtod PRIVATE CHJSON_USE_FROM_CHARS_DOUBLE=0 CHJSON_USE_CHFLOAT=0)
    target_compile_definitions(chjson_tests_small_mt PRIVATE CHJSON_PARSE_DOM_ARENA_RESERVE_MAX=1024
      CHJSON_PARSE_MT_MIN_BYTES=1024 CHJSON_PARSE_MT_MIN_SPANS=4 CHJSON_PARSE_MT_MIN_AVG_SPAN_BYTES=64)
  endif()


  add_executable(chjson_parse_file
    test/parse_file_main.cpp
  )
  target_link_libraries(chjson_parse_file PRIVATE chjson)
  target_compile_features(chjson_parse_file PRIVATE cxx_std_17)

  # JSONTestSuite helper tools for cross-library robustness comparison.
  if(EXISTS "${_CHJSON_NLOHMANN_INC}")
    add_executable(nlohmann_parse_file test/nlohmann_parse_file_main.cpp)
    target_compile_features(nlohmann_parse_file PRIVATE cxx_std_17)
    target_include_directories(nlohmann_parse_file PRIVATE "${_CHJSON_NLOHMANN_INC}")
  endif()

  if(EXISTS "${_CHJSON_RAPIDJSON_INC}")
    add_executable(rapidjson_parse_file test/rapidjson_parse_file_main.cpp)
    target_compile_features(rapidjson_parse_file PRIVATE cxx_std_17)
    target_include_directories(rapidjson_parse_file PRIVATE "${_CHJSON_RAPIDJSON_INC}")
  endif()

  if(EXISTS "${_CHJSON_JSONCPP_DIR}/include")
    add_executable(jsoncpp_parse_file test/jsoncpp_parse_file_main.cpp)
    target_compile_features(jsoncpp_parse_file PRIVATE cxx_std_17)
    target_include_directories(jsoncpp_parse_file PRIVATE "${_CHJSON_JSONCPP_DIR}/include")
    if(TARGET jsoncpp_static)
      target_link_libraries(jsoncpp_parse_file PRIVATE jsoncpp_static)
    elseif(TARGET jsoncpp_lib)
      target_link_libraries(jsoncpp_parse_file PRIVATE jsoncpp_lib)
    endif()
  endif()
endif()

if(CHJSON_BUILD_BENCHMARKS)
  add_executable(chjson_bench
    benchmark/benchmark_main.cpp
  )
  target_link_libraries(chjson_bench PRIVATE chjson)
  target_compile_features(chjson_bench PRIVATE cxx_std_17)

  if(CHJSON_BASELINE_INCLUDE)
    if(NOT EXISTS "${CHJSON_BASELINE_INCLUDE}/chjson/chjson.hpp")
      message(FATAL_ERROR "CHJSON_BASELINE_INCLUDE must contain chjson/chjson.hpp")
    endif()
    add_executable(chjson_baseline_bench benchmark/benchmark_main.cpp)
    target_include_directories(chjson_baseline_bench BEFORE PRIVATE "${CHJSON_BASELINE_INCLUDE}")
    target_link_libraries(chjson_baseline_bench PRIVATE chjson)
  endif()

  if(CHJSON_BUILD_COMPARE_BENCH)
    add_executable(chjson_compare_bench
      benchmark/compare_main.cpp
    )
    target_link_libraries(chjson_compare_bench PRIVATE chjson)
    target_compile_features(chjson_compare_bench PRIVATE cxx_std_17)
    if(EXISTS "${_CHJSON_NLOHMANN_INC}")
      target_include_directories(chjson_compare_bench PRIVATE "${_CHJSON_NLOHMANN_INC}")
    endif()

    if(EXISTS "${_CHJSON_RAPIDJSON_INC}")
      target_include_directories(chjson_compare_bench PRIVATE "${_CHJSON_RAPIDJSON_INC}")
    endif()

    if(TARGET jsoncpp_static)
      target_link_libraries(chjson_compare_bench PRIVATE jsoncpp_static)
    elseif(TARGET jsoncpp_lib)
      target_link_libraries(chjson_compare_bench PRIVATE jsoncpp_lib)
    endif()
  endif()
endif()

if(CHJSON_ENABLE_SANITIZERS)
  if(MSVC)
    target_compile_options(chjson INTERFACE /fsanitize=address /Zi)
    target_link_options(chjson INTERFACE /INCREMENTAL:NO)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(chjson INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(chjson INTERFACE -fsanitize=address,undefined)
  else()
    message(FATAL_ERROR "Sanitizers are unsupported by this compiler")
  endif()
endif()
