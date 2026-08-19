#include <filesystem>

#include "test.hpp"
#include "vectordb/shard/service.hpp"

VDB_TEST(shard_service_is_idempotent_and_searchable) {
  const auto directory = std::filesystem::temp_directory_path() / "vectordb-shard-service-test";
  std::filesystem::remove_all(directory);
  vectordb::shard::ShardService service(directory);
  service.create_collection({.name = "items", .dimensions = 2, .metric = vectordb::Metric::L2Squared,
                             .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
  const auto first = service.batch_upsert(
      "items", "request-1", {{.id = 7, .generation = 1, .vector = {1.0F, 1.0F}, .payload = {}}});
  const auto repeated = service.batch_upsert(
      "items", "request-1", {{.id = 7, .generation = 1, .vector = {1.0F, 1.0F}, .payload = {}}});
  VDB_REQUIRE(first.committed_index == repeated.committed_index);
  VDB_REQUIRE(first.applied_count == 1U);
  const std::vector<float> query{1.1F, 1.1F};
  VDB_REQUIRE(service.search("items", query, 1).front().id == 7U);
  std::filesystem::remove_all(directory);
}

VDB_TEST(shard_collection_catalog_survives_restart_and_deletes_data) {
  const auto directory = std::filesystem::temp_directory_path() / "vectordb-shard-catalog-test";
  std::filesystem::remove_all(directory);
  {
    vectordb::shard::ShardService service(directory);
    service.create_collection({.name = "catalog", .dimensions = 3, .metric = vectordb::Metric::Cosine,
                               .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
    (void)service.batch_upsert(
        "catalog", "seed", {{.id = 4, .generation = 1, .vector = {1.0F, 0.0F, 0.0F}, .payload = {}}});
    VDB_REQUIRE(service.describe_collection("catalog").live_vectors == 1U);
  }
  {
    vectordb::shard::ShardService recovered(directory);
    const auto collections = recovered.list_collections();
    VDB_REQUIRE(collections.size() == 1U);
    VDB_REQUIRE(collections.front().config.dimensions == 3U);
    VDB_REQUIRE(collections.front().live_vectors == 1U);
    VDB_REQUIRE(recovered.delete_collection("catalog"));
    VDB_REQUIRE(recovered.list_collections().empty());
    VDB_REQUIRE(!std::filesystem::exists(directory / "catalog.wal"));
  }
  std::filesystem::remove_all(directory);
}
