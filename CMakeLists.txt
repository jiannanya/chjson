cmake_minimum_required(VERSION 3.15)
project(chjson LANGUAGES CXX)

# Release+Clang performance flags.
# Note: keep this limited to safe perf knobs (no fast-math / LTO / exception model changes).
function(chjson_enable_release_clang_perf target_name)
  # if(NOT TARGET ${target_name})
  #   return()
  # endif()

  # if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  #   return()
  # endif()

  # get_target_property(_chjson_target_type ${target_name} TYPE)
  # if(_chjson_target_type STREQUAL "INTERFACE_LIBRARY")
  #   if(MSVC)
  #     # MSVC/clang-cl already enables high optimization in Release by default; keep this minimal.
  #     target_compile_options(${target_name} INTERFACE $<$<CONFIG:Release>:/O2 /DNDEBUG>)
  #   else()
  #     # Rely on CMake's default Release flags for -O3/-DNDEBUG; add only arch/tune knobs.
  #     target_compile_options(${target_name} INTERFACE $<$<CONFIG:Release>:-march=native -mtune=native -fomit-frame-pointer>)
  #   endif()
  # else()
  #   if(MSVC)
  #     target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:/O2 /DNDEBUG>)
  #   else()
  #     target_compile_options(${target_name} PRIVATE $<$<CONFIG:Release>:-march=native -mtune=native -fomit-frame-pointer>)
  #   endif()
  # endif()
endfunction()

option(CHJSON_BUILD_TESTS "Build chjson tests" ON)
option(CHJSON_BUILD_BENCHMARKS "Build chjson benchmarks" OFF)
option(CHJSON_BUILD_COMPARE_BENCH "Build comparison benchmark vs json_cpp and jsoncpp" OFF)

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

add_library(chjson INTERFACE)
target_include_directories(chjson INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/another/chfloat/include
)
target_compile_features(chjson INTERFACE cxx_std_17)
chjson_enable_release_clang_perf(chjson)

if(CHJSON_BUILD_TESTS)
  enable_testing()
  add_executable(chjson_tests
    test/test_main.cpp
    test/test_numbers.cpp
    test/test_strings.cpp
    test/test_structure.cpp
    test/test_errors.cpp
    test/test_dom.cpp
    test/test_random.cpp
  )
  target_link_libraries(chjson_tests PRIVATE chjson)
  target_compile_features(chjson_tests PRIVATE cxx_std_17)
  chjson_enable_release_clang_perf(chjson_tests)
  add_test(NAME chjson_tests COMMAND chjson_tests)

  add_executable(chjson_parse_file
    test/parse_file_main.cpp
  )
  target_link_libraries(chjson_parse_file PRIVATE chjson)
  target_compile_features(chjson_parse_file PRIVATE cxx_std_17)
  chjson_enable_release_clang_perf(chjson_parse_file)

  # JSONTestSuite helper tools for cross-library robustness comparison.
  if(EXISTS "${_CHJSON_NLOHMANN_INC}")
    add_executable(nlohmann_parse_file test/nlohmann_parse_file_main.cpp)
    target_compile_features(nlohmann_parse_file PRIVATE cxx_std_17)
    target_include_directories(nlohmann_parse_file PRIVATE "${_CHJSON_NLOHMANN_INC}")
    chjson_enable_release_clang_perf(nlohmann_parse_file)
  endif()

  if(EXISTS "${_CHJSON_RAPIDJSON_INC}")
    add_executable(rapidjson_parse_file test/rapidjson_parse_file_main.cpp)
    target_compile_features(rapidjson_parse_file PRIVATE cxx_std_17)
    target_include_directories(rapidjson_parse_file PRIVATE "${_CHJSON_RAPIDJSON_INC}")
    chjson_enable_release_clang_perf(rapidjson_parse_file)
  endif()

  if(EXISTS "${_CHJSON_JSONCPP_DIR}/include")
    add_executable(jsoncpp_parse_file test/jsoncpp_parse_file_main.cpp)
    target_compile_features(jsoncpp_parse_file PRIVATE cxx_std_17)
    target_include_directories(jsoncpp_parse_file PRIVATE "${_CHJSON_JSONCPP_DIR}/include")
    chjson_enable_release_clang_perf(jsoncpp_parse_file)
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
  chjson_enable_release_clang_perf(chjson_bench)

  if(CHJSON_BUILD_COMPARE_BENCH)
    add_executable(chjson_compare_bench
      benchmark/compare_main.cpp
    )
    target_link_libraries(chjson_compare_bench PRIVATE chjson)
    target_compile_features(chjson_compare_bench PRIVATE cxx_std_17)
    chjson_enable_release_clang_perf(chjson_compare_bench)
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
