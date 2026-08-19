#include <filesystem>
#include <fstream>

#include "test.hpp"
#include "vectordb/storage/wal.hpp"

VDB_TEST(wal_recovers_durable_entries_and_ignores_torn_tail) {
  const auto path = test::temp_path("wal-recovery");
  std::filesystem::remove(path);
  vectordb::WriteAheadLog wal(path);
  (void)wal.append_upsert({.id = 11,
                           .generation = 1,
                           .vector = {1.0F, 2.0F},
                           .payload = {{.key = "tenant", .value = std::string{"blue"}},
                                       {.key = "priority", .value = std::int64_t{7}}}});
  (void)wal.append_delete(11, 1);
  {
    std::ofstream file(path, std::ios::binary | std::ios::app);
    const char partial[] = {'V', 'D', 'B'};
    file.write(partial, sizeof(partial));
  }
  const auto entries = wal.recover();
  VDB_REQUIRE(entries.size() == 2U);
  VDB_REQUIRE(entries.front().record.id == 11U);
  VDB_REQUIRE(entries.front().record.vector.size() == 2U);
  VDB_REQUIRE(entries.back().operation == vectordb::WalOperation::Delete);
  std::filesystem::remove(path);
}

