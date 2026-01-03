cmake_minimum_required(VERSION 3.15)
project(chjson LANGUAGES CXX)

option(CHJSON_BUILD_TESTS "Build chjson tests" ON)
option(CHJSON_BUILD_BENCHMARKS "Build chjson benchmarks" ON)
option(CHJSON_BUILD_COMPARE_BENCH "Build comparison benchmark vs json_cpp and jsoncpp" ON)

add_library(chjson INTERFACE)
target_include_directories(chjson INTERFACE ${CMAKE_CURRENT_LIST_DIR}/include)
target_compile_features(chjson INTERFACE cxx_std_17)

if(CHJSON_BUILD_TESTS)
  enable_testing()
  add_executable(chjson_tests
    test/test_main.cpp
  )
  target_link_libraries(chjson_tests PRIVATE chjson)
  target_compile_features(chjson_tests PRIVATE cxx_std_17)
  add_test(NAME chjson_tests COMMAND chjson_tests)
endif()

if(CHJSON_BUILD_BENCHMARKS)
  add_executable(chjson_bench
    benchmark/benchmark_main.cpp
  )
  target_link_libraries(chjson_bench PRIVATE chjson)
  target_compile_features(chjson_bench PRIVATE cxx_std_17)

  if(CHJSON_BUILD_COMPARE_BENCH)
    set(_CHJSON_NLOHMANN_INC "${CMAKE_CURRENT_LIST_DIR}/../json_cpp/include")
    set(_CHJSON_JSONCPP_DIR  "${CMAKE_CURRENT_LIST_DIR}/../jsoncpp")
    set(_CHJSON_RAPIDJSON_INC "${CMAKE_CURRENT_LIST_DIR}/../rapidjson/include")

    if(EXISTS "${_CHJSON_JSONCPP_DIR}/CMakeLists.txt")
      # Avoid jsoncpp's tests and post-build unittests inside our build.
      set(JSONCPP_WITH_TESTS OFF CACHE BOOL "" FORCE)
      set(JSONCPP_WITH_POST_BUILD_UNITTEST OFF CACHE BOOL "" FORCE)
      set(JSONCPP_WITH_EXAMPLE OFF CACHE BOOL "" FORCE)
      set(JSONCPP_WITH_WARNING_AS_ERROR OFF CACHE BOOL "" FORCE)
      set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
      set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)

      add_subdirectory("${_CHJSON_JSONCPP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/jsoncpp-build" EXCLUDE_FROM_ALL)
    endif()

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
