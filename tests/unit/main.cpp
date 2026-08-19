#include <exception>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "test.hpp"

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;
  std::size_t failures = 0;
  const bool skip_network = std::getenv("VDB_TEST_SKIP_NETWORK") != nullptr;
  const char* only_test = std::getenv("VDB_TEST_ONLY");
  for (const auto& test_case : test::cases()) {
    if (only_test != nullptr && test_case.name != only_test) continue;
    if (skip_network && test_case.name.find("grpc_") != std::string::npos) {
      std::cout << "SKIP " << test_case.name << '\n';
      continue;
    }
    try {
      test_case.function();
      std::cout << "PASS " << test_case.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test_case.name << ": " << error.what() << '\n';
    }
  }
  std::cout << "tests=" << test::cases().size() << " failures=" << failures << '\n';
  return failures == 0U ? 0 : 1;
}
