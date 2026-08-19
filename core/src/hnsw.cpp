#include "vectordb/hnsw/hnsw.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_set>

#include "vectordb/distance/distance.hpp"

namespace vectordb {
namespace {

struct Candidate {
  float distance{};
  std::uint32_t node{};
};

struct ClosestFirst {
  bool operator()(const Candidate& left, const Candidate& right) const noexcept { return left.distance > right.distance; }
};

struct FurthestFirst {
  bool operator()(const Candidate& left, const Candidate& right) const noexcept { return left.distance < right.distance; }
};

}  // namespace

HnswIndex::HnswIndex(HnswConfig config) : config_(config), random_(config.random_seed) {
  if (config_.dimensions == 0U || config_.max_neighbors == 0U || config_.ef_construction == 0U) {
    throw std::invalid_argument("invalid HNSW configuration");
  }
}

void HnswIndex::insert(VectorId id, Generation generation, std::span<const float> vector) {
  std::unique_lock lock(mutex_);
  insert_locked(id, generation, vector);
}

void HnswIndex::upsert(VectorId id, Generation generation, std::span<const float> vector) {
  if (id == 0U || generation == 0U || vector.size() != config_.dimensions) {
    throw std::invalid_argument("invalid HNSW record");
  }
  std::unique_lock lock(mutex_);
  const auto found = node_by_id_.find(id);
  if (found != node_by_id_.end()) {
    if (nodes_[found->second].generation >= generation) throw std::invalid_argument("HNSW generation is stale");
    nodes_[found->second].active = false;
    node_by_id_.erase(found);
  }
  insert_locked(id, generation, vector);
}

void HnswIndex::insert_locked(VectorId id, Generation generation, std::span<const float> vector) {
  if (id == 0U || generation == 0U || vector.size() != config_.dimensions) {
    throw std::invalid_argument("invalid HNSW record");
  }
  if (node_by_id_.contains(id)) throw std::invalid_argument("HNSW does not allow duplicate IDs");
  if (quantizer_.has_value()) throw std::logic_error("cannot insert into a sealed SQ8 HNSW index");

  const std::uint32_t level = random_level();
  const std::uint32_t node_id = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(Node{.id = id,
                        .generation = generation,
                        .vector = std::vector<float>(vector.begin(), vector.end()),
                        .neighbors = std::vector<std::vector<std::uint32_t>>(level + 1U)});
  node_by_id_.emplace(id, node_id);

  if (!has_entry_point_) {
    entry_point_ = node_id;
    max_level_ = level;
    has_entry_point_ = true;
    return;
  }

  std::uint32_t current = entry_point_;
  for (std::uint32_t layer = max_level_; layer > level; --layer) {
    const auto nearest = search_layer(vector, {current}, layer, 1U);
    if (!nearest.empty()) current = nearest.front();
  }

  const std::uint32_t top_layer = std::min(level, max_level_);
  for (std::int64_t signed_layer = static_cast<std::int64_t>(top_layer); signed_layer >= 0; --signed_layer) {
    const auto layer = static_cast<std::uint32_t>(signed_layer);
    const auto nearest = search_layer(vector, {current}, layer, config_.ef_construction);
    const std::size_t count = std::min<std::size_t>(config_.max_neighbors, nearest.size());
    for (std::size_t index = 0; index < count; ++index) link(node_id, nearest[index], layer);
    if (!nearest.empty()) current = nearest.front();
  }

  if (level > max_level_) {
    entry_point_ = node_id;
    max_level_ = level;
  }
}

void HnswIndex::deactivate(VectorId id) {
  std::unique_lock lock(mutex_);
  const auto found = node_by_id_.find(id);
  if (found == node_by_id_.end()) return;
  nodes_[found->second].active = false;
  node_by_id_.erase(found);
}

void HnswIndex::enable_sq8() {
  std::unique_lock lock(mutex_);
  if (nodes_.empty()) throw std::logic_error("cannot quantize an empty HNSW index");
  std::vector<std::vector<float>> vectors;
  vectors.reserve(nodes_.size());
  for (const auto& node : nodes_) vectors.push_back(node.vector);
  ScalarQuantizer8 quantizer;
  quantizer.train(vectors);
  for (auto& node : nodes_) node.sq8_code = quantizer.encode(node.vector);
  quantizer_ = std::move(quantizer);
}

std::vector<SearchHit> HnswIndex::search(std::span<const float> query, std::uint32_t k, std::uint32_t ef_search) const {
  if (query.size() != config_.dimensions || k == 0U) throw std::invalid_argument("invalid HNSW query");
  std::shared_lock lock(mutex_);
  if (!has_entry_point_) return {};
  std::uint32_t current = entry_point_;
  for (std::uint32_t layer = max_level_; layer > 0U; --layer) {
    const auto nearest = search_layer(query, {current}, layer, 1U);
    if (!nearest.empty()) current = nearest.front();
  }
  const auto nearest = search_layer(query, {current}, 0U, std::max(k, ef_search));
  const std::size_t rerank_count = std::min<std::size_t>(nearest.size(), std::max<std::uint32_t>(k, 100U));
  std::vector<Candidate> reranked;
  reranked.reserve(rerank_count);
  for (std::size_t index = 0; index < rerank_count; ++index) {
    if (!nodes_[nearest[index]].active) continue;
    reranked.push_back({.distance = distance(query, nodes_[nearest[index]].vector, config_.metric), .node = nearest[index]});
  }
  std::sort(reranked.begin(), reranked.end(), [](const Candidate& left, const Candidate& right) {
    return left.distance < right.distance;
  });
  std::vector<SearchHit> results;
  const std::size_t count = std::min<std::size_t>(k, reranked.size());
  results.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const auto& candidate = reranked[index];
    const auto& node = nodes_[candidate.node];
    results.push_back(SearchHit{.id = node.id, .generation = node.generation, .score = candidate.distance});
  }
  return results;
}

