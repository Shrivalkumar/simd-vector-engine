#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "vectordb/engine/engine.hpp"

namespace vectordb::shard {

struct BatchUpsertResult {
  std::uint64_t committed_index{};
  std::uint32_t applied_count{};
};

class ShardService {
 public:
  explicit ShardService(std::filesystem::path data_directory);

  void create_collection(CollectionConfig config);
  [[nodiscard]] BatchUpsertResult batch_upsert(const std::string& collection, const std::string& idempotency_key,
                                                std::vector<Record> records);
  [[nodiscard]] std::vector<SearchHit> search(const std::string& collection, std::span<const float> query,
                                               std::uint32_t k, std::uint32_t ef_search = 96) const;
  void erase(const std::string& collection, VectorId id, Generation generation);
  [[nodiscard]] std::optional<Record> get(const std::string& collection, VectorId id) const;

 private:
  [[nodiscard]] std::shared_ptr<Collection> collection_for(const std::string& collection) const;

  std::filesystem::path data_directory_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Collection>> collections_;
  std::unordered_map<std::string, BatchUpsertResult> completed_requests_;
  std::uint64_t committed_index_{};
};

}  // namespace vectordb::shard
