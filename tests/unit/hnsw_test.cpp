#include <vector>

#include "test.hpp"
#include "vectordb/hnsw/hnsw.hpp"

VDB_TEST(hnsw_returns_nearest_neighbor) {
  vectordb::HnswIndex index({.dimensions = 2,
                             .metric = vectordb::Metric::L2Squared,
                             .max_neighbors = 8,
                             .ef_construction = 64,
                             .random_seed = 7});
  for (std::uint64_t id = 1; id <= 100U; ++id) {
    const std::vector<float> vector{static_cast<float>(id), static_cast<float>(id % 7U)};
    index.insert(id, 1, vector);
  }
  const std::vector<float> query{41.1F, 6.0F};
  const auto hits = index.search(query, 3, 64);
  VDB_REQUIRE(hits.size() == 3U);
  VDB_REQUIRE(hits.front().id == 41U);
  VDB_REQUIRE(hits.front().score < 0.02F);
}
