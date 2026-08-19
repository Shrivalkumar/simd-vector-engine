#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "test.hpp"
#include "vectordb/shard/grpc_service.hpp"

namespace {

void encode_vector(const std::vector<float>& values, vectordb::v1::DenseVector* vector) {
  vector->set_dimension(static_cast<std::uint32_t>(values.size()));
  vector->set_fp32_le(std::string(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float)));
}

}  // namespace

VDB_TEST(grpc_service_streams_upserts_and_serves_search) {
  const auto directory = std::filesystem::temp_directory_path() / "vectordb-grpc-test";
  std::filesystem::create_directories(directory);
  std::filesystem::remove(directory / "grpc-items.wal");
  auto shard = std::make_shared<vectordb::shard::ShardService>(directory);
  shard->create_collection({.name = "grpc-items", .dimensions = 2, .metric = vectordb::Metric::L2Squared,
                            .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
  vectordb::shard::ShardGrpcService service(shard);
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  auto server = builder.BuildAndStart();
  VDB_REQUIRE(server != nullptr);
  VDB_REQUIRE(port > 0);
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  auto stub = vectordb::v1::VectorData::NewStub(channel);
  {
    grpc::ClientContext context;
    auto stream = stub->BatchUpsert(&context);
    vectordb::v1::BatchUpsertRequest request;
    request.set_collection("grpc-items");
    request.set_idempotency_key("grpc-test-request");
    request.set_sequence(1);
    auto* record = request.add_records();
    record->set_id(99);
    record->set_generation(1);
    encode_vector({2.0F, 3.0F}, record->mutable_vector());
    VDB_REQUIRE(stream->Write(request));
    VDB_REQUIRE(stream->WritesDone());
    vectordb::v1::BatchUpsertAck ack;
    VDB_REQUIRE(stream->Read(&ack));
    VDB_REQUIRE(ack.applied_count() == 1U);
    VDB_REQUIRE(stream->Finish().ok());
  }
  {
    grpc::ClientContext context;
    vectordb::v1::VectorSearchRequest request;
    request.set_collection("grpc-items");
    request.set_top_k(1);
    encode_vector({2.1F, 3.1F}, request.mutable_vector());
    vectordb::v1::VectorSearchResponse response;
    VDB_REQUIRE(stub->VectorSearch(&context, request, &response).ok());
    VDB_REQUIRE(response.hits_size() == 1);
    VDB_REQUIRE(response.hits(0).id() == 99U);
  }
  server->Shutdown();
  std::filesystem::remove(directory / "grpc-items.wal");
}
