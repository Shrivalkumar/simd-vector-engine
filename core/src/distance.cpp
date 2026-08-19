#include "vectordb/distance/distance.hpp"

#include <cmath>
#include <stdexcept>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace vectordb {
namespace {

void validate_dimensions(std::span<const float> lhs, std::span<const float> rhs) {
  if (lhs.size() != rhs.size()) throw std::invalid_argument("vector dimensions differ");
}

[[maybe_unused]] float l2_scalar(std::span<const float> lhs, std::span<const float> rhs) {
  float accumulator = 0.0F;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    const float delta = lhs[index] - rhs[index];
    accumulator = std::fma(delta, delta, accumulator);
  }
  return accumulator;
}

[[maybe_unused]] float dot_scalar(std::span<const float> lhs, std::span<const float> rhs) {
  float accumulator = 0.0F;
  for (std::size_t index = 0; index < lhs.size(); ++index) {
    accumulator = std::fma(lhs[index], rhs[index], accumulator);
  }
  return accumulator;
}

#if defined(__aarch64__) || defined(__ARM_NEON)
float l2_neon(std::span<const float> lhs, std::span<const float> rhs) {
  std::size_t index = 0;
  float32x4_t acc0 = vdupq_n_f32(0.0F);
  float32x4_t acc1 = vdupq_n_f32(0.0F);
  float32x4_t acc2 = vdupq_n_f32(0.0F);
  float32x4_t acc3 = vdupq_n_f32(0.0F);
  for (; index + 16U <= lhs.size(); index += 16U) {
    const float32x4_t a0 = vld1q_f32(lhs.data() + index);
    const float32x4_t b0 = vld1q_f32(rhs.data() + index);
    const float32x4_t a1 = vld1q_f32(lhs.data() + index + 4U);
    const float32x4_t b1 = vld1q_f32(rhs.data() + index + 4U);
    const float32x4_t a2 = vld1q_f32(lhs.data() + index + 8U);
    const float32x4_t b2 = vld1q_f32(rhs.data() + index + 8U);
    const float32x4_t a3 = vld1q_f32(lhs.data() + index + 12U);
    const float32x4_t b3 = vld1q_f32(rhs.data() + index + 12U);
    const float32x4_t d0 = vsubq_f32(a0, b0);
    const float32x4_t d1 = vsubq_f32(a1, b1);
    const float32x4_t d2 = vsubq_f32(a2, b2);
    const float32x4_t d3 = vsubq_f32(a3, b3);
    acc0 = vfmaq_f32(acc0, d0, d0);
    acc1 = vfmaq_f32(acc1, d1, d1);
    acc2 = vfmaq_f32(acc2, d2, d2);
    acc3 = vfmaq_f32(acc3, d3, d3);
  }
  float total = vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3)));
  for (; index < lhs.size(); ++index) {
    const float delta = lhs[index] - rhs[index];
    total = std::fma(delta, delta, total);
  }
  return total;
}

float dot_neon(std::span<const float> lhs, std::span<const float> rhs) {
  std::size_t index = 0;
  float32x4_t acc0 = vdupq_n_f32(0.0F);
  float32x4_t acc1 = vdupq_n_f32(0.0F);
  float32x4_t acc2 = vdupq_n_f32(0.0F);
  float32x4_t acc3 = vdupq_n_f32(0.0F);
  for (; index + 16U <= lhs.size(); index += 16U) {
    acc0 = vfmaq_f32(acc0, vld1q_f32(lhs.data() + index), vld1q_f32(rhs.data() + index));
    acc1 = vfmaq_f32(acc1, vld1q_f32(lhs.data() + index + 4U), vld1q_f32(rhs.data() + index + 4U));
    acc2 = vfmaq_f32(acc2, vld1q_f32(lhs.data() + index + 8U), vld1q_f32(rhs.data() + index + 8U));
    acc3 = vfmaq_f32(acc3, vld1q_f32(lhs.data() + index + 12U), vld1q_f32(rhs.data() + index + 12U));
  }
  float total = vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3)));
  for (; index < lhs.size(); ++index) total = std::fma(lhs[index], rhs[index], total);
  return total;
}
#endif

}  // namespace

float l2_squared(std::span<const float> lhs, std::span<const float> rhs) {
  validate_dimensions(lhs, rhs);
#if defined(__aarch64__) || defined(__ARM_NEON)
  return l2_neon(lhs, rhs);
#else
  return l2_scalar(lhs, rhs);
#endif
}

float dot_product(std::span<const float> lhs, std::span<const float> rhs) {
  validate_dimensions(lhs, rhs);
#if defined(__aarch64__) || defined(__ARM_NEON)
  return dot_neon(lhs, rhs);
#else
  return dot_scalar(lhs, rhs);
#endif
}

float cosine_distance(std::span<const float> lhs, std::span<const float> rhs) {
  validate_dimensions(lhs, rhs);
  const float lhs_norm = dot_product(lhs, lhs);
  const float rhs_norm = dot_product(rhs, rhs);
  if (lhs_norm <= 0.0F || rhs_norm <= 0.0F) {
    throw std::invalid_argument("cosine distance is undefined for a zero vector");
  }
  return 1.0F - dot_product(lhs, rhs) / std::sqrt(lhs_norm * rhs_norm);
}

float distance(std::span<const float> lhs, std::span<const float> rhs, Metric metric) {
  switch (metric) {
    case Metric::L2Squared: return l2_squared(lhs, rhs);
    case Metric::DotProduct: return -dot_product(lhs, rhs);
    case Metric::Cosine: return cosine_distance(lhs, rhs);
  }
  throw std::invalid_argument("unsupported metric");
}

bool cpu_uses_neon() noexcept {
#if defined(__aarch64__) || defined(__ARM_NEON)
  return true;
#else
  return false;
#endif
}

}  // namespace vectordb
