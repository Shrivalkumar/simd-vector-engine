#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "test.hpp"
#include "vectordb/coordinator/grpc_service.hpp"
#include "vectordb/shard/grpc_service.hpp"

namespace {

void encode_query(const std::vector<float>& values, vectordb::v1::DenseVector* vector) {
  vector->set_dimension(static_cast<std::uint32_t>(values.size()));
  vector->set_fp32_le(std::string(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(float)));
}

std::unique_ptr<grpc::Server> start_server(grpc::Service* service, int* port) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), port);
  builder.RegisterService(service);
  auto server = builder.BuildAndStart();
  if (!server || *port <= 0) throw std::runtime_error("failed to start local gRPC server");
  return server;
}

std::shared_ptr<vectordb::shard::ShardService> make_shard(const std::filesystem::path& directory, std::uint64_t id,
                                                           std::vector<float> vector) {
  std::filesystem::create_directories(directory);
  std::filesystem::remove(directory / "records.wal");
  auto shard = std::make_shared<vectordb::shard::ShardService>(directory);
  shard->create_collection({.name = "records", .dimensions = 2, .metric = vectordb::Metric::L2Squared,
                            .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
  (void)shard->batch_upsert("records", "seed-" + std::to_string(id), {{.id = id, .generation = 1, .vector = std::move(vector)}});
  return shard;
}

}  // namespace

VDB_TEST(coordinator_fans_out_and_merges_shard_results) {
  const auto root = std::filesystem::temp_directory_path() / "vectordb-coordinator-grpc-test";
  const auto shard_a = make_shard(root / "a", 1, {0.0F, 0.0F});
  const auto shard_b = make_shard(root / "b", 2, {4.0F, 4.0F});
  const auto shard_c = make_shard(root / "c", 3, {8.0F, 8.0F});
  (void)shard_a->batch_upsert("records", "seed-a", {{.id = 2, .generation = 1, .vector = {4.0F, 4.0F}}, {.id = 3, .generation = 1, .vector = {8.0F, 8.0F}}});
  (void)shard_b->batch_upsert("records", "seed-b", {{.id = 1, .generation = 1, .vector = {0.0F, 0.0F}}, {.id = 3, .generation = 1, .vector = {8.0F, 8.0F}}});
  (void)shard_c->batch_upsert("records", "seed-c", {{.id = 1, .generation = 1, .vector = {0.0F, 0.0F}}, {.id = 2, .generation = 1, .vector = {4.0F, 4.0F}}});
  vectordb::shard::ShardGrpcService service_a(shard_a);
  vectordb::shard::ShardGrpcService service_b(shard_b);
  vectordb::shard::ShardGrpcService service_c(shard_c);
  int port_a = 0;
  int port_b = 0;
  int port_c = 0;
  auto server_a = start_server(&service_a, &port_a);
  auto server_b = start_server(&service_b, &port_b);
  auto server_c = start_server(&service_c, &port_c);
  auto directory = std::make_shared<vectordb::coordinator::ShardDirectory>(3, 1);
  directory->add_node({.id = "a", .endpoint = "127.0.0.1:" + std::to_string(port_a), .virtual_nodes = 32});
  directory->add_node({.id = "b", .endpoint = "127.0.0.1:" + std::to_string(port_b), .virtual_nodes = 32});
  directory->add_node({.id = "c", .endpoint = "127.0.0.1:" + std::to_string(port_c), .virtual_nodes = 32});
  directory->initialize_placements();
  vectordb::coordinator::CoordinatorGrpcService coordinator(
      directory, {{"a", "127.0.0.1:" + std::to_string(port_a)}, {"b", "127.0.0.1:" + std::to_string(port_b)}, {"c", "127.0.0.1:" + std::to_string(port_c)}});
  int coordinator_port = 0;
  auto coordinator_server = start_server(static_cast<vectordb::v1::VectorData::Service*>(&coordinator), &coordinator_port);
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(coordinator_port), grpc::InsecureChannelCredentials());
  auto client = vectordb::v1::VectorData::NewStub(channel);
  grpc::ClientContext context;
  vectordb::v1::VectorSearchRequest request;
  request.set_collection("records");
  request.set_top_k(1);
  encode_query({0.1F, 0.1F}, request.mutable_vector());
  vectordb::v1::VectorSearchResponse response;
  VDB_REQUIRE(client->VectorSearch(&context, request, &response).ok());
  VDB_REQUIRE(response.hits_size() == 1);
  VDB_REQUIRE(response.hits(0).id() == 1U);
  coordinator_server->Shutdown();
  server_a->Shutdown();
  server_b->Shutdown();
  server_c->Shutdown();
  std::filesystem::remove(root / "a" / "records.wal");
  std::filesystem::remove(root / "b" / "records.wal");
  std::filesystem::remove(root / "c" / "records.wal");
}
