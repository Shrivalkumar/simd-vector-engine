#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

#include "vectordb/common/types.hpp"

namespace vectordb {

class ScalarQuantizer8 {
 public:
  ScalarQuantizer8() = default;

  void train(std::span<const std::vector<float>> vectors) {
    if (vectors.empty() || vectors.front().empty()) throw std::invalid_argument("SQ8 needs non-empty training vectors");
    dimensions_ = static_cast<std::uint32_t>(vectors.front().size());
    minimum_.assign(dimensions_, std::numeric_limits<float>::infinity());
    scale_.assign(dimensions_, 1.0F);
    std::vector<float> maximum(dimensions_, -std::numeric_limits<float>::infinity());
    for (const auto& vector : vectors) {
      if (vector.size() != dimensions_) throw std::invalid_argument("SQ8 training dimensions differ");
      for (std::uint32_t dimension = 0; dimension < dimensions_; ++dimension) {
        minimum_[dimension] = std::min(minimum_[dimension], vector[dimension]);
        maximum[dimension] = std::max(maximum[dimension], vector[dimension]);
      }
    }
    for (std::uint32_t dimension = 0; dimension < dimensions_; ++dimension) {
      const float range = maximum[dimension] - minimum_[dimension];
      scale_[dimension] = range > std::numeric_limits<float>::epsilon() ? range / 255.0F : 1.0F;
    }
  }

  [[nodiscard]] std::vector<std::uint8_t> encode(std::span<const float> vector) const {
    validate(vector);
    std::vector<std::uint8_t> code(dimensions_);
    for (std::uint32_t dimension = 0; dimension < dimensions_; ++dimension) {
      const float quantized = std::round((vector[dimension] - minimum_[dimension]) / scale_[dimension]);
      code[dimension] = static_cast<std::uint8_t>(std::clamp(quantized, 0.0F, 255.0F));
    }
    return code;
  }

  [[nodiscard]] float approximate_distance(std::span<const float> query, std::span<const std::uint8_t> code,
                                           Metric metric) const {
    validate(query);
    if (code.size() != dimensions_) throw std::invalid_argument("SQ8 code dimensions differ");
    float l2 = 0.0F;
    float dot = 0.0F;
    float query_norm = 0.0F;
    float code_norm = 0.0F;
    for (std::uint32_t dimension = 0; dimension < dimensions_; ++dimension) {
      const float reconstructed = minimum_[dimension] + scale_[dimension] * static_cast<float>(code[dimension]);
      const float delta = query[dimension] - reconstructed;
      l2 = std::fma(delta, delta, l2);
      dot = std::fma(query[dimension], reconstructed, dot);
      query_norm = std::fma(query[dimension], query[dimension], query_norm);
      code_norm = std::fma(reconstructed, reconstructed, code_norm);
    }
    switch (metric) {
      case Metric::L2Squared: return l2;
      case Metric::DotProduct: return -dot;
      case Metric::Cosine:
        if (query_norm <= 0.0F || code_norm <= 0.0F) return std::numeric_limits<float>::infinity();
        return 1.0F - dot / std::sqrt(query_norm * code_norm);
    }
    throw std::invalid_argument("unsupported SQ8 metric");
  }

  [[nodiscard]] std::uint32_t dimensions() const noexcept { return dimensions_; }

 private:
  void validate(std::span<const float> vector) const {
    if (dimensions_ == 0U || vector.size() != dimensions_) throw std::invalid_argument("SQ8 dimensions differ");
  }

  std::uint32_t dimensions_{};
  std::vector<float> minimum_;
  std::vector<float> scale_;
};

}  // namespace vectordb

