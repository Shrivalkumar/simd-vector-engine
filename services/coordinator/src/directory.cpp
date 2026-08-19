#include "vectordb/coordinator/directory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace vectordb::coordinator {

ShardDirectory::ShardDirectory(std::uint64_t logical_shard_count, std::uint32_t replication_factor)
    : logical_shard_count_(logical_shard_count), replication_factor_(replication_factor) {
  if (logical_shard_count_ == 0U || replication_factor_ == 0U) {
    throw std::invalid_argument("logical shards and replication factor must be positive");
  }
}

void ShardDirectory::add_node(NodeDescriptor node) {
  if (node.id.empty() || node.endpoint.empty() || node.virtual_nodes == 0U) throw std::invalid_argument("invalid node descriptor");
  std::scoped_lock lock(mutex_);
  if (nodes_.contains(node.id)) throw std::invalid_argument("node already exists");
  nodes_.emplace(node.id, std::move(node));
  rebuild_ring_locked();
  increment_epoch_locked();
}

void ShardDirectory::set_node_state(const std::string& node_id, NodeState state) {
  std::scoped_lock lock(mutex_);
  const auto found = nodes_.find(node_id);
  if (found == nodes_.end()) throw std::out_of_range("node does not exist");
  found->second.state = state;
  rebuild_ring_locked();
  increment_epoch_locked();
}

void ShardDirectory::initialize_placements() {
  std::scoped_lock lock(mutex_);
  if (!placements_.empty()) throw std::logic_error("placements are already initialized");
  if (ring_.empty()) throw std::logic_error("cannot initialize a directory without active nodes");
  for (LogicalShardId shard = 0; shard < logical_shard_count_; ++shard) {
    auto replicas = replicas_for("logical-shard/" + std::to_string(shard));
    if (replicas.size() != replication_factor_) throw std::logic_error("insufficient active nodes for replication factor");
    placements_.emplace(shard, ShardPlacement{.shard = shard, .replicas = std::move(replicas), .leader = {}});
    placements_.at(shard).leader = placements_.at(shard).replicas.front();
  }
  increment_epoch_locked();
}

Route ShardDirectory::route(const std::string& partition_key) const {
  if (partition_key.empty()) throw std::invalid_argument("partition key is required");
  std::scoped_lock lock(mutex_);
  require_initialized_locked();
  const LogicalShardId shard = hash(partition_key) % logical_shard_count_;
  const auto found = placements_.find(shard);
  if (found == placements_.end()) throw std::logic_error("placement is missing");
  return {.directory_epoch = epoch_, .shard = shard, .leader = found->second.leader, .replicas = found->second.replicas};
}

std::vector<ShardPlacement> ShardDirectory::placements() const {
  std::scoped_lock lock(mutex_);
  std::vector<ShardPlacement> result;
  result.reserve(placements_.size());
  for (const auto& [shard, placement] : placements_) {
    (void)shard;
    result.push_back(placement);
  }
  return result;
}

std::uint64_t ShardDirectory::epoch() const {
  std::scoped_lock lock(mutex_);
  return epoch_;
}

Migration ShardDirectory::begin_migration(LogicalShardId shard, const std::string& destination) {
  std::scoped_lock lock(mutex_);
  require_initialized_locked();
  const auto placement = placements_.find(shard);
  if (placement == placements_.end()) throw std::out_of_range("shard does not exist");
  const auto node = nodes_.find(destination);
  if (node == nodes_.end() || node->second.state != NodeState::Active) throw std::invalid_argument("destination is not active");
  if (std::find(placement->second.replicas.begin(), placement->second.replicas.end(), destination) != placement->second.replicas.end()) {
    throw std::invalid_argument("destination already hosts the shard");
  }
  const std::string source = placement->second.replicas.back();
  Migration migration{.id = "migration-" + std::to_string(next_migration_id_++), .shard = shard,
                      .source = source, .destination = destination};
  migrations_.emplace(migration.id, migration);
  increment_epoch_locked();
  return migration;
}

