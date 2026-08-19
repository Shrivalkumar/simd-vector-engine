#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <thread>
#include <utility>
#include <vector>

#include "test.hpp"
#include "vectordb/distance/distance.hpp"
#include "vectordb/hnsw/hnsw.hpp"

namespace {

std::vector<float> clustered_vector(std::uint32_t dimensions, std::uint32_t seed, std::uint32_t cluster) {
  std::uint32_t state = seed ^ ((cluster + 1U) * 0x9e3779b1U);
  std::vector<float> vector(dimensions);
  double squared_norm = 0.0;
  for (std::uint32_t index = 0; index < dimensions; ++index) {
    state = state * 1'664'525U + 1'013'904'223U;
    const double random = static_cast<double>(state) / static_cast<double>(UINT32_MAX);
    const double centroid = index % 16U == cluster % 16U ? 0.8 : 0.0;
    const double value = centroid + (random - 0.5) * 0.35;
    vector[index] = static_cast<float>(value);
    squared_norm += value * value;
  }
  const float inverse_norm = static_cast<float>(1.0 / std::sqrt(squared_norm));
  for (auto& value : vector) value *= inverse_norm;
  return vector;
}

}  // namespace

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

VDB_TEST(hnsw_clustered_recall_meets_small_dataset_gate) {
  constexpr std::uint32_t dimensions = 64U;
  constexpr std::size_t vector_count = 128U;
  constexpr std::size_t top_k = 10U;
  vectordb::HnswIndex index({.dimensions = dimensions,
                             .metric = vectordb::Metric::Cosine,
                             .max_neighbors = 32,
                             .ef_construction = 200,
                             .random_seed = 0x766563746f726462ULL});
  std::vector<std::vector<float>> vectors;
  vectors.reserve(vector_count);
  for (std::size_t offset = 0; offset < vector_count; ++offset) {
    vectors.push_back(clustered_vector(dimensions, static_cast<std::uint32_t>(offset + 1U),
                                       static_cast<std::uint32_t>(offset % 16U)));
    index.insert(offset + 1U, 1U, vectors.back());
  }

  std::size_t exact_matches = 0U;
  std::size_t self_hits = 0U;
  for (std::size_t query_index = 0; query_index < vectors.size(); ++query_index) {
    std::vector<std::pair<float, std::uint64_t>> exact;
    exact.reserve(vectors.size());
    for (std::size_t candidate = 0; candidate < vectors.size(); ++candidate) {
      exact.emplace_back(vectordb::cosine_distance(vectors[query_index], vectors[candidate]), candidate + 1U);
    }
    std::partial_sort(exact.begin(), exact.begin() + top_k, exact.end());
    std::unordered_set<std::uint64_t> expected;
    for (std::size_t rank = 0; rank < top_k; ++rank) expected.insert(exact[rank].second);

    const auto approximate = index.search(vectors[query_index], top_k, 96U);
    self_hits += static_cast<std::size_t>(std::any_of(approximate.begin(), approximate.end(), [query_index](const auto& hit) {
      return hit.id == query_index + 1U;
    }));
    for (const auto& hit : approximate) exact_matches += static_cast<std::size_t>(expected.contains(hit.id));
  }
  const double recall = static_cast<double>(exact_matches) / static_cast<double>(vector_count * top_k);
  const double self_hit_rate = static_cast<double>(self_hits) / static_cast<double>(vector_count);
  VDB_REQUIRE(recall >= 0.95);
  VDB_REQUIRE(self_hit_rate >= 0.98);
}

VDB_TEST(hnsw_concurrent_queries_and_ingestion_are_safe) {
  constexpr std::uint32_t dimensions = 32U;
  vectordb::HnswIndex index({.dimensions = dimensions,
                             .metric = vectordb::Metric::Cosine,
                             .max_neighbors = 16,
                             .ef_construction = 96,
                             .random_seed = 17U});
  std::vector<std::vector<float>> vectors;
  vectors.reserve(128U);
  for (std::uint32_t offset = 0; offset < 128U; ++offset) {
    vectors.push_back(clustered_vector(dimensions, offset + 1U, offset % 8U));
  }
  for (std::uint64_t id = 1U; id <= 64U; ++id) index.insert(id, 1U, vectors[id - 1U]);

  std::atomic<bool> start{false};
  std::atomic<std::uint32_t> failures{0U};
  std::thread writer([&] {
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    try {
      for (std::uint64_t id = 65U; id <= 128U; ++id) index.insert(id, 1U, vectors[id - 1U]);
    } catch (...) {
      failures.fetch_add(1U, std::memory_order_relaxed);
    }
  });
  std::vector<std::thread> readers;
  for (std::uint32_t reader = 0; reader < 4U; ++reader) {
    readers.emplace_back([&, reader] {
      while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
      try {
        for (std::uint32_t iteration = 0; iteration < 200U; ++iteration) {
          const auto& query = vectors[(iteration + reader * 11U) % vectors.size()];
          if (index.search(query, 10U, 64U).empty()) failures.fetch_add(1U, std::memory_order_relaxed);
        }
      } catch (...) {
        failures.fetch_add(1U, std::memory_order_relaxed);
      }
    });
  }
  start.store(true, std::memory_order_release);
  writer.join();
  for (auto& reader : readers) reader.join();
  VDB_REQUIRE(failures.load(std::memory_order_relaxed) == 0U);
  VDB_REQUIRE(index.size() == 128U);
}
