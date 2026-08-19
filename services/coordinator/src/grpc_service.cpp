#include "vectordb/coordinator/grpc_service.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vectordb::coordinator {
namespace {

grpc::Status unavailable(const std::string& message) {
  return {grpc::StatusCode::UNAVAILABLE, message};
}

struct PartialResult {
  LogicalShardId shard{};
  grpc::Status status;
  vectordb::v1::VectorSearchResponse response;
};

}  // namespace

CoordinatorGrpcService::CoordinatorGrpcService(std::shared_ptr<ShardDirectory> directory,
                                               std::unordered_map<std::string, std::string> node_endpoints)
    : directory_(std::move(directory)), node_endpoints_(std::move(node_endpoints)) {
  if (!directory_) throw std::invalid_argument("CoordinatorGrpcService requires a directory");
}

grpc::Status CoordinatorGrpcService::VectorSearch(grpc::ServerContext* context,
                                                   const vectordb::v1::VectorSearchRequest* request,
                                                   vectordb::v1::VectorSearchResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  const auto placements = directory_->placements();
  std::vector<std::future<PartialResult>> futures;
  futures.reserve(placements.size());
  for (const auto& placement : placements) {
    futures.push_back(std::async(std::launch::async, [this, request_copy = *request, shard = placement.shard]() mutable {
      PartialResult result{.shard = shard, .status = grpc::Status::OK};
      result.status = search_shard(request_copy, shard, &result.response);
      return result;
    }));
  }
  std::unordered_map<std::uint64_t, vectordb::v1::SearchHit> unique_hits;
  for (auto& future : futures) {
    const auto partial = future.get();
    if (!partial.status.ok()) return partial.status;
    for (const auto& hit : partial.response.hits()) {
      const auto found = unique_hits.find(hit.id());
      if (found == unique_hits.end() || hit.distance() < found->second.distance()) unique_hits.insert_or_assign(hit.id(), hit);
    }
  }
  std::vector<vectordb::v1::SearchHit> merged;
  merged.reserve(unique_hits.size());
  for (auto& [id, hit] : unique_hits) {
    (void)id;
    merged.push_back(std::move(hit));
  }
  std::sort(merged.begin(), merged.end(), [](const auto& left, const auto& right) {
    return left.distance() == right.distance() ? left.id() < right.id() : left.distance() < right.distance();
  });
  const auto top_k = std::min<std::size_t>(request->top_k(), merged.size());
  for (std::size_t index = 0; index < top_k; ++index) response->add_hits()->CopyFrom(merged[index]);
  response->set_shard_index(directory_->epoch());
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::ScatterGatherQuery(grpc::ServerContext* context,
                                                         const vectordb::v1::ScatterGatherQueryRequest* request,
                                                         vectordb::v1::ScatterGatherQueryResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  vectordb::v1::VectorSearchResponse partial;
  const auto status = search_shard(request->query(), request->logical_shard(), &partial);
  if (!status.ok()) return status;
  response->set_directory_epoch(directory_->epoch());
  response->set_logical_shard(request->logical_shard());
  for (const auto& hit : partial.hits()) response->add_hits()->CopyFrom(hit);
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::NodeHeartbeat(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<vectordb::v1::CoordinatorCommand, vectordb::v1::NodeHeartbeatRequest>* stream) {
  vectordb::v1::NodeHeartbeatRequest heartbeat;
  while (stream->Read(&heartbeat)) {
    if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "node cancelled heartbeat stream"};
    try {
      heartbeats_.record(heartbeat.node_id(), heartbeat.incarnation());
    } catch (const std::exception& error) {
      return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
    }
  }
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::search_shard(const vectordb::v1::VectorSearchRequest& request,
                                                   LogicalShardId logical_shard,
                                                   vectordb::v1::VectorSearchResponse* response) const {
  const auto endpoint = endpoint_for(logical_shard);
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto client = vectordb::v1::VectorData::NewStub(channel);
  grpc::ClientContext context;
  const auto timeout = request.timeout_millis() == 0U ? 250U : request.timeout_millis();
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(timeout));
  const auto status = client->VectorSearch(&context, request, response);
  if (!status.ok()) return unavailable("shard " + std::to_string(logical_shard) + " failed: " + status.error_message());
  return grpc::Status::OK;
}

std::string CoordinatorGrpcService::endpoint_for(LogicalShardId logical_shard) const {
  const auto placements = directory_->placements();
  const auto found = std::find_if(placements.begin(), placements.end(), [logical_shard](const ShardPlacement& placement) {
    return placement.shard == logical_shard;
  });
  if (found == placements.end()) throw std::out_of_range("logical shard does not exist");
  const auto endpoint = node_endpoints_.find(found->leader);
  if (endpoint == node_endpoints_.end()) throw std::out_of_range("leader endpoint is missing");
  return endpoint->second;
}

}  // namespace vectordb::coordinator
