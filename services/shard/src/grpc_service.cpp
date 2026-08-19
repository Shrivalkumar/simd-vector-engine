#include "vectordb/shard/grpc_service.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace vectordb::shard {
namespace {

std::vector<float> decode_vector(const vectordb::v1::DenseVector& vector) {
  if (vector.dimension() == 0U) throw std::invalid_argument("vector dimension must be positive");
  const std::size_t expected_bytes = static_cast<std::size_t>(vector.dimension()) * sizeof(float);
  if (vector.fp32_le().size() != expected_bytes) throw std::invalid_argument("fp32_le byte length does not match dimension");
  std::vector<float> result(vector.dimension());
  std::memcpy(result.data(), vector.fp32_le().data(), expected_bytes);
  for (const float value : result) {
    if (!std::isfinite(value)) throw std::invalid_argument("vectors cannot contain NaN or infinity");
  }
  return result;
}

PayloadValue decode_value(const vectordb::v1::PayloadValue& value) {
  switch (value.value_case()) {
    case vectordb::v1::PayloadValue::kStringValue: return value.string_value();
    case vectordb::v1::PayloadValue::kIntValue: return value.int_value();
    case vectordb::v1::PayloadValue::kDoubleValue: return value.double_value();
    case vectordb::v1::PayloadValue::kBoolValue: return value.bool_value();
    case vectordb::v1::PayloadValue::kBytesValue:
      throw std::invalid_argument("bytes payload values are not enabled in the first storage segment format");
    case vectordb::v1::PayloadValue::VALUE_NOT_SET:
      throw std::invalid_argument("payload value is required");
  }
  throw std::invalid_argument("unsupported payload value");
}

Record decode_record(const vectordb::v1::Record& record, bool with_vector) {
  if (record.id() == 0U || record.generation() == 0U) throw std::invalid_argument("record id and generation must be non-zero");
  Record result{.id = record.id(), .generation = record.generation()};
  if (with_vector) result.vector = decode_vector(record.vector());
  result.payload.reserve(static_cast<std::size_t>(record.payload_size()));
  for (const auto& field : record.payload()) {
    if (field.key().empty()) throw std::invalid_argument("payload key is required");
    result.payload.push_back({.key = field.key(), .value = decode_value(field.value())});
  }
  return result;
}

void encode_value(const PayloadValue& source, vectordb::v1::PayloadValue* destination) {
  std::visit(
      [destination](const auto& value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, std::string>) destination->set_string_value(value);
        else if constexpr (std::is_same_v<Value, std::int64_t>) destination->set_int_value(value);
        else if constexpr (std::is_same_v<Value, double>) destination->set_double_value(value);
        else destination->set_bool_value(value);
      },
      source);
}

void encode_record(const Record& source, vectordb::v1::Record* destination) {
  destination->set_id(source.id);
  destination->set_generation(source.generation);
  auto* vector = destination->mutable_vector();
  vector->set_dimension(static_cast<std::uint32_t>(source.vector.size()));
  vector->set_fp32_le(std::string(reinterpret_cast<const char*>(source.vector.data()), source.vector.size() * sizeof(float)));
  for (const auto& field : source.payload) {
    auto* output = destination->add_payload();
    output->set_key(field.key);
    encode_value(field.value, output->mutable_value());
  }
}

grpc::Status invalid_argument(const std::exception& error) {
  return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
}

bool has_filter(const vectordb::v1::Filter& filter) {
  return filter.must_size() != 0 || filter.should_size() != 0 || filter.must_not_size() != 0;
}

}  // namespace

ShardGrpcService::ShardGrpcService(std::shared_ptr<ShardService> shard) : shard_(std::move(shard)) {
  if (!shard_) throw std::invalid_argument("ShardGrpcService requires a shard");
}

grpc::Status ShardGrpcService::BatchUpsert(
    grpc::ServerContext* context,
    grpc::ServerReaderWriter<vectordb::v1::BatchUpsertAck, vectordb::v1::BatchUpsertRequest>* stream) {
  vectordb::v1::BatchUpsertRequest request;
  while (stream->Read(&request)) {
    if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled stream"};
    vectordb::v1::BatchUpsertAck ack;
    ack.set_sequence(request.sequence());
    try {
      std::vector<Record> records;
      records.reserve(static_cast<std::size_t>(request.records_size()));
      for (const auto& record : request.records()) records.push_back(decode_record(record, true));
      const auto result = shard_->batch_upsert(request.collection(), request.idempotency_key(), std::move(records));
      ack.set_committed_index(result.committed_index);
      ack.set_applied_count(result.applied_count);
    } catch (const std::exception& error) {
      auto* record_error = ack.add_errors();
      record_error->set_code(static_cast<std::uint32_t>(grpc::StatusCode::INVALID_ARGUMENT));
      record_error->set_message(error.what());
    }
    if (!stream->Write(ack)) return {grpc::StatusCode::CANCELLED, "client stopped consuming acknowledgements"};
  }
  return grpc::Status::OK;
}

grpc::Status ShardGrpcService::VectorSearch(grpc::ServerContext* context,
                                             const vectordb::v1::VectorSearchRequest* request,
                                             vectordb::v1::VectorSearchResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  if (has_filter(request->filter())) return {grpc::StatusCode::UNIMPLEMENTED, "payload filters require the bitmap segment backend"};
  try {
    const auto query = decode_vector(request->vector());
    const auto hits = shard_->search(request->collection(), query, request->top_k(), request->ef_search() == 0U ? 96U : request->ef_search());
    for (const auto& hit : hits) {
      auto* output = response->add_hits();
      output->set_id(hit.id);
      output->set_generation(hit.generation);
      output->set_distance(hit.score);
    }
    return grpc::Status::OK;
  } catch (const std::exception& error) {
    return invalid_argument(error);
  }
}

grpc::Status ShardGrpcService::GetRecords(grpc::ServerContext* context, const vectordb::v1::GetRecordsRequest* request,
                                           vectordb::v1::GetRecordsResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  try {
    for (const auto id : request->ids()) {
      const auto record = shard_->get(request->collection(), id);
      if (record.has_value()) encode_record(*record, response->add_records());
    }
    return grpc::Status::OK;
  } catch (const std::exception& error) {
    return invalid_argument(error);
  }
}

grpc::Status ShardGrpcService::DeleteRecords(grpc::ServerContext* context,
                                              const vectordb::v1::DeleteRecordsRequest* request,
                                              vectordb::v1::DeleteRecordsResponse* response) {
  if (context->IsCancelled()) return {grpc::StatusCode::CANCELLED, "client cancelled request"};
  try {
    for (const auto& record : request->records()) shard_->erase(request->collection(), record.id(), record.generation());
    response->set_deleted_count(static_cast<std::uint32_t>(request->records_size()));
    return grpc::Status::OK;
  } catch (const std::exception& error) {
    return invalid_argument(error);
  }
}

}  // namespace vectordb::shard
