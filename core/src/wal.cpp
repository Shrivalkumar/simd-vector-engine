#include "vectordb/storage/wal.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <span>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace vectordb {
namespace {

constexpr std::uint32_t kMagic = 0x56444231U;  // VDB1
constexpr std::size_t kHeaderBytes = sizeof(std::uint32_t) * 3U;
constexpr std::uint32_t kMaxFrameBytes = 64U * 1024U * 1024U;

class Bytes {
 public:
  template <typename T>
  void append_pod(const T& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(&value);
    data_.insert(data_.end(), bytes, bytes + sizeof(T));
  }

  void append_string(const std::string& value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("string exceeds WAL encoding limit");
    }
    const auto length = static_cast<std::uint32_t>(value.size());
    append_pod(length);
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    data_.insert(data_.end(), bytes, bytes + value.size());
  }

  void append_vector(const std::vector<float>& value) {
    const auto* bytes = reinterpret_cast<const std::byte*>(value.data());
    data_.insert(data_.end(), bytes, bytes + value.size() * sizeof(float));
  }

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return data_; }

 private:
  std::vector<std::byte> data_;
};

class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  template <typename T>
  T read_pod() {
    if (remaining() < sizeof(T)) throw std::runtime_error("truncated WAL entry");
    T value{};
    std::memcpy(&value, bytes_.data() + offset_, sizeof(T));
    offset_ += sizeof(T);
    return value;
  }

  [[nodiscard]] std::string read_string() {
    const auto length = read_pod<std::uint32_t>();
    if (remaining() < length) throw std::runtime_error("truncated WAL string");
    std::string result(length, '\0');
    std::memcpy(result.data(), bytes_.data() + offset_, length);
    offset_ += length;
    return result;
  }

  [[nodiscard]] std::vector<float> read_vector(std::uint32_t dimensions) {
    const std::size_t byte_count = static_cast<std::size_t>(dimensions) * sizeof(float);
    if (remaining() < byte_count) throw std::runtime_error("truncated WAL vector");
    std::vector<float> result(dimensions);
    std::memcpy(result.data(), bytes_.data() + offset_, byte_count);
    offset_ += byte_count;
    return result;
  }

  [[nodiscard]] bool finished() const noexcept { return offset_ == bytes_.size(); }

 private:
  [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
  std::span<const std::byte> bytes_;
  std::size_t offset_{};
};

std::uint32_t crc32(std::span<const std::byte> input) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::byte byte : input) {
    crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte));
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = static_cast<std::uint32_t>(-(crc & 1U));
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

void write_all(int fd, const void* buffer, std::size_t byte_count) {
  auto* cursor = static_cast<const std::byte*>(buffer);
  std::size_t remaining = byte_count;
  while (remaining > 0U) {
    const auto written = ::write(fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      throw std::system_error(errno, std::generic_category(), "WAL write failed");
    }
    if (written == 0) throw std::runtime_error("WAL write made no progress");
    cursor += written;
    remaining -= static_cast<std::size_t>(written);
  }
}

void encode_payload(Bytes& bytes, const std::vector<PayloadField>& payload) {
  if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("payload exceeds WAL encoding limit");
  }
  bytes.append_pod(static_cast<std::uint32_t>(payload.size()));
  for (const auto& field : payload) {
    bytes.append_string(field.key);
    std::visit(
        [&bytes](const auto& value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, std::string>) {
            bytes.append_pod<std::uint8_t>(0U);
            bytes.append_string(value);
          } else if constexpr (std::is_same_v<Value, std::int64_t>) {
            bytes.append_pod<std::uint8_t>(1U);
            bytes.append_pod(value);
          } else if constexpr (std::is_same_v<Value, double>) {
            bytes.append_pod<std::uint8_t>(2U);
            bytes.append_pod(value);
          } else {
            bytes.append_pod<std::uint8_t>(3U);
            bytes.append_pod<std::uint8_t>(value ? 1U : 0U);
          }
        },
        field.value);
  }
}

std::vector<PayloadField> decode_payload(Reader& reader) {
  const auto count = reader.read_pod<std::uint32_t>();
  if (count > 1'000'000U) throw std::runtime_error("invalid WAL payload field count");
  std::vector<PayloadField> payload;
  payload.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    PayloadField field{.key = reader.read_string(), .value = std::string{}};
    switch (reader.read_pod<std::uint8_t>()) {
      case 0U: field.value = reader.read_string(); break;
      case 1U: field.value = reader.read_pod<std::int64_t>(); break;
      case 2U: field.value = reader.read_pod<double>(); break;
      case 3U: field.value = reader.read_pod<std::uint8_t>() != 0U; break;
      default: throw std::runtime_error("invalid WAL payload value type");
    }
    payload.push_back(std::move(field));
  }
  return payload;
}

