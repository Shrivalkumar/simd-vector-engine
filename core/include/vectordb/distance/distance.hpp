#pragma once

#include <span>

#include "vectordb/common/types.hpp"

namespace vectordb {

[[nodiscard]] float l2_squared(std::span<const float> lhs, std::span<const float> rhs);
[[nodiscard]] float dot_product(std::span<const float> lhs, std::span<const float> rhs);
[[nodiscard]] float cosine_distance(std::span<const float> lhs, std::span<const float> rhs);
[[nodiscard]] float distance(std::span<const float> lhs, std::span<const float> rhs, Metric metric);
[[nodiscard]] bool cpu_uses_neon() noexcept;

}  // namespace vectordb

