#include <cstring>
#include <filesystem>
#include <memory>
#include <initializer_list>
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

std::unique_ptr<grpc::Server> start_server(std::initializer_list<grpc::Service*> services, int* port) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), port);
  for (auto* service : services) builder.RegisterService(service);
  auto server = builder.BuildAndStart();
  if (!server || *port <= 0) throw std::runtime_error("failed to start local gRPC server");
  return server;
}

std::shared_ptr<vectordb::shard::ShardService> make_shard(const std::filesystem::path& directory, std::uint64_t id,
                                                           std::vector<float> vector) {
  std::filesystem::create_directories(directory);
  std::filesystem::remove_all(directory);
  auto shard = std::make_shared<vectordb::shard::ShardService>(directory);
  shard->create_collection({.name = "records", .dimensions = 2, .metric = vectordb::Metric::L2Squared,
                            .hnsw = {.max_neighbors = 8, .ef_construction = 64}});
  (void)shard->batch_upsert(
      "records", "seed-" + std::to_string(id),
      {{.id = id, .generation = 1, .vector = std::move(vector), .payload = {}}});
  return shard;
}

}  // namespace

VDB_TEST(coordinator_fans_out_and_merges_shard_results) {
  const auto root = std::filesystem::temp_directory_path() / "vectordb-coordinator-grpc-test";
  const auto shard_a = make_shard(root / "a", 1, {0.0F, 0.0F});
  const auto shard_b = make_shard(root / "b", 2, {4.0F, 4.0F});
  const auto shard_c = make_shard(root / "c", 3, {8.0F, 8.0F});
  (void)shard_a->batch_upsert(
      "records", "seed-a",
      {{.id = 2, .generation = 1, .vector = {4.0F, 4.0F}, .payload = {}},
       {.id = 3, .generation = 1, .vector = {8.0F, 8.0F}, .payload = {}}});
  (void)shard_b->batch_upsert(
      "records", "seed-b",
      {{.id = 1, .generation = 1, .vector = {0.0F, 0.0F}, .payload = {}},
       {.id = 3, .generation = 1, .vector = {8.0F, 8.0F}, .payload = {}}});
  (void)shard_c->batch_upsert(
      "records", "seed-c",
      {{.id = 1, .generation = 1, .vector = {0.0F, 0.0F}, .payload = {}},
       {.id = 2, .generation = 1, .vector = {4.0F, 4.0F}, .payload = {}}});
  vectordb::shard::ShardGrpcService service_a(shard_a);
  vectordb::shard::ShardGrpcService service_b(shard_b);
  vectordb::shard::ShardGrpcService service_c(shard_c);
  int port_a = 0;
  int port_b = 0;
  int port_c = 0;
  auto server_a = start_server({&service_a}, &port_a);
  auto server_b = start_server({&service_b}, &port_b);
  auto server_c = start_server({&service_c}, &port_c);
  auto directory = std::make_shared<vectordb::coordinator::ShardDirectory>(3, 1);
  directory->add_node({.id = "a", .endpoint = "127.0.0.1:" + std::to_string(port_a), .virtual_nodes = 32});
  directory->add_node({.id = "b", .endpoint = "127.0.0.1:" + std::to_string(port_b), .virtual_nodes = 32});
  directory->add_node({.id = "c", .endpoint = "127.0.0.1:" + std::to_string(port_c), .virtual_nodes = 32});
  directory->initialize_placements();
  vectordb::coordinator::CoordinatorGrpcService coordinator(
      directory, {{"a", "127.0.0.1:" + std::to_string(port_a)}, {"b", "127.0.0.1:" + std::to_string(port_b)}, {"c", "127.0.0.1:" + std::to_string(port_c)}});
  int coordinator_port = 0;
  auto coordinator_server = start_server({static_cast<vectordb::v1::VectorData::Service*>(&coordinator)}, &coordinator_port);
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
  std::filesystem::remove_all(root);
}

