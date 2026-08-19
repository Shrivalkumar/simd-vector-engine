#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace vectordb {

using VectorId = std::uint64_t;
using Generation = std::uint64_t;
using PayloadValue = std::variant<std::string, std::int64_t, double, bool>;

struct PayloadField {
  std::string key;
  PayloadValue value;
};

struct Record {
  VectorId id{};
  Generation generation{};
  std::vector<float> vector;
  std::vector<PayloadField> payload;
};

enum class Metric : std::uint8_t { L2Squared, DotProduct, Cosine };

struct SearchHit {
  VectorId id{};
  Generation generation{};
  float score{};
};

}  // namespace vectordb

