#include "vectordb/engine/engine.hpp"

#include <stdexcept>

namespace vectordb {

Collection::Collection(std::filesystem::path data_directory, CollectionConfig config)
    : config_(std::move(config)), segment_(data_directory / (config_.name + ".wal"), config_.dimensions) {
  if (config_.name.empty() || config_.dimensions == 0U) throw std::invalid_argument("invalid collection configuration");
  config_.hnsw.dimensions = config_.dimensions;
  config_.hnsw.metric = config_.metric;
  index_ = std::make_unique<HnswIndex>(config_.hnsw);
}

void Collection::recover() {
  std::scoped_lock write_lock(write_mutex_);
  segment_.recover();
  std::unique_lock lock(index_mutex_);
  index_ = std::make_unique<HnswIndex>(config_.hnsw);
  for (const auto& record : segment_.live_records()) index_->insert(record.id, record.generation, record.vector);
}

void Collection::upsert(Record record) {
  std::scoped_lock write_lock(write_mutex_);
  segment_.upsert(record);
  std::shared_lock index_lock(index_mutex_);
  index_->upsert(record.id, record.generation, record.vector);
}

void Collection::erase(VectorId id, Generation expected_generation) {
  std::scoped_lock write_lock(write_mutex_);
  segment_.erase(id, expected_generation);
  std::shared_lock index_lock(index_mutex_);
  index_->deactivate(id);
}

std::optional<Record> Collection::get(VectorId id) const { return segment_.get(id); }

std::vector<SearchHit> Collection::search(std::span<const float> query, std::uint32_t k, std::uint32_t ef_search) const {
  std::shared_lock lock(index_mutex_);
  return index_->search(query, k, ef_search);
}

std::size_t Collection::size() const { return segment_.size(); }

}  // namespace vectordb