std::size_t HnswIndex::size() const {
  std::shared_lock lock(mutex_);
  return nodes_.size();
}

bool HnswIndex::contains(VectorId id) const {
  std::shared_lock lock(mutex_);
  return node_by_id_.contains(id);
}

bool HnswIndex::uses_sq8() const {
  std::shared_lock lock(mutex_);
  return quantizer_.has_value();
}

std::uint32_t HnswIndex::random_level() {
  const double unit = std::max(std::generate_canonical<double, 53>(random_), std::numeric_limits<double>::min());
  const double multiplier = 1.0 / std::log(static_cast<double>(config_.max_neighbors));
  return static_cast<std::uint32_t>(-std::log(unit) * multiplier);
}

float HnswIndex::node_distance(std::span<const float> query, std::uint32_t node_id) const {
  const auto& node = nodes_.at(node_id);
  if (quantizer_.has_value()) return quantizer_->approximate_distance(query, node.sq8_code, config_.metric);
  return distance(query, node.vector, config_.metric);
}

std::vector<std::uint32_t> HnswIndex::search_layer(std::span<const float> query,
                                                    std::vector<std::uint32_t> entry_points,
                                                    std::uint32_t layer, std::uint32_t ef) const {
  std::priority_queue<Candidate, std::vector<Candidate>, ClosestFirst> candidates;
  std::priority_queue<Candidate, std::vector<Candidate>, FurthestFirst> best;
  std::unordered_set<std::uint32_t> visited;
  for (const auto entry : entry_points) {
    if (entry >= nodes_.size() || layer >= nodes_[entry].neighbors.size() || !visited.insert(entry).second) continue;
    const Candidate candidate{.distance = node_distance(query, entry), .node = entry};
    candidates.push(candidate);
    best.push(candidate);
  }
  while (!candidates.empty()) {
    const Candidate current = candidates.top();
    candidates.pop();
    if (best.size() >= ef && current.distance > best.top().distance) break;
    for (const auto neighbor : nodes_[current.node].neighbors[layer]) {
      if (!visited.insert(neighbor).second) continue;
      const Candidate candidate{.distance = node_distance(query, neighbor), .node = neighbor};
      if (best.size() < ef || candidate.distance < best.top().distance) {
        candidates.push(candidate);
        best.push(candidate);
        if (best.size() > ef) best.pop();
      }
    }
  }
  std::vector<Candidate> ordered;
  ordered.reserve(best.size());
  while (!best.empty()) {
    ordered.push_back(best.top());
    best.pop();
  }
  std::sort(ordered.begin(), ordered.end(), [](const Candidate& left, const Candidate& right) {
    return left.distance < right.distance;
  });
  std::vector<std::uint32_t> result;
  result.reserve(ordered.size());
  for (const auto candidate : ordered) result.push_back(candidate.node);
  return result;
}

void HnswIndex::link(std::uint32_t source, std::uint32_t target, std::uint32_t layer) {
  auto& source_edges = nodes_[source].neighbors[layer];
  auto& target_edges = nodes_[target].neighbors[layer];
  if (std::find(source_edges.begin(), source_edges.end(), target) == source_edges.end()) source_edges.push_back(target);
  if (std::find(target_edges.begin(), target_edges.end(), source) == target_edges.end()) target_edges.push_back(source);
  prune_neighbors(source, layer);
  prune_neighbors(target, layer);
}

void HnswIndex::prune_neighbors(std::uint32_t node_id, std::uint32_t layer) {
  auto& edges = nodes_[node_id].neighbors[layer];
  if (edges.size() <= config_.max_neighbors) return;
  const auto& origin = nodes_[node_id].vector;
  std::sort(edges.begin(), edges.end(), [&origin, this](std::uint32_t left, std::uint32_t right) {
    return distance(origin, nodes_[left].vector, config_.metric) < distance(origin, nodes_[right].vector, config_.metric);
  });
  edges.resize(config_.max_neighbors);
}

}  // namespace vectordb
