#include "vectordb/coordinator/grpc_service.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <map>
#include <set>
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

struct PartialUpsert {
  std::string endpoint;
  grpc::Status status;
  vectordb::v1::BatchUpsertAck acknowledgement;
};

PartialUpsert forward_upsert(std::string endpoint, vectordb::v1::BatchUpsertRequest batch) {
  PartialUpsert result{
      .endpoint = std::move(endpoint),
      .status = grpc::Status::OK,
      .acknowledgement = {},
  };
  auto channel = grpc::CreateChannel(result.endpoint, grpc::InsecureChannelCredentials());
  auto stub = vectordb::v1::VectorData::NewStub(channel);
  grpc::ClientContext shard_context;
  shard_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  auto shard_stream = stub->BatchUpsert(&shard_context);
  if (!shard_stream->Write(batch) || !shard_stream->WritesDone()) {
    shard_context.TryCancel();
    result.status = unavailable("shard upsert stream failed for " + result.endpoint);
    (void)shard_stream->Finish();
    return result;
  }
  vectordb::v1::BatchUpsertAck partial;
  while (shard_stream->Read(&partial)) {
    result.acknowledgement.set_committed_index(
        std::max(result.acknowledgement.committed_index(), partial.committed_index()));
    result.acknowledgement.set_applied_count(
        result.acknowledgement.applied_count() + partial.applied_count());
    for (const auto& error : partial.errors()) result.acknowledgement.add_errors()->CopyFrom(error);
  }
  result.status = shard_stream->Finish();
  if (!result.status.ok()) {
    result.status = unavailable("shard upsert failed for " + result.endpoint + ": " + result.status.error_message());
  }
  return result;
}

template <typename Stub, typename Request, typename Response, typename Method>
grpc::Status call_unary(const std::string& endpoint, const Request& request, Response* response, Method method) {
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto stub = Stub::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(1000));
  return ((*stub).*method)(&context, request, response);
}

}  // namespace

CoordinatorGrpcService::CoordinatorGrpcService(std::shared_ptr<ShardDirectory> directory,
                                               std::unordered_map<std::string, std::string> node_endpoints)
    : directory_(std::move(directory)), node_endpoints_(std::move(node_endpoints)) {
  if (!directory_) throw std::invalid_argument("CoordinatorGrpcService requires a directory");
}

