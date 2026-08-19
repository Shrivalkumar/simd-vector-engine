#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "vectordb/common/types.hpp"
#include "vectordb/hnsw/hnsw.hpp"
#include "vectordb/segment/segment.hpp"

namespace vectordb {

struct CollectionConfig {
  std::string name;
  std::uint32_t dimensions{};
  Metric metric{Metric::Cosine};
  HnswConfig hnsw{};
};

class Collection {
 public:
  Collection(std::filesystem::path data_directory, CollectionConfig config);

  void recover();
  void upsert(Record record);
  void erase(VectorId id, Generation expected_generation);
  [[nodiscard]] std::optional<Record> get(VectorId id) const;
  [[nodiscard]] std::vector<SearchHit> search(std::span<const float> query, std::uint32_t k,
                                               std::uint32_t ef_search = 96) const;
  [[nodiscard]] std::size_t size() const;

 private:
  CollectionConfig config_;
  Segment segment_;
  mutable std::mutex write_mutex_;
  mutable std::shared_mutex index_mutex_;
  std::unique_ptr<HnswIndex> index_;
};

}  // namespace vectordb
