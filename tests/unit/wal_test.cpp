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

VDB_TEST(wal_group_commit_preserves_every_record_and_lsn) {
  const auto path = test::temp_path("wal-group-commit");
  std::filesystem::remove(path);
  vectordb::WriteAheadLog wal(path);
  const std::vector<vectordb::Record> records{
      {.id = 1, .generation = 1, .vector = {1.0F, 0.0F}, .payload = {}},
      {.id = 2, .generation = 1, .vector = {0.0F, 1.0F}, .payload = {}},
      {.id = 3, .generation = 1, .vector = {0.5F, 0.5F}, .payload = {}},
  };
  VDB_REQUIRE(wal.append_upserts(records) == 3U);
  VDB_REQUIRE(wal.append_upsert({.id = 4, .generation = 1, .vector = {0.25F, 0.75F}, .payload = {}}) == 4U);
  const auto recovered = wal.recover();
  VDB_REQUIRE(recovered.size() == 4U);
  for (std::size_t index = 0; index < recovered.size(); ++index) {
    VDB_REQUIRE(recovered[index].lsn == index + 1U);
    VDB_REQUIRE(recovered[index].record.id == index + 1U);
  }
  std::filesystem::remove(path);
}
