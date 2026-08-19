#include "vectordb/hnsw/hnsw.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
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

void HnswIndex::upsert_batch(std::span<const Record> records) {
  if (records.empty()) return;
  for (const auto& record : records) {
    if (record.id == 0U || record.generation == 0U) throw std::invalid_argument("invalid HNSW record");
    (void)validate_vector(record.vector);
  }
  std::unique_lock lock(mutex_);
  if (quantizer_.has_value()) throw std::logic_error("cannot insert into a sealed SQ8 HNSW index");
  std::unordered_map<VectorId, Generation> staged_generations;
  staged_generations.reserve(records.size());
  for (const auto& record : records) {
    Generation current_generation = 0U;
    if (const auto staged = staged_generations.find(record.id); staged != staged_generations.end()) {
      current_generation = staged->second;
    } else if (const auto found = node_by_id_.find(record.id); found != node_by_id_.end()) {
      current_generation = nodes_[found->second].generation;
    }
    if (current_generation >= record.generation) throw std::invalid_argument("HNSW generation is stale");
    staged_generations.insert_or_assign(record.id, record.generation);
  }
  for (const auto& record : records) {
    if (const auto found = node_by_id_.find(record.id); found != node_by_id_.end()) {
      nodes_[found->second].active = false;
      node_by_id_.erase(found);
    }
    insert_locked(record.id, record.generation, record.vector);
  }
}

double HnswIndex::validate_vector(std::span<const float> vector) const {
  if (vector.size() != config_.dimensions) throw std::invalid_argument("invalid HNSW record");
  double squared_norm = 0.0;
  for (const float value : vector) {
    if (!std::isfinite(value)) throw std::invalid_argument("HNSW vector contains a non-finite value");
    const auto wide_value = static_cast<double>(value);
    squared_norm = std::fma(wide_value, wide_value, squared_norm);
  }
  if (config_.metric == Metric::Cosine && squared_norm <= 0.0) {
    throw std::invalid_argument("cosine distance is undefined for a zero vector");
  }
  return squared_norm;
}

