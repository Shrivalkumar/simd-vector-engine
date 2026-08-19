#pragma once

#include <filesystem>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "vectordb/common/types.hpp"
#include "vectordb/storage/wal.hpp"

namespace vectordb {

class Segment {
 public:
  Segment(std::filesystem::path wal_path, std::uint32_t dimensions);

  void recover();
  void upsert(Record record);
  void erase(VectorId id, Generation expected_generation);
  [[nodiscard]] std::optional<Record> get(VectorId id) const;
  [[nodiscard]] std::vector<Record> live_records() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] std::uint32_t dimensions() const noexcept { return dimensions_; }

 private:
  void apply_upsert(const Record& record);
  void apply_delete(VectorId id, Generation generation);

  std::uint32_t dimensions_;
  WriteAheadLog wal_;
  mutable std::shared_mutex mutex_;
  std::unordered_map<VectorId, Record> records_;
};

}  // namespace vectordb

