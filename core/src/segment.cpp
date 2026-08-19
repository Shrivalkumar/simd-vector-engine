#include "vectordb/segment/segment.hpp"

#include <stdexcept>

namespace vectordb {

Segment::Segment(std::filesystem::path wal_path, std::uint32_t dimensions)
    : dimensions_(dimensions), wal_(std::move(wal_path)) {
  if (dimensions_ == 0U) throw std::invalid_argument("segment dimensions must be positive");
}

void Segment::recover() {
  std::unique_lock lock(mutex_);
  records_.clear();
  for (const auto& entry : wal_.recover()) {
    if (entry.operation == WalOperation::Upsert) {
      if (entry.record.vector.size() != dimensions_) throw std::runtime_error("WAL dimension mismatch");
      apply_upsert(entry.record);
    } else {
      apply_delete(entry.record.id, entry.record.generation);
    }
  }
}

void Segment::upsert(Record record) {
  if (record.id == 0U || record.generation == 0U) throw std::invalid_argument("id and generation must be non-zero");
  if (record.vector.size() != dimensions_) throw std::invalid_argument("record dimension mismatch");
  std::unique_lock lock(mutex_);
  if (const auto found = records_.find(record.id); found != records_.end() && found->second.generation >= record.generation) {
    throw std::invalid_argument("record generation is stale");
  }
  (void)wal_.append_upsert(record);
  apply_upsert(record);
}

void Segment::erase(VectorId id, Generation expected_generation) {
  if (id == 0U || expected_generation == 0U) throw std::invalid_argument("id and generation must be non-zero");
  std::unique_lock lock(mutex_);
  const auto found = records_.find(id);
  if (found == records_.end()) throw std::out_of_range("record does not exist");
  if (found->second.generation != expected_generation) throw std::invalid_argument("record generation is stale");
  (void)wal_.append_delete(id, expected_generation);
  apply_delete(id, expected_generation);
}

std::optional<Record> Segment::get(VectorId id) const {
  std::shared_lock lock(mutex_);
  const auto found = records_.find(id);
  if (found == records_.end()) return std::nullopt;
  return found->second;
}

std::vector<Record> Segment::live_records() const {
  std::shared_lock lock(mutex_);
  std::vector<Record> result;
  result.reserve(records_.size());
  for (const auto& [id, record] : records_) {
    (void)id;
    result.push_back(record);
  }
  return result;
}

std::size_t Segment::size() const {
  std::shared_lock lock(mutex_);
  return records_.size();
}

void Segment::apply_upsert(const Record& record) { records_.insert_or_assign(record.id, record); }

void Segment::apply_delete(VectorId id, Generation generation) {
  const auto found = records_.find(id);
  if (found != records_.end() && found->second.generation <= generation) records_.erase(found);
}

}  // namespace vectordb