void HnswIndex::insert_locked(VectorId id, Generation generation, std::span<const float> vector) {
  if (id == 0U || generation == 0U || vector.size() != config_.dimensions) {
    throw std::invalid_argument("invalid HNSW record");
  }
  if (node_by_id_.contains(id)) throw std::invalid_argument("HNSW does not allow duplicate IDs");
  if (quantizer_.has_value()) throw std::logic_error("cannot insert into a sealed SQ8 HNSW index");
  const double squared_norm = validate_vector(vector);

  std::vector<float> stored_vector(vector.begin(), vector.end());
  if (config_.metric == Metric::Cosine) {
    const double inverse_norm = 1.0 / std::sqrt(squared_norm);
    for (auto& value : stored_vector) value = static_cast<float>(static_cast<double>(value) * inverse_norm);
  }

  const std::uint32_t level = random_level();
  const std::uint32_t node_id = static_cast<std::uint32_t>(nodes_.size());
  nodes_.push_back(Node{.id = id,
                        .generation = generation,
                        .vector = std::move(stored_vector),
                        .sq8_code = {},
                        .neighbors = std::vector<std::vector<std::uint32_t>>(level + 1U)});
  node_by_id_.emplace(id, node_id);

  if (!has_entry_point_) {
    entry_point_ = node_id;
    max_level_ = level;
    has_entry_point_ = true;
    return;
  }

  const auto& routing_vector = nodes_[node_id].vector;

  std::uint32_t current = entry_point_;
  for (std::uint32_t layer = max_level_; layer > level; --layer) {
    const auto nearest = search_layer(routing_vector, {current}, layer, 1U);
    if (!nearest.empty()) current = nearest.front();
  }

  const std::uint32_t top_layer = std::min(level, max_level_);
  for (std::int64_t signed_layer = static_cast<std::int64_t>(top_layer); signed_layer >= 0; --signed_layer) {
    const auto layer = static_cast<std::uint32_t>(signed_layer);
    const auto nearest = search_layer(routing_vector, {current}, layer, config_.ef_construction);
    const auto selected = select_neighbors(routing_vector, nearest, config_.max_neighbors);
    for (const auto neighbor : selected) link(node_id, neighbor, layer);
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
  const double squared_norm = validate_vector(query);
  std::vector<float> normalized_query;
  std::span<const float> routing_query = query;
  if (config_.metric == Metric::Cosine) {
    normalized_query.assign(query.begin(), query.end());
    const double inverse_norm = 1.0 / std::sqrt(squared_norm);
    for (auto& value : normalized_query) value = static_cast<float>(static_cast<double>(value) * inverse_norm);
    routing_query = normalized_query;
  }
  std::shared_lock lock(mutex_);
  if (!has_entry_point_) return {};
  std::uint32_t current = entry_point_;
  for (std::uint32_t layer = max_level_; layer > 0U; --layer) {
    const auto nearest = search_layer(routing_query, {current}, layer, 1U);
    if (!nearest.empty()) current = nearest.front();
  }
  const auto nearest = search_layer(routing_query, {current}, 0U, std::max(k, ef_search));
  const std::size_t rerank_count = nearest.size();
  std::vector<Candidate> reranked;
  reranked.reserve(rerank_count);
  for (std::size_t index = 0; index < rerank_count; ++index) {
    if (!nodes_[nearest[index]].active) continue;
    reranked.push_back({.distance = exact_distance(routing_query, nodes_[nearest[index]].vector), .node = nearest[index]});
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
  return exact_distance(query, node.vector);
}

float HnswIndex::exact_distance(std::span<const float> lhs, std::span<const float> rhs) const {
  if (config_.metric == Metric::Cosine) return 1.0F - dot_product(lhs, rhs);
  return distance(lhs, rhs, config_.metric);
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

std::vector<std::uint32_t> HnswIndex::select_neighbors(std::span<const float> query,
                                                       std::span<const std::uint32_t> candidates,
                                                       std::uint32_t maximum) const {
  std::vector<Candidate> ordered;
  ordered.reserve(candidates.size());
  for (const auto node : candidates) {
    if (node >= nodes_.size() || !nodes_[node].active) continue;
    ordered.push_back({.distance = exact_distance(query, nodes_[node].vector), .node = node});
  }
  std::sort(ordered.begin(), ordered.end(), [](const Candidate& left, const Candidate& right) {
    return left.distance == right.distance ? left.node < right.node : left.distance < right.distance;
  });

  std::vector<std::uint32_t> selected;
  std::vector<std::uint32_t> rejected;
  selected.reserve(std::min<std::size_t>(maximum, ordered.size()));
  rejected.reserve(ordered.size());
  for (const auto& candidate : ordered) {
    bool diverse = true;
    for (const auto chosen : selected) {
      if (exact_distance(nodes_[candidate.node].vector, nodes_[chosen].vector) < candidate.distance) {
        diverse = false;
        break;
      }
    }
    if (diverse && selected.size() < maximum) selected.push_back(candidate.node);
    else rejected.push_back(candidate.node);
  }
  for (const auto candidate : rejected) {
    if (selected.size() >= maximum) break;
    selected.push_back(candidate);
  }
  return selected;
}

std::uint32_t HnswIndex::maximum_neighbors(std::uint32_t layer) const noexcept {
  if (layer != 0U || config_.max_neighbors > std::numeric_limits<std::uint32_t>::max() / 2U) {
    return config_.max_neighbors;
  }
  return config_.max_neighbors * 2U;
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
  const auto maximum = maximum_neighbors(layer);
  if (edges.size() <= maximum) return;
  const auto& origin = nodes_[node_id].vector;
  edges = select_neighbors(origin, edges, maximum);
}

}  // namespace vectordb
