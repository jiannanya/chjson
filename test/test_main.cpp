#include "test_common.hpp"

#include <iostream>

void test_numbers();
void test_strings();
void test_structure();
void test_errors();
void test_dom();
void test_random();
void test_regressions();

int main() {
  test_numbers();
  test_strings();
  test_structure();
  test_errors();
  test_dom();
  test_random();
  test_regressions();

  std::cout << "chjson tests passed\n";
  return 0;
}
