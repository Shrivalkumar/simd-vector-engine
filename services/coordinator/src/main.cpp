#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "vectordb/coordinator/grpc_service.hpp"

namespace {

std::vector<std::string> split(const std::string& source) {
  std::vector<std::string> result;
  std::stringstream stream(source);
  std::string item;
  while (std::getline(stream, item, ',')) {
    if (!item.empty()) result.push_back(item);
  }
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: vectordb-coordinatord <listen-address> <logical-shards> <node-id=endpoint,...>\n";
    return EXIT_FAILURE;
  }
  try {
    const auto logical_shards = static_cast<std::uint64_t>(std::stoull(argv[2]));
    auto directory = std::make_shared<vectordb::coordinator::ShardDirectory>(logical_shards, 1);
    std::unordered_map<std::string, std::string> endpoints;
    for (const auto& assignment : split(argv[3])) {
      const auto separator = assignment.find('=');
      if (separator == std::string::npos || separator == 0U || separator + 1U == assignment.size()) {
        throw std::invalid_argument("node assignment must be node-id=endpoint");
      }
      const auto node_id = assignment.substr(0, separator);
      const auto endpoint = assignment.substr(separator + 1U);
      directory->add_node({.id = node_id, .endpoint = endpoint, .virtual_nodes = 128});
      endpoints.emplace(node_id, endpoint);
    }
    directory->initialize_placements();
    vectordb::coordinator::CoordinatorGrpcService service(directory, endpoints);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(argv[1], grpc::InsecureServerCredentials());
    builder.RegisterService(static_cast<vectordb::v1::VectorData::Service*>(&service));
    builder.RegisterService(static_cast<vectordb::v1::ClusterInternal::Service*>(&service));
    builder.RegisterService(static_cast<vectordb::v1::CollectionAdmin::Service*>(&service));
    auto server = builder.BuildAndStart();
    if (!server) throw std::runtime_error("failed to start gRPC coordinator");
    std::cout << "vectordb coordinator serving " << logical_shards << " logical shards on " << argv[1] << '\n';
    server->Wait();
  } catch (const std::exception& error) {
    std::cerr << "vectordb-coordinatord: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
