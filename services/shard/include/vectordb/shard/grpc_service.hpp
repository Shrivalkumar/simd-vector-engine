#pragma once

#include <memory>

#include <grpcpp/grpcpp.h>

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

}  // namespace vectordb::shard

