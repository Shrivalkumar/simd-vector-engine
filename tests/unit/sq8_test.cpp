#include <vector>

#include "test.hpp"
#include "vectordb/hnsw/hnsw.hpp"
#include "vectordb/quantization/sq8.hpp"

VDB_TEST(sq8_preserves_nearest_l2_ordering) {
  const std::vector<std::vector<float>> training{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}, {0.8F, 0.2F, 0.4F}};
  vectordb::ScalarQuantizer8 quantizer;
  quantizer.train(training);
  const auto near_code = quantizer.encode(training[2]);
  const auto far_code = quantizer.encode(training[0]);
  const std::vector<float> query{0.79F, 0.21F, 0.39F};
  VDB_REQUIRE(quantizer.approximate_distance(query, near_code, vectordb::Metric::L2Squared) <
              quantizer.approximate_distance(query, far_code, vectordb::Metric::L2Squared));
}

VDB_TEST(hnsw_sq8_traversal_reranks_with_raw_vectors) {
  vectordb::HnswIndex index({.dimensions = 2, .metric = vectordb::Metric::L2Squared, .max_neighbors = 8, .ef_construction = 64});
  for (std::uint64_t id = 1; id <= 64U; ++id) {
    const std::vector<float> vector{static_cast<float>(id), static_cast<float>(id % 5U)};
    index.insert(id, 1, vector);
  }
  index.enable_sq8();
  VDB_REQUIRE(index.uses_sq8());
  const std::vector<float> query{23.05F, 3.0F};
  const auto hits = index.search(query, 1, 64);
  VDB_REQUIRE(hits.front().id == 23U);
}
