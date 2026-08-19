#include <stdexcept>
#include <vector>

#include "test.hpp"
#include "vectordb/distance/distance.hpp"

VDB_TEST(distance_l2_dot_and_cosine) {
  const std::vector<float> left{1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
  const std::vector<float> right{2.0F, 3.0F, 4.0F, 5.0F, 6.0F};
  VDB_REQUIRE_NEAR(vectordb::l2_squared(left, right), 5.0F, 1e-5F);
  VDB_REQUIRE_NEAR(vectordb::dot_product(left, right), 70.0F, 1e-5F);
  VDB_REQUIRE_NEAR(vectordb::cosine_distance(left, left), 0.0F, 1e-5F);
}

VDB_TEST(distance_rejects_bad_vectors) {
  bool dimension_error = false;
  try {
    (void)vectordb::l2_squared(std::vector<float>{1.0F}, std::vector<float>{1.0F, 2.0F});
  } catch (const std::invalid_argument&) {
    dimension_error = true;
  }
  VDB_REQUIRE(dimension_error);
  bool zero_error = false;
  try {
    (void)vectordb::cosine_distance(std::vector<float>{0.0F}, std::vector<float>{1.0F});
  } catch (const std::invalid_argument&) {
    zero_error = true;
  }
  VDB_REQUIRE(zero_error);
}

