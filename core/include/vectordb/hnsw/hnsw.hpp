#pragma once

#include <cstdint>
#include <random>
#include <shared_mutex>
#include <span>
#include <optional>
#include <unordered_map>
#include <vector>

#include "vectordb/common/types.hpp"
#include "vectordb/quantization/sq8.hpp"

namespace vectordb {

struct HnswConfig {
  std::uint32_t dimensions{};
  Metric metric{Metric::Cosine};
  std::uint32_t max_neighbors{32};
  std::uint32_t ef_construction{200};
  std::uint64_t random_seed{0x766563746f726462ULL};
};

class HnswIndex {
 public:
  explicit HnswIndex(HnswConfig config);

  void insert(VectorId id, Generation generation, std::span<const float> vector);
  void upsert(VectorId id, Generation generation, std::span<const float> vector);
  void upsert_batch(std::span<const Record> records);
  void deactivate(VectorId id);
  void enable_sq8();
  [[nodiscard]] std::vector<SearchHit> search(std::span<const float> query, std::uint32_t k,
                                               std::uint32_t ef_search = 96) const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool contains(VectorId id) const;
  [[nodiscard]] bool uses_sq8() const;

 private:
  struct Node {
    VectorId id{};
    Generation generation{};
    bool active{true};
    std::vector<float> vector;
    std::vector<std::uint8_t> sq8_code;
    std::vector<std::vector<std::uint32_t>> neighbors;
  };

  [[nodiscard]] std::uint32_t random_level();
  [[nodiscard]] double validate_vector(std::span<const float> vector) const;
  void insert_locked(VectorId id, Generation generation, std::span<const float> vector);
  [[nodiscard]] float exact_distance(std::span<const float> lhs, std::span<const float> rhs) const;
  [[nodiscard]] float node_distance(std::span<const float> query, std::uint32_t node_id) const;
  [[nodiscard]] std::vector<std::uint32_t> search_layer(std::span<const float> query,
                                                         std::vector<std::uint32_t> entry_points,
                                                         std::uint32_t layer, std::uint32_t ef) const;
  [[nodiscard]] std::vector<std::uint32_t> select_neighbors(std::span<const float> query,
                                                             std::span<const std::uint32_t> candidates,
                                                             std::uint32_t maximum) const;
  [[nodiscard]] std::uint32_t maximum_neighbors(std::uint32_t layer) const noexcept;
  void link(std::uint32_t source, std::uint32_t target, std::uint32_t layer);
  void prune_neighbors(std::uint32_t node_id, std::uint32_t layer);

  HnswConfig config_;
  mutable std::shared_mutex mutex_;
  std::vector<Node> nodes_;
  std::unordered_map<VectorId, std::uint32_t> node_by_id_;
  std::uint32_t entry_point_{};
  std::uint32_t max_level_{};
  bool has_entry_point_{};
  std::mt19937_64 random_;
  std::optional<ScalarQuantizer8> quantizer_;
};

}  // namespace vectordb