grpc::Status CoordinatorGrpcService::BatchUpsert(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<vectordb::v1::BatchUpsertAck, vectordb::v1::BatchUpsertRequest>* stream) {
  vectordb::v1::BatchUpsertRequest request;
  while (stream->Read(&request)) {
    if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
    std::map<std::string, vectordb::v1::BatchUpsertRequest> batches;
    for (const auto& record : request.records()) {
      auto& batch = batches[endpoint_for_record(record.id())];
      batch.set_collection(request.collection());
      batch.set_idempotency_key(request.idempotency_key());
      batch.set_sequence(request.sequence());
      batch.set_consistency(request.consistency());
      batch.add_records()->CopyFrom(record);
    }
    vectordb::v1::BatchUpsertAck combined;
    combined.set_sequence(request.sequence());
    std::vector<std::future<PartialUpsert>> futures;
    futures.reserve(batches.size());
    for (auto& [endpoint, batch] : batches) {
      futures.push_back(std::async(std::launch::async, forward_upsert, endpoint, std::move(batch)));
    }
    for (auto& future : futures) {
      auto partial = future.get();
      if (!partial.status.ok()) return partial.status;
      combined.set_committed_index(std::max(combined.committed_index(), partial.acknowledgement.committed_index()));
      combined.set_applied_count(combined.applied_count() + partial.acknowledgement.applied_count());
      for (const auto& error : partial.acknowledgement.errors()) combined.add_errors()->CopyFrom(error);
    }
    if (!stream->Write(combined)) return {grpc::StatusCode::CANCELLED, "client stopped consuming acknowledgements"};
  }
  return grpc::Status::OK;
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
      PartialResult result{
          .shard = shard,
          .status = grpc::Status::OK,
          .response = {},
      };
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

grpc::Status CoordinatorGrpcService::GetRecords(grpc::ServerContext* context,
                                                const vectordb::v1::GetRecordsRequest* request,
                                                vectordb::v1::GetRecordsResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  std::map<std::string, vectordb::v1::GetRecordsRequest> batches;
  for (const auto id : request->ids()) {
    auto& batch = batches[endpoint_for_record(id)];
    batch.set_collection(request->collection());
    batch.set_consistency(request->consistency());
    batch.add_ids(id);
  }
  std::unordered_map<std::uint64_t, vectordb::v1::Record> records;
  for (const auto& [endpoint, batch] : batches) {
    vectordb::v1::GetRecordsResponse partial;
    const auto status = call_unary<vectordb::v1::VectorData>(
        endpoint, batch, &partial, &vectordb::v1::VectorData::Stub::GetRecords);
    if (!status.ok()) return unavailable("shard get failed for " + endpoint + ": " + status.error_message());
    for (const auto& record : partial.records()) records.insert_or_assign(record.id(), record);
  }
  for (const auto id : request->ids()) {
    const auto found = records.find(id);
    if (found != records.end()) response->add_records()->CopyFrom(found->second);
  }
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::DeleteRecords(grpc::ServerContext* context,
                                                   const vectordb::v1::DeleteRecordsRequest* request,
                                                   vectordb::v1::DeleteRecordsResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  std::map<std::string, vectordb::v1::DeleteRecordsRequest> batches;
  for (const auto& record : request->records()) {
    auto& batch = batches[endpoint_for_record(record.id())];
    batch.set_collection(request->collection());
    batch.set_idempotency_key(request->idempotency_key());
    batch.set_consistency(request->consistency());
    batch.add_records()->CopyFrom(record);
  }
  for (const auto& [endpoint, batch] : batches) {
    vectordb::v1::DeleteRecordsResponse partial;
    const auto status = call_unary<vectordb::v1::VectorData>(
        endpoint, batch, &partial, &vectordb::v1::VectorData::Stub::DeleteRecords);
    if (!status.ok()) return unavailable("shard delete failed for " + endpoint + ": " + status.error_message());
    response->set_deleted_count(response->deleted_count() + partial.deleted_count());
    response->set_committed_index(std::max(response->committed_index(), partial.committed_index()));
    for (const auto& error : partial.errors()) response->add_errors()->CopyFrom(error);
  }
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::CreateCollection(grpc::ServerContext* context,
                                                      const vectordb::v1::CreateCollectionRequest* request,
                                                      vectordb::v1::CreateCollectionResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  bool first = true;
  for (const auto& endpoint : unique_endpoints()) {
    vectordb::v1::CreateCollectionResponse partial;
    const auto status = call_unary<vectordb::v1::CollectionAdmin>(
        endpoint, *request, &partial, &vectordb::v1::CollectionAdmin::Stub::CreateCollection);
    if (!status.ok()) return unavailable("collection create failed for " + endpoint + ": " + status.error_message());
    if (first) {
      response->CopyFrom(partial);
      first = false;
    }
  }
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::ListCollections(grpc::ServerContext* context,
                                                     const vectordb::v1::ListCollectionsRequest* request,
                                                     vectordb::v1::ListCollectionsResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  std::map<std::string, vectordb::v1::CollectionInfo> aggregate;
  for (const auto& endpoint : unique_endpoints()) {
    vectordb::v1::ListCollectionsResponse partial;
    const auto status = call_unary<vectordb::v1::CollectionAdmin>(
        endpoint, *request, &partial, &vectordb::v1::CollectionAdmin::Stub::ListCollections);
    if (!status.ok()) return unavailable("collection list failed for " + endpoint + ": " + status.error_message());
    for (const auto& collection : partial.collections()) {
      auto [found, inserted] = aggregate.try_emplace(collection.name(), collection);
      if (!inserted) {
        if (found->second.dimensions() != collection.dimensions() || found->second.metric() != collection.metric()) {
          return {grpc::StatusCode::FAILED_PRECONDITION, "collection metadata differs between shards"};
        }
        found->second.set_live_vectors(found->second.live_vectors() + collection.live_vectors());
        found->second.set_resident_bytes(found->second.resident_bytes() + collection.resident_bytes());
      }
    }
  }
  for (const auto& [name, collection] : aggregate) {
    (void)name;
    response->add_collections()->CopyFrom(collection);
  }
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::DescribeCollection(grpc::ServerContext* context,
                                                        const vectordb::v1::DescribeCollectionRequest* request,
                                                        vectordb::v1::DescribeCollectionResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  bool first = true;
  for (const auto& endpoint : unique_endpoints()) {
    vectordb::v1::DescribeCollectionResponse partial;
    const auto status = call_unary<vectordb::v1::CollectionAdmin>(
        endpoint, *request, &partial, &vectordb::v1::CollectionAdmin::Stub::DescribeCollection);
    if (!status.ok()) return unavailable("collection describe failed for " + endpoint + ": " + status.error_message());
    if (first) {
      response->CopyFrom(partial);
      first = false;
    } else {
      auto* collection = response->mutable_collection();
      collection->set_live_vectors(collection->live_vectors() + partial.collection().live_vectors());
      collection->set_resident_bytes(collection->resident_bytes() + partial.collection().resident_bytes());
    }
  }
  return grpc::Status::OK;
}

grpc::Status CoordinatorGrpcService::DeleteCollection(grpc::ServerContext* context,
                                                      const vectordb::v1::DeleteCollectionRequest* request,
                                                      vectordb::v1::DeleteCollectionResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  for (const auto& endpoint : unique_endpoints()) {
    vectordb::v1::DeleteCollectionResponse partial;
    const auto status = call_unary<vectordb::v1::CollectionAdmin>(
        endpoint, *request, &partial, &vectordb::v1::CollectionAdmin::Stub::DeleteCollection);
    if (!status.ok()) return unavailable("collection delete failed for " + endpoint + ": " + status.error_message());
    response->set_deleted(response->deleted() || partial.deleted());
  }
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

std::string CoordinatorGrpcService::endpoint_for_record(std::uint64_t record_id) const {
  const auto route = directory_->route(std::to_string(record_id));
  const auto endpoint = node_endpoints_.find(route.leader);
  if (endpoint == node_endpoints_.end()) throw std::out_of_range("leader endpoint is missing");
  return endpoint->second;
}

std::vector<std::string> CoordinatorGrpcService::unique_endpoints() const {
  std::set<std::string> values;
  for (const auto& [node, endpoint] : node_endpoints_) {
    (void)node;
    values.insert(endpoint);
  }
  return {values.begin(), values.end()};
}

}  // namespace vectordb::coordinator