VDB_TEST(coordinator_routes_crud_and_collection_administration) {
  const auto root = std::filesystem::temp_directory_path() / "vectordb-coordinator-crud-test";
  std::filesystem::remove_all(root);
  auto shard_a = std::make_shared<vectordb::shard::ShardService>(root / "a");
  auto shard_b = std::make_shared<vectordb::shard::ShardService>(root / "b");
  auto shard_c = std::make_shared<vectordb::shard::ShardService>(root / "c");
  vectordb::shard::ShardGrpcService data_a(shard_a);
  vectordb::shard::ShardGrpcService data_b(shard_b);
  vectordb::shard::ShardGrpcService data_c(shard_c);
  vectordb::shard::ShardAdminGrpcService admin_a(shard_a);
  vectordb::shard::ShardAdminGrpcService admin_b(shard_b);
  vectordb::shard::ShardAdminGrpcService admin_c(shard_c);
  int port_a = 0;
  int port_b = 0;
  int port_c = 0;
  auto server_a = start_server({&data_a, &admin_a}, &port_a);
  auto server_b = start_server({&data_b, &admin_b}, &port_b);
  auto server_c = start_server({&data_c, &admin_c}, &port_c);
  const std::unordered_map<std::string, std::string> endpoints{
      {"a", "127.0.0.1:" + std::to_string(port_a)},
      {"b", "127.0.0.1:" + std::to_string(port_b)},
      {"c", "127.0.0.1:" + std::to_string(port_c)}};
  auto directory = std::make_shared<vectordb::coordinator::ShardDirectory>(3, 1);
  for (const auto& [node, endpoint] : endpoints) directory->add_node({.id = node, .endpoint = endpoint, .virtual_nodes = 32});
  directory->initialize_placements();
  vectordb::coordinator::CoordinatorGrpcService coordinator(directory, endpoints);
  int coordinator_port = 0;
  auto coordinator_server = start_server(
      {static_cast<vectordb::v1::VectorData::Service*>(&coordinator),
       static_cast<vectordb::v1::CollectionAdmin::Service*>(&coordinator)},
      &coordinator_port);
  auto channel = grpc::CreateChannel("127.0.0.1:" + std::to_string(coordinator_port), grpc::InsecureChannelCredentials());
  auto admin = vectordb::v1::CollectionAdmin::NewStub(channel);
  auto data = vectordb::v1::VectorData::NewStub(channel);
  {
    grpc::ClientContext context;
    vectordb::v1::CreateCollectionRequest request;
    request.set_name("routed");
    request.set_dimensions(2);
    request.set_metric(vectordb::v1::DISTANCE_METRIC_L2_SQUARED);
    vectordb::v1::CreateCollectionResponse response;
    VDB_REQUIRE(admin->CreateCollection(&context, request, &response).ok());
    VDB_REQUIRE(response.collection().name() == "routed");
  }
  {
    grpc::ClientContext context;
    auto stream = data->BatchUpsert(&context);
    vectordb::v1::BatchUpsertRequest request;
    request.set_collection("routed");
    request.set_idempotency_key("crud-seed");
    for (const auto& [id, values] : std::vector<std::pair<std::uint64_t, std::vector<float>>>{{11, {0.0F, 0.0F}}, {22, {4.0F, 4.0F}}, {33, {8.0F, 8.0F}}}) {
      auto* record = request.add_records();
      record->set_id(id);
      record->set_generation(1);
      encode_query(values, record->mutable_vector());
    }
    VDB_REQUIRE(stream->Write(request));
    VDB_REQUIRE(stream->WritesDone());
    vectordb::v1::BatchUpsertAck ack;
    VDB_REQUIRE(stream->Read(&ack));
    VDB_REQUIRE(ack.applied_count() == 3U);
    VDB_REQUIRE(stream->Finish().ok());
  }
  {
    grpc::ClientContext context;
    vectordb::v1::VectorSearchRequest request;
    request.set_collection("routed");
    request.set_top_k(1);
    encode_query({3.9F, 4.1F}, request.mutable_vector());
    vectordb::v1::VectorSearchResponse response;
    VDB_REQUIRE(data->VectorSearch(&context, request, &response).ok());
    VDB_REQUIRE(response.hits_size() == 1);
    VDB_REQUIRE(response.hits(0).id() == 22U);
  }
  {
    grpc::ClientContext context;
    vectordb::v1::DeleteRecordsRequest request;
    request.set_collection("routed");
    auto* record = request.add_records();
    record->set_id(22);
    record->set_generation(1);
    vectordb::v1::DeleteRecordsResponse response;
    VDB_REQUIRE(data->DeleteRecords(&context, request, &response).ok());
    VDB_REQUIRE(response.deleted_count() == 1U);
  }
  {
    grpc::ClientContext context;
    vectordb::v1::DescribeCollectionRequest request;
    request.set_name("routed");
    vectordb::v1::DescribeCollectionResponse response;
    VDB_REQUIRE(admin->DescribeCollection(&context, request, &response).ok());
    VDB_REQUIRE(response.collection().live_vectors() == 2U);
  }
  coordinator_server->Shutdown();
  server_a->Shutdown();
  server_b->Shutdown();
  server_c->Shutdown();
  std::filesystem::remove_all(root);
}
