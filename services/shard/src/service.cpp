#include "vectordb/shard/service.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace vectordb::shard {
namespace {

bool valid_collection_name(const std::string& name) {
  return !name.empty() && name.size() <= 128U &&
         std::all_of(name.begin(), name.end(), [](unsigned char value) {
           return std::isalnum(value) != 0 || value == '-' || value == '_';
         });
}

std::uint32_t metric_code(Metric metric) {
  switch (metric) {
    case Metric::Cosine: return 1U;
    case Metric::L2Squared: return 2U;
    case Metric::DotProduct: return 3U;
  }
  throw std::invalid_argument("unsupported collection metric");
}

Metric decode_metric(std::uint32_t code) {
  switch (code) {
    case 1U: return Metric::Cosine;
    case 2U: return Metric::L2Squared;
    case 3U: return Metric::DotProduct;
    default: throw std::runtime_error("invalid metric in collection manifest");
  }
}

}  // namespace

ShardService::ShardService(std::filesystem::path data_directory) : data_directory_(std::move(data_directory)) {
  std::filesystem::create_directories(data_directory_);
  load_manifest();
}

void ShardService::create_collection(CollectionConfig config) {
  std::scoped_lock lock(mutex_);
  if (!valid_collection_name(config.name)) throw std::invalid_argument("collection name must use letters, digits, '-' or '_'");
  if (config.dimensions == 0U) throw std::invalid_argument("collection dimensions must be positive");
  if (collections_.contains(config.name)) throw std::invalid_argument("collection already exists");
  if (config.hnsw.max_neighbors == 0U) config.hnsw.max_neighbors = 32U;
  if (config.hnsw.ef_construction == 0U) config.hnsw.ef_construction = 200U;
  auto collection = std::make_shared<Collection>(data_directory_, config);
  collection->recover();
  collections_.emplace(config.name, collection);
  configurations_.emplace(config.name, config);
  try {
    persist_manifest_locked();
  } catch (...) {
    configurations_.erase(config.name);
    collections_.erase(config.name);
    throw;
  }
}

std::vector<CollectionSummary> ShardService::list_collections() const {
  std::shared_lock lock(mutex_);
  std::vector<CollectionSummary> result;
  result.reserve(collections_.size());
  for (const auto& [name, collection] : collections_) {
    const auto& config = configurations_.at(name);
    const auto live_vectors = static_cast<std::uint64_t>(collection->size());
    result.push_back({.config = config,
                      .live_vectors = live_vectors,
                      .resident_bytes = live_vectors * static_cast<std::uint64_t>(config.dimensions) * sizeof(float)});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.config.name < right.config.name;
  });
  return result;
}

CollectionSummary ShardService::describe_collection(const std::string& collection) const {
  std::shared_ptr<Collection> target;
  CollectionConfig config;
  {
    std::shared_lock lock(mutex_);
    target = collection_for(collection);
    config = configurations_.at(collection);
  }
  const auto live_vectors = static_cast<std::uint64_t>(target->size());
  return {.config = config,
          .live_vectors = live_vectors,
          .resident_bytes = live_vectors * static_cast<std::uint64_t>(config.dimensions) * sizeof(float)};
}

bool ShardService::delete_collection(const std::string& collection) {
  std::scoped_lock request_lock(request_mutex_);
  std::scoped_lock lock(mutex_);
  const auto found = collections_.find(collection);
  if (found == collections_.end()) return false;
  const auto saved_collection = found->second;
  const auto saved_config = configurations_.at(collection);
  collections_.erase(found);
  configurations_.erase(collection);
  try {
    persist_manifest_locked();
  } catch (...) {
    collections_.emplace(collection, saved_collection);
    configurations_.emplace(collection, saved_config);
    throw;
  }
  std::erase_if(completed_requests_, [&collection](const auto& item) {
    return item.first.starts_with(collection + ':');
  });
  const auto wal_path = data_directory_ / (collection + ".wal");
  std::error_code error;
  (void)std::filesystem::remove(wal_path, error);
  if (error) throw std::system_error(error, "deleting collection WAL failed");
  return true;
}

