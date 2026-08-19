#include <chrono>
#include <set>

#include "test.hpp"
#include "vectordb/coordinator/directory.hpp"

VDB_TEST(directory_routes_and_cuts_over_only_after_catchup) {
  vectordb::coordinator::ShardDirectory directory(12, 2);
  directory.add_node({.id = "node-a", .endpoint = "node-a:7000", .virtual_nodes = 64});
  directory.add_node({.id = "node-b", .endpoint = "node-b:7000", .virtual_nodes = 64});
  directory.add_node({.id = "node-c", .endpoint = "node-c:7000", .virtual_nodes = 64});
  directory.initialize_placements();
  const auto initial = directory.route("tenant-17");
  VDB_REQUIRE(initial.replicas.size() == 2U);
  VDB_REQUIRE(initial.leader == initial.replicas.front());
  const auto migration = directory.begin_migration(initial.shard, "node-c");
  const auto before_cutover = directory.route("tenant-17");
  VDB_REQUIRE(before_cutover.replicas == initial.replicas);
  directory.mark_caught_up(migration.id);
  directory.cutover(migration.id);
  const auto after_cutover = directory.route("tenant-17");
  VDB_REQUIRE(after_cutover.replicas != initial.replicas);
  VDB_REQUIRE(std::find(after_cutover.replicas.begin(), after_cutover.replicas.end(), "node-c") != after_cutover.replicas.end());
}

VDB_TEST(heartbeat_registry_rejects_stale_incarnations) {
  vectordb::coordinator::HeartbeatRegistry registry;
  registry.record("node-a", 2);
  registry.record("node-a", 1);
  VDB_REQUIRE(registry.is_healthy("node-a", std::chrono::milliseconds(50)));
  VDB_REQUIRE(!registry.is_healthy("node-b", std::chrono::milliseconds(50)));
}

