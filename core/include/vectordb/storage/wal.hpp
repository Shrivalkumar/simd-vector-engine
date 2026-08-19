#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
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
  [[nodiscard]] std::uint64_t append_delete(VectorId id, Generation generation);
  [[nodiscard]] std::vector<WalEntry> recover() const;
  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::uint64_t append(WalOperation operation, const Record& record);

  std::filesystem::path path_;
  mutable std::mutex mutex_;
  std::uint64_t next_lsn_{1};
};

}  // namespace vectordb

