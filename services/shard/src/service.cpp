#include "vectordb/shard/service.hpp"

#include <stdexcept>

namespace vectordb::shard {

ShardService::ShardService(std::filesystem::path data_directory) : data_directory_(std::move(data_directory)) {
  std::filesystem::create_directories(data_directory_);
}

void ShardService::create_collection(CollectionConfig config) {
  std::scoped_lock lock(mutex_);
  if (collections_.contains(config.name)) throw std::invalid_argument("collection already exists");
  auto collection = std::make_shared<Collection>(data_directory_, config);
  collection->recover();
  collections_.emplace(config.name, std::move(collection));
}

BatchUpsertResult ShardService::batch_upsert(const std::string& collection, const std::string& idempotency_key,
                                             std::vector<Record> records) {
  if (idempotency_key.empty()) throw std::invalid_argument("idempotency key is required");
  std::scoped_lock lock(mutex_);
  const std::string request_key = collection + ':' + idempotency_key;
  if (const auto completed = completed_requests_.find(request_key); completed != completed_requests_.end()) {
    return completed->second;
  }
  const auto target = collection_for(collection);
  for (auto& record : records) target->upsert(std::move(record));
  const BatchUpsertResult result{.committed_index = ++committed_index_,
                                 .applied_count = static_cast<std::uint32_t>(records.size())};
  completed_requests_.emplace(request_key, result);
  return result;
}

std::vector<SearchHit> ShardService::search(const std::string& collection, std::span<const float> query,
                                             std::uint32_t k, std::uint32_t ef_search) const {
  std::shared_lock lock(mutex_);
  return collection_for(collection)->search(query, k, ef_search);
}

void ShardService::erase(const std::string& collection, VectorId id, Generation generation) {
  std::scoped_lock lock(mutex_);
  collection_for(collection)->erase(id, generation);
  ++committed_index_;
}

std::optional<Record> ShardService::get(const std::string& collection, VectorId id) const {
  std::shared_lock lock(mutex_);
  return collection_for(collection)->get(id);
}

std::shared_ptr<Collection> ShardService::collection_for(const std::string& collection) const {
  const auto found = collections_.find(collection);
  if (found == collections_.end()) throw std::out_of_range("collection does not exist");
  return found->second;
}

}  // namespace vectordb::shard
