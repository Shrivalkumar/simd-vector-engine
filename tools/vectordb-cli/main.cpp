#include <chrono>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

#include "vectordb/distance/distance.hpp"
#include "vectordb/engine/engine.hpp"

namespace {

int demo() {
  // A demo must not inherit an earlier WAL: replaying a prior run would make
  // generation 1 stale and turn a simple smoke check into a false failure.
  const auto run_id = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = std::filesystem::temp_directory_path() /
                         ("vectordb-cli-demo-" + std::to_string(run_id));
  vectordb::Collection collection(
      directory,
      vectordb::CollectionConfig{.name = "documents",
                                 .dimensions = 4,
                                 .metric = vectordb::Metric::Cosine,
                                 .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
  collection.recover();
  collection.upsert({.id = 101,
                     .generation = 1,
                     .vector = {0.9F, 0.1F, 0.0F, 0.0F},
                     .payload = {{.key = "title", .value = std::string{"vector search"}}}});
  collection.upsert({.id = 102,
                     .generation = 1,
                     .vector = {0.0F, 0.0F, 0.9F, 0.1F},
                     .payload = {{.key = "title", .value = std::string{"distributed systems"}}}});
  const std::vector<float> query{0.8F, 0.2F, 0.0F, 0.0F};
  const auto results = collection.search(query, 2U);
  std::cout << "collection=documents records=" << collection.size() << " neon="
            << (vectordb::cpu_uses_neon() ? "enabled" : "unavailable") << '\n';
  for (const auto& result : results) {
    std::cout << "id=" << result.id << " generation=" << result.generation << " distance=" << result.score << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "demo") return demo();
  std::cerr << "usage: vectordb-cli demo\n";
  return 2;
}