BatchUpsertResult ShardService::batch_upsert(const std::string& collection, const std::string& idempotency_key,
                                             std::vector<Record> records) {
  if (idempotency_key.empty()) throw std::invalid_argument("idempotency key is required");
  std::scoped_lock request_lock(request_mutex_);
  const std::string request_key = collection + ':' + idempotency_key;
  if (const auto completed = completed_requests_.find(request_key); completed != completed_requests_.end()) {
    return completed->second;
  }
  std::shared_ptr<Collection> target;
  {
    std::shared_lock lock(mutex_);
    target = collection_for(collection);
  }
  target->upsert_batch(records);
  const BatchUpsertResult result{.committed_index = ++committed_index_,
                                 .applied_count = static_cast<std::uint32_t>(records.size())};
  completed_requests_.emplace(request_key, result);
  return result;
}

std::vector<SearchHit> ShardService::search(const std::string& collection, std::span<const float> query,
                                             std::uint32_t k, std::uint32_t ef_search) const {
  std::shared_ptr<Collection> target;
  {
    std::shared_lock lock(mutex_);
    target = collection_for(collection);
  }
  return target->search(query, k, ef_search);
}

void ShardService::erase(const std::string& collection, VectorId id, Generation generation) {
  std::scoped_lock request_lock(request_mutex_);
  std::shared_ptr<Collection> target;
  {
    std::shared_lock lock(mutex_);
    target = collection_for(collection);
  }
  target->erase(id, generation);
  ++committed_index_;
}

std::optional<Record> ShardService::get(const std::string& collection, VectorId id) const {
  std::shared_ptr<Collection> target;
  {
    std::shared_lock lock(mutex_);
    target = collection_for(collection);
  }
  return target->get(id);
}

std::shared_ptr<Collection> ShardService::collection_for(const std::string& collection) const {
  const auto found = collections_.find(collection);
  if (found == collections_.end()) throw std::out_of_range("collection does not exist");
  return found->second;
}

void ShardService::load_manifest() {
  const auto manifest = data_directory_ / "collections.manifest";
  if (!std::filesystem::exists(manifest)) return;
  std::ifstream input(manifest);
  if (!input) throw std::runtime_error("opening collection manifest failed");
  std::string name;
  std::uint32_t dimensions = 0U;
  std::uint32_t metric = 0U;
  std::uint32_t max_neighbors = 0U;
  std::uint32_t ef_construction = 0U;
  while (input >> name >> dimensions >> metric >> max_neighbors >> ef_construction) {
    CollectionConfig config{.name = name,
                            .dimensions = dimensions,
                            .metric = decode_metric(metric),
                            .hnsw = {.max_neighbors = max_neighbors, .ef_construction = ef_construction}};
    if (!valid_collection_name(config.name) || config.dimensions == 0U || config.hnsw.max_neighbors == 0U ||
        config.hnsw.ef_construction == 0U || collections_.contains(config.name)) {
      throw std::runtime_error("invalid collection manifest entry");
    }
    auto collection = std::make_shared<Collection>(data_directory_, config);
    collection->recover();
    collections_.emplace(config.name, std::move(collection));
    configurations_.emplace(config.name, std::move(config));
  }
  if (!input.eof()) throw std::runtime_error("malformed collection manifest");
}

void ShardService::persist_manifest_locked() const {
  const auto temporary = data_directory_ / "collections.manifest.tmp";
  const auto destination = data_directory_ / "collections.manifest";
  std::vector<std::string> names;
  names.reserve(configurations_.size());
  for (const auto& [name, config] : configurations_) {
    (void)config;
    names.push_back(name);
  }
  std::sort(names.begin(), names.end());
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("creating collection manifest failed");
    for (const auto& name : names) {
      const auto& config = configurations_.at(name);
      output << config.name << ' ' << config.dimensions << ' ' << metric_code(config.metric) << ' '
             << config.hnsw.max_neighbors << ' ' << config.hnsw.ef_construction << '\n';
    }
    output.flush();
    if (!output) throw std::runtime_error("writing collection manifest failed");
  }
  std::filesystem::rename(temporary, destination);
}

}  // namespace vectordb::shard
