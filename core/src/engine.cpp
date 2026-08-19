#include "vectordb/engine/engine.hpp"

#include <cmath>
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
  upsert_batch(std::span<const Record>(&record, 1U));
}

void Collection::upsert_batch(std::span<const Record> records) {
  if (records.empty()) return;
  for (const auto& record : records) {
    if (record.vector.size() != config_.dimensions) throw std::invalid_argument("record dimension mismatch");
    double squared_norm = 0.0;
    for (const float value : record.vector) {
      if (!std::isfinite(value)) throw std::invalid_argument("record vector contains a non-finite value");
      const auto wide_value = static_cast<double>(value);
      squared_norm = std::fma(wide_value, wide_value, squared_norm);
    }
    if (config_.metric == Metric::Cosine && squared_norm <= 0.0) {
      throw std::invalid_argument("cosine distance is undefined for a zero vector");
    }
  }
  std::scoped_lock write_lock(write_mutex_);
  segment_.upsert_batch(records);
  std::shared_lock index_lock(index_mutex_);
  index_->upsert_batch(records);
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
