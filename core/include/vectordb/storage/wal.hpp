#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "vectordb/common/types.hpp"

namespace vectordb {

enum class WalOperation : std::uint8_t { Upsert = 1, Delete = 2 };

struct WalEntry {
  std::uint64_t lsn{};
  WalOperation operation{};
  Record record;
};

class WriteAheadLog {
 public:
  explicit WriteAheadLog(std::filesystem::path path);
  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;

  [[nodiscard]] std::uint64_t append_upsert(const Record& record);
  [[nodiscard]] std::uint64_t append_upserts(std::span<const Record> records);
  [[nodiscard]] std::uint64_t append_delete(VectorId id, Generation generation);
  [[nodiscard]] std::vector<WalEntry> recover() const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::uint64_t append(WalOperation operation, const Record& record);
  std::uint64_t append_frames(std::span<const std::pair<WalOperation, const Record*>> records);

  std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::uint64_t next_lsn_{1};
};

}  // namespace vectordb
