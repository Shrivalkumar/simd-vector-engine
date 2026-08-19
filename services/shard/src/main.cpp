#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "vectordb/shard/grpc_service.hpp"

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "usage: vectordb-shardd <data-directory> <listen-address> <collection> <dimensions>\n";
    return EXIT_FAILURE;
  }
  try {
    const auto dimensions = static_cast<std::uint32_t>(std::stoul(argv[4]));
    auto shard = std::make_shared<vectordb::shard::ShardService>(std::filesystem::path(argv[1]));
    shard->create_collection({.name = argv[3], .dimensions = dimensions, .metric = vectordb::Metric::Cosine,
                              .hnsw = {.max_neighbors = 32, .ef_construction = 200}});
    vectordb::shard::ShardGrpcService service(shard);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(argv[2], grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    auto server = builder.BuildAndStart();
    if (!server) throw std::runtime_error("failed to start gRPC server");
    std::cout << "vectordb shard serving collection " << argv[3] << " on " << argv[2] << '\n';
    server->Wait();
  } catch (const std::exception& error) {
    std::cerr << "vectordb-shardd: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