void ShardDirectory::mark_caught_up(const std::string& migration_id) {
  std::scoped_lock lock(mutex_);
  auto found = migrations_.find(migration_id);
  if (found == migrations_.end()) throw std::out_of_range("migration does not exist");
  if (found->second.phase != MigrationPhase::Copying) throw std::logic_error("migration is not copying");
  found->second.phase = MigrationPhase::ReadyToCutover;
  increment_epoch_locked();
}

void ShardDirectory::cutover(const std::string& migration_id) {
  std::scoped_lock lock(mutex_);
  const auto migration = migrations_.find(migration_id);
  if (migration == migrations_.end()) throw std::out_of_range("migration does not exist");
  if (migration->second.phase != MigrationPhase::ReadyToCutover) throw std::logic_error("migration is not caught up");
  auto placement = placements_.find(migration->second.shard);
  if (placement == placements_.end()) throw std::logic_error("migration placement is missing");
  auto source = std::find(placement->second.replicas.begin(), placement->second.replicas.end(), migration->second.source);
  if (source == placement->second.replicas.end()) throw std::logic_error("migration source is no longer a replica");
  *source = migration->second.destination;
  if (placement->second.leader == migration->second.source) placement->second.leader = migration->second.destination;
  migrations_.erase(migration);
  increment_epoch_locked();
}

std::optional<Migration> ShardDirectory::migration(const std::string& migration_id) const {
  std::scoped_lock lock(mutex_);
  const auto found = migrations_.find(migration_id);
  if (found == migrations_.end()) return std::nullopt;
  return found->second;
}

std::uint64_t ShardDirectory::hash(std::string_view value) noexcept {
  std::uint64_t result = 14695981039346656037ULL;
  for (const char character : value) {
    result ^= static_cast<std::uint8_t>(character);
    result *= 1099511628211ULL;
  }
  return result;
}

std::vector<std::string> ShardDirectory::replicas_for(std::string_view key) const {
  std::vector<std::string> replicas;
  if (ring_.empty()) return replicas;
  const auto start = ring_successor(hash(key));
  std::unordered_set<std::string> chosen;
  auto token = start;
  do {
    if (chosen.insert(token->second.node).second) replicas.push_back(token->second.node);
    if (replicas.size() == replication_factor_) break;
    ++token;
    if (token == ring_.end()) token = ring_.begin();
  } while (token != start);
  return replicas;
}

std::map<std::uint64_t, ShardDirectory::Token>::const_iterator ShardDirectory::ring_successor(std::uint64_t value) const {
  const auto found = ring_.lower_bound(value);
  return found == ring_.end() ? ring_.begin() : found;
}

void ShardDirectory::rebuild_ring_locked() {
  ring_.clear();
  for (const auto& [id, node] : nodes_) {
    if (node.state != NodeState::Active) continue;
    for (std::uint32_t token = 0; token < node.virtual_nodes; ++token) {
      const std::string token_key = id + '#' + std::to_string(token);
      std::uint64_t value = hash(token_key);
      while (ring_.contains(value)) ++value;
      ring_.emplace(value, Token{.value = value, .node = id});
    }
  }
}

void ShardDirectory::require_initialized_locked() const {
  if (placements_.size() != logical_shard_count_) throw std::logic_error("directory placements are not initialized");
}

void ShardDirectory::increment_epoch_locked() {
  if (epoch_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("directory epoch overflow");
  ++epoch_;
}

void HeartbeatRegistry::record(const std::string& node_id, std::uint64_t incarnation) {
  if (node_id.empty() || incarnation == 0U) throw std::invalid_argument("invalid heartbeat");
  std::scoped_lock lock(mutex_);
  const auto now = std::chrono::steady_clock::now();
  const auto existing = heartbeats_.find(node_id);
  if (existing != heartbeats_.end() && incarnation < existing->second.incarnation) return;
  heartbeats_.insert_or_assign(node_id, LastSeen{.incarnation = incarnation, .timestamp = now});
}

bool HeartbeatRegistry::is_healthy(const std::string& node_id, std::chrono::milliseconds timeout) const {
  std::scoped_lock lock(mutex_);
  const auto found = heartbeats_.find(node_id);
  if (found == heartbeats_.end()) return false;
  return std::chrono::steady_clock::now() - found->second.timestamp <= timeout;
}

}  // namespace vectordb::coordinator