std::vector<std::byte> encode_entry(std::uint64_t lsn, WalOperation operation, const Record& record) {
  Bytes bytes;
  bytes.append_pod(lsn);
  bytes.append_pod(static_cast<std::uint8_t>(operation));
  bytes.append_pod(record.id);
  bytes.append_pod(record.generation);
  if (record.vector.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::length_error("vector exceeds WAL encoding limit");
  }
  bytes.append_pod(static_cast<std::uint32_t>(record.vector.size()));
  if (operation == WalOperation::Upsert) {
    bytes.append_vector(record.vector);
    encode_payload(bytes, record.payload);
  }
  return bytes.data();
}

WalEntry decode_entry(std::span<const std::byte> bytes) {
  Reader reader(bytes);
  WalEntry entry{};
  entry.lsn = reader.read_pod<std::uint64_t>();
  const auto operation = reader.read_pod<std::uint8_t>();
  if (operation != static_cast<std::uint8_t>(WalOperation::Upsert) &&
      operation != static_cast<std::uint8_t>(WalOperation::Delete)) {
    throw std::runtime_error("invalid WAL operation");
  }
  entry.operation = static_cast<WalOperation>(operation);
  entry.record.id = reader.read_pod<VectorId>();
  entry.record.generation = reader.read_pod<Generation>();
  const auto dimensions = reader.read_pod<std::uint32_t>();
  if (entry.operation == WalOperation::Upsert) {
    entry.record.vector = reader.read_vector(dimensions);
    entry.record.payload = decode_payload(reader);
  } else if (dimensions != 0U) {
    throw std::runtime_error("delete WAL entry contains a vector dimension");
  }
  if (!reader.finished()) throw std::runtime_error("trailing bytes in WAL entry");
  return entry;
}

}  // namespace

WriteAheadLog::WriteAheadLog(std::filesystem::path path) : path_(std::move(path)) {
  std::filesystem::create_directories(path_.parent_path());
  const auto entries = recover();
  for (const auto& entry : entries) next_lsn_ = std::max(next_lsn_, entry.lsn + 1U);
}

std::uint64_t WriteAheadLog::append_upsert(const Record& record) { return append(WalOperation::Upsert, record); }

std::uint64_t WriteAheadLog::append_delete(VectorId id, Generation generation) {
  return append(WalOperation::Delete, Record{.id = id, .generation = generation});
}

std::uint64_t WriteAheadLog::append(WalOperation operation, const Record& record) {
  std::scoped_lock lock(mutex_);
  const std::uint64_t lsn = next_lsn_;
  const auto payload = encode_entry(lsn, operation, record);
  if (payload.size() > kMaxFrameBytes) throw std::length_error("WAL frame exceeds configured maximum");
  const std::array<std::uint32_t, 3> header{
      kMagic, static_cast<std::uint32_t>(payload.size()), crc32(payload)};
  const int fd = ::open(path_.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0640);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "opening WAL failed");
  try {
    write_all(fd, header.data(), kHeaderBytes);
    write_all(fd, payload.data(), payload.size());
    if (::fsync(fd) != 0) throw std::system_error(errno, std::generic_category(), "WAL fsync failed");
    if (::close(fd) != 0) throw std::system_error(errno, std::generic_category(), "closing WAL failed");
  } catch (...) {
    const int saved_errno = errno;
    (void)::close(fd);
    errno = saved_errno;
    throw;
  }
  ++next_lsn_;
  return lsn;
}

std::vector<WalEntry> WriteAheadLog::recover() const {
  std::scoped_lock lock(mutex_);
  if (!std::filesystem::exists(path_)) return {};
  const int fd = ::open(path_.c_str(), O_RDONLY);
  if (fd < 0) throw std::system_error(errno, std::generic_category(), "opening WAL for recovery failed");
  std::vector<std::byte> bytes;
  try {
    std::array<std::byte, 64U * 1024U> chunk{};
    while (true) {
      const auto read_count = ::read(fd, chunk.data(), chunk.size());
      if (read_count < 0) {
        if (errno == EINTR) continue;
        throw std::system_error(errno, std::generic_category(), "reading WAL failed");
      }
      if (read_count == 0) break;
      bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + read_count);
    }
    (void)::close(fd);
  } catch (...) {
    const int saved_errno = errno;
    (void)::close(fd);
    errno = saved_errno;
    throw;
  }

  std::vector<WalEntry> entries;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (bytes.size() - offset < kHeaderBytes) break;  // torn tail; never acknowledge it.
    std::array<std::uint32_t, 3> header{};
    std::memcpy(header.data(), bytes.data() + offset, kHeaderBytes);
    if (header[0] != kMagic || header[1] > kMaxFrameBytes) {
      throw std::runtime_error("corrupt WAL header");
    }
    const std::size_t frame_size = header[1];
    const std::size_t required = kHeaderBytes + frame_size;
    if (bytes.size() - offset < required) break;  // torn tail
    const std::span<const std::byte> payload(bytes.data() + offset + kHeaderBytes, frame_size);
    if (crc32(payload) != header[2]) throw std::runtime_error("WAL checksum mismatch");
    entries.push_back(decode_entry(payload));
    offset += required;
  }
  return entries;
}

}  // namespace vectordb
