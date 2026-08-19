#include <filesystem>

#include "test.hpp"
#include "vectordb/shard/service.hpp"

VDB_TEST(shard_service_is_idempotent_and_searchable) {
  const auto directory = std::filesystem::temp_directory_path() / "vectordb-shard-service-test";
  std::filesystem::create_directories(directory);
  std::filesystem::remove(directory / "items.wal");
  vectordb::shard::ShardService service(directory);
  service.create_collection({.name = "items", .dimensions = 2, .metric = vectordb::Metric::L2Squared,
                             .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
  const auto first = service.batch_upsert("items", "request-1", {{.id = 7, .generation = 1, .vector = {1.0F, 1.0F}}});
  const auto repeated = service.batch_upsert("items", "request-1", {{.id = 7, .generation = 1, .vector = {1.0F, 1.0F}}});
  VDB_REQUIRE(first.committed_index == repeated.committed_index);
  VDB_REQUIRE(first.applied_count == 1U);
  const std::vector<float> query{1.1F, 1.1F};
  VDB_REQUIRE(service.search("items", query, 1).front().id == 7U);
  std::filesystem::remove(directory / "items.wal");
}

