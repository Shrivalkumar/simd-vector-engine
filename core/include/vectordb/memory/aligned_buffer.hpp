#pragma once

#include <bit>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace vectordb {

template <typename T, std::size_t Alignment = 128>
class AlignedBuffer {
  static_assert(std::has_single_bit(Alignment));

 public:
  explicit AlignedBuffer(std::size_t size = 0) { resize(size); }
  AlignedBuffer(const AlignedBuffer&) = delete;
  AlignedBuffer& operator=(const AlignedBuffer&) = delete;
  AlignedBuffer(AlignedBuffer&& other) noexcept { swap(other); }
  AlignedBuffer& operator=(AlignedBuffer&& other) noexcept {
    if (this != &other) {
      reset();
      swap(other);
    }
    return *this;
  }
  ~AlignedBuffer() { reset(); }

  void resize(std::size_t new_size) {
    if (new_size == size_) return;
    AlignedBuffer replacement(new_size, PrivateTag{});
    const std::size_t count = (new_size < size_) ? new_size : size_;
    for (std::size_t i = 0; i < count; ++i) replacement.data_[i] = std::move(data_[i]);
    swap(replacement);
  }

  [[nodiscard]] T* data() noexcept { return data_; }
  [[nodiscard]] const T* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] T& operator[](std::size_t index) noexcept { return data_[index]; }
  [[nodiscard]] const T& operator[](std::size_t index) const noexcept { return data_[index]; }

 private:
  struct PrivateTag {};
  AlignedBuffer(std::size_t size, PrivateTag) : size_(size) {
    if (size_ == 0) return;
    data_ = static_cast<T*>(::operator new[](size_ * sizeof(T), std::align_val_t{Alignment}));
    try {
      for (std::size_t i = 0; i < size_; ++i) std::construct_at(data_ + i);
    } catch (...) {
      ::operator delete[](data_, std::align_val_t{Alignment});
      data_ = nullptr;
      size_ = 0;
      throw;
    }
  }

  void reset() noexcept {
    if (data_ == nullptr) return;
    for (std::size_t i = 0; i < size_; ++i) std::destroy_at(data_ + i);
    ::operator delete[](data_, std::align_val_t{Alignment});
    data_ = nullptr;
    size_ = 0;
  }

  void swap(AlignedBuffer& other) noexcept {
    std::swap(data_, other.data_);
    std::swap(size_, other.size_);
  }

  T* data_{};
  std::size_t size_{};
};

}  // namespace vectordb
