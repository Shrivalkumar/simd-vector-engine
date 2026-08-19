#pragma once

#include <cmath>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace test {

struct Case {
  std::string name;
  std::function<void()> function;
};

inline std::vector<Case>& cases() {
  static std::vector<Case> value;
  return value;
}

struct Register {
  Register(std::string name, std::function<void()> function) { cases().push_back({std::move(name), std::move(function)}); }
};

inline std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("vectordb-" + name + "-" + std::to_string(std::hash<std::string>{}(name)) + ".wal");
}

inline void require(bool condition, const char* expression) {
  if (!condition) throw std::runtime_error(std::string("requirement failed: ") + expression);
}

inline void require_near(float actual, float expected, float epsilon, const char* expression) {
  if (std::fabs(actual - expected) > epsilon) {
    throw std::runtime_error(std::string("requirement failed: ") + expression);
  }
}

}  // namespace test

#define VDB_TEST(name)                           \
  static void name();                            \
  static const test::Register name##_registration{#name, name}; \
  static void name()

#define VDB_REQUIRE(expression) test::require((expression), #expression)
#define VDB_REQUIRE_NEAR(actual, expected, epsilon) test::require_near((actual), (expected), (epsilon), #actual " ~= " #expected)

