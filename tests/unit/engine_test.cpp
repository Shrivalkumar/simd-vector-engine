#include <filesystem>

#include "test.hpp"
#include "vectordb/engine/engine.hpp"

VDB_TEST(collection_persists_crud_and_search) {
  const auto directory = std::filesystem::temp_directory_path() / "vectordb-engine-test";
  std::filesystem::create_directories(directory);
  const auto wal_path = directory / "vectors.wal";
  std::filesystem::remove(wal_path);

  const vectordb::CollectionConfig config{.name = "vectors",
                                           .dimensions = 3,
                                           .metric = vectordb::Metric::Cosine,
                                           .hnsw = {.max_neighbors = 8, .ef_construction = 32, .random_seed = 4}};
  {
    vectordb::Collection collection(directory, config);
    collection.recover();
    collection.upsert({.id = 1, .generation = 1, .vector = {1.0F, 0.0F, 0.0F}, .payload = {}});
    collection.upsert({.id = 2, .generation = 1, .vector = {0.0F, 1.0F, 0.0F}, .payload = {}});
    const std::vector<float> first_query{0.9F, 0.1F, 0.0F};
    VDB_REQUIRE(collection.search(first_query, 1).front().id == 1U);
    collection.upsert({.id = 1, .generation = 2, .vector = {0.0F, 0.0F, 1.0F}, .payload = {}});
    const std::vector<float> replacement_query{0.0F, 0.0F, 1.0F};
    VDB_REQUIRE(collection.search(replacement_query, 1).front().generation == 2U);
    collection.erase(2, 1);
    VDB_REQUIRE(collection.size() == 1U);
  }
  {
    vectordb::Collection collection(directory, config);
    collection.recover();
    VDB_REQUIRE(collection.size() == 1U);
    VDB_REQUIRE(collection.get(1).has_value());
    VDB_REQUIRE(!collection.get(2).has_value());
    const std::vector<float> second_query{1.0F, 0.0F, 0.0F};
    VDB_REQUIRE(collection.search(second_query, 1).front().id == 1U);
  }
  std::filesystem::remove(wal_path);
}

VDB_TEST(collection_rejects_invalid_batch_before_wal_commit) {
  const auto directory = test::temp_path("invalid-batch").parent_path() / "vectordb-invalid-batch";
  std::filesystem::remove_all(directory);
  const vectordb::CollectionConfig config{.name = "records",
                                          .dimensions = 2,
                                          .metric = vectordb::Metric::Cosine,
                                          .hnsw = {.max_neighbors = 8, .ef_construction = 32}};
  {
    vectordb::Collection collection(directory, config);
    const std::vector<vectordb::Record> records{
        {.id = 1, .generation = 1, .vector = {1.0F, 0.0F}, .payload = {}},
        {.id = 2, .generation = 1, .vector = {0.0F, 0.0F}, .payload = {}},
    };
    bool rejected = false;
    try {
      collection.upsert_batch(records);
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    VDB_REQUIRE(rejected);
    VDB_REQUIRE(collection.size() == 0U);
  }
  {
    vectordb::Collection recovered(directory, config);
    recovered.recover();
    VDB_REQUIRE(recovered.size() == 0U);
  }
  std::filesystem::remove_all(directory);
}
