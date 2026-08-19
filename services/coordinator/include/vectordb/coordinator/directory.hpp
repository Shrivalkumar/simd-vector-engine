#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vectordb::coordinator {

using LogicalShardId = std::uint64_t;

enum class NodeState : std::uint8_t { Active, Draining, Unhealthy };
enum class MigrationPhase : std::uint8_t { Copying, CatchingUp, ReadyToCutover };

struct NodeDescriptor {
  std::string id;
  std::string endpoint;
  std::uint32_t virtual_nodes{128};
  NodeState state{NodeState::Active};
};

struct ShardPlacement {
  LogicalShardId shard{};
  std::vector<std::string> replicas;
  std::string leader;
};

struct Route {
  std::uint64_t directory_epoch{};
  LogicalShardId shard{};
  std::string leader;
  std::vector<std::string> replicas;
};

struct Migration {
  std::string id;
  LogicalShardId shard{};
  std::string source;
  std::string destination;
  MigrationPhase phase{MigrationPhase::Copying};
};

class ShardDirectory {
 public:
  ShardDirectory(std::uint64_t logical_shard_count, std::uint32_t replication_factor);

  void add_node(NodeDescriptor node);
  void set_node_state(const std::string& node_id, NodeState state);
  void initialize_placements();
  [[nodiscard]] Route route(const std::string& partition_key) const;
  [[nodiscard]] std::vector<ShardPlacement> placements() const;
  [[nodiscard]] std::uint64_t epoch() const;

  [[nodiscard]] Migration begin_migration(LogicalShardId shard, const std::string& destination);
  void mark_caught_up(const std::string& migration_id);
  void cutover(const std::string& migration_id);
  [[nodiscard]] std::optional<Migration> migration(const std::string& migration_id) const;

 private:
  struct Token {
    std::uint64_t value{};
    std::string node;
    [[nodiscard]] bool operator<(const Token& other) const noexcept { return value < other.value; }
  };

  [[nodiscard]] static std::uint64_t hash(std::string_view value) noexcept;
  [[nodiscard]] std::vector<std::string> replicas_for(std::string_view key) const;
  [[nodiscard]] std::map<std::uint64_t, Token>::const_iterator ring_successor(std::uint64_t value) const;
  void rebuild_ring_locked();
  void require_initialized_locked() const;
  void increment_epoch_locked();

  std::uint64_t logical_shard_count_;
  std::uint32_t replication_factor_;
  mutable std::mutex mutex_;
  std::uint64_t epoch_{1};
  std::unordered_map<std::string, NodeDescriptor> nodes_;
  std::map<std::uint64_t, Token> ring_;
  std::unordered_map<LogicalShardId, ShardPlacement> placements_;
  std::unordered_map<std::string, Migration> migrations_;
  std::uint64_t next_migration_id_{1};
};

class HeartbeatRegistry {
 public:
  void record(const std::string& node_id, std::uint64_t incarnation);
  [[nodiscard]] bool is_healthy(const std::string& node_id, std::chrono::milliseconds timeout) const;

 private:
  struct LastSeen {
    std::uint64_t incarnation{};
    std::chrono::steady_clock::time_point timestamp;
  };
  mutable std::mutex mutex_;
  std::unordered_map<std::string, LastSeen> heartbeats_;
};

}  // namespace vectordb::coordinator

