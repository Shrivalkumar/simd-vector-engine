#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

#include "vectordb/coordinator/directory.hpp"
#include "vectordb/v1/admin.grpc.pb.h"
#include "vectordb/v1/cluster.grpc.pb.h"
#include "vectordb/v1/search.grpc.pb.h"

namespace vectordb::coordinator {

class CoordinatorGrpcService final : public vectordb::v1::VectorData::Service,
                                     public vectordb::v1::ClusterInternal::Service,
                                     public vectordb::v1::CollectionAdmin::Service {
 public:
  CoordinatorGrpcService(std::shared_ptr<ShardDirectory> directory,
                         std::unordered_map<std::string, std::string> node_endpoints);

  grpc::Status BatchUpsert(grpc::ServerContext* context,
                           grpc::ServerReaderWriter<vectordb::v1::BatchUpsertAck,
                                                    vectordb::v1::BatchUpsertRequest>* stream) override;
  grpc::Status VectorSearch(grpc::ServerContext* context, const vectordb::v1::VectorSearchRequest* request,
                            vectordb::v1::VectorSearchResponse* response) override;
  grpc::Status GetRecords(grpc::ServerContext* context, const vectordb::v1::GetRecordsRequest* request,
                          vectordb::v1::GetRecordsResponse* response) override;
  grpc::Status DeleteRecords(grpc::ServerContext* context, const vectordb::v1::DeleteRecordsRequest* request,
                             vectordb::v1::DeleteRecordsResponse* response) override;
  grpc::Status CreateCollection(grpc::ServerContext* context, const vectordb::v1::CreateCollectionRequest* request,
                                vectordb::v1::CreateCollectionResponse* response) override;
  grpc::Status ListCollections(grpc::ServerContext* context, const vectordb::v1::ListCollectionsRequest* request,
                               vectordb::v1::ListCollectionsResponse* response) override;
  grpc::Status DescribeCollection(grpc::ServerContext* context,
                                  const vectordb::v1::DescribeCollectionRequest* request,
                                  vectordb::v1::DescribeCollectionResponse* response) override;
  grpc::Status DeleteCollection(grpc::ServerContext* context, const vectordb::v1::DeleteCollectionRequest* request,
                                vectordb::v1::DeleteCollectionResponse* response) override;
  grpc::Status ScatterGatherQuery(grpc::ServerContext* context,
                                  const vectordb::v1::ScatterGatherQueryRequest* request,
                                  vectordb::v1::ScatterGatherQueryResponse* response) override;
  grpc::Status NodeHeartbeat(
      grpc::ServerContext* context,
      grpc::ServerReaderWriter<vectordb::v1::CoordinatorCommand, vectordb::v1::NodeHeartbeatRequest>* stream) override;

 private:
  [[nodiscard]] grpc::Status search_shard(const vectordb::v1::VectorSearchRequest& request,
                                          LogicalShardId logical_shard,
                                          vectordb::v1::VectorSearchResponse* response) const;
  [[nodiscard]] std::string endpoint_for(LogicalShardId logical_shard) const;
  [[nodiscard]] std::string endpoint_for_record(std::uint64_t record_id) const;
  [[nodiscard]] std::vector<std::string> unique_endpoints() const;

  std::shared_ptr<ShardDirectory> directory_;
  std::unordered_map<std::string, std::string> node_endpoints_;
  HeartbeatRegistry heartbeats_;
};

}  // namespace vectordb::coordinator
