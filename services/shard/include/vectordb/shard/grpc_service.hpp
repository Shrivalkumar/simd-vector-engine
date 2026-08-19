#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

#include "vectordb/v1/admin.grpc.pb.h"
#include "vectordb/v1/search.grpc.pb.h"
#include "vectordb/shard/service.hpp"

namespace vectordb::shard {

class ShardGrpcService final : public vectordb::v1::VectorData::Service {
 public:
  explicit ShardGrpcService(std::shared_ptr<ShardService> shard);

  grpc::Status BatchUpsert(grpc::ServerContext* context,
                           grpc::ServerReaderWriter<vectordb::v1::BatchUpsertAck,
                                                    vectordb::v1::BatchUpsertRequest>* stream) override;
  grpc::Status VectorSearch(grpc::ServerContext* context, const vectordb::v1::VectorSearchRequest* request,
                            vectordb::v1::VectorSearchResponse* response) override;
  grpc::Status GetRecords(grpc::ServerContext* context, const vectordb::v1::GetRecordsRequest* request,
                          vectordb::v1::GetRecordsResponse* response) override;
  grpc::Status DeleteRecords(grpc::ServerContext* context, const vectordb::v1::DeleteRecordsRequest* request,
                             vectordb::v1::DeleteRecordsResponse* response) override;

 private:
  std::shared_ptr<ShardService> shard_;
};

class ShardAdminGrpcService final : public vectordb::v1::CollectionAdmin::Service {
 public:
  explicit ShardAdminGrpcService(std::shared_ptr<ShardService> shard);

  grpc::Status CreateCollection(grpc::ServerContext* context, const vectordb::v1::CreateCollectionRequest* request,
                                vectordb::v1::CreateCollectionResponse* response) override;
  grpc::Status ListCollections(grpc::ServerContext* context, const vectordb::v1::ListCollectionsRequest* request,
                               vectordb::v1::ListCollectionsResponse* response) override;
  grpc::Status DescribeCollection(grpc::ServerContext* context,
                                  const vectordb::v1::DescribeCollectionRequest* request,
                                  vectordb::v1::DescribeCollectionResponse* response) override;
  grpc::Status DeleteCollection(grpc::ServerContext* context, const vectordb::v1::DeleteCollectionRequest* request,
                                vectordb::v1::DeleteCollectionResponse* response) override;

 private:
  std::shared_ptr<ShardService> shard_;
};

}  // namespace vectordb::shard
