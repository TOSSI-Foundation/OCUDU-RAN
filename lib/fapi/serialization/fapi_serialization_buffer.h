#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ocudu {
namespace fapi_serial {

class buffer_writer
{
  uint8_t* data_;
  uint32_t capacity_;
  uint32_t offset_ = 0;

public:
  buffer_writer(void* data, uint32_t capacity) : data_(static_cast<uint8_t*>(data)), capacity_(capacity) {}

  /// Returns the number of bytes written so far.
  uint32_t bytes_written() const { return offset_; }

  /// Returns pointer to the underlying data.
  uint8_t* data() const { return data_; }

  /// Returns remaining capacity in bytes.
  uint32_t remaining() const { return capacity_ - offset_; }

  void write_u8(uint8_t v)
  {
    ensure_space(1);
    data_[offset_++] = v;
  }

  void write_i8(int8_t v)
  {
    ensure_space(1);
    data_[offset_++] = static_cast<uint8_t>(v);
  }

  void write_bool(bool v) { write_u8(v ? 1 : 0); }

  void write_u16(uint16_t v)
  {
    ensure_space(2);
    std::memcpy(data_ + offset_, &v, 2);
    offset_ += 2;
  }

  void write_i16(int16_t v)
  {
    ensure_space(2);
    std::memcpy(data_ + offset_, &v, 2);
    offset_ += 2;
  }

  void write_u32(uint32_t v)
  {
    ensure_space(4);
    std::memcpy(data_ + offset_, &v, 4);
    offset_ += 4;
  }

  void write_i32(int32_t v)
  {
    ensure_space(4);
    std::memcpy(data_ + offset_, &v, 4);
    offset_ += 4;
  }

  void write_u64(uint64_t v)
  {
    ensure_space(8);
    std::memcpy(data_ + offset_, &v, 8);
    offset_ += 8;
  }

  void write_i64(int64_t v)
  {
    ensure_space(8);
    std::memcpy(data_ + offset_, &v, 8);
    offset_ += 8;
  }

  void write_float(float v)
  {
    ensure_space(4);
    std::memcpy(data_ + offset_, &v, 4);
    offset_ += 4;
  }

  void write_double(double v)
  {
    ensure_space(8);
    std::memcpy(data_ + offset_, &v, 8);
    offset_ += 8;
  }

  /// Write raw bytes from a source buffer.
  void write_bytes(const void* src, uint32_t len)
  {
    if (len == 0) {
      return;
    }
    ensure_space(len);
    std::memcpy(data_ + offset_, src, len);
    offset_ += len;
  }

  /// Write a fixed-size array of uint8_t.
  template <std::size_t N>
  void write_array_u8(const std::array<uint8_t, N>& arr)
  {
    write_bytes(arr.data(), N);
  }

  /// Write a fixed-size array of uint16_t.
  template <std::size_t N>
  void write_array_u16(const std::array<uint16_t, N>& arr)
  {
    for (std::size_t i = 0; i < N; ++i) {
      write_u16(arr[i]);
    }
  }

  /// Write a fixed-size array of uint32_t.
  template <std::size_t N>
  void write_array_u32(const std::array<uint32_t, N>& arr)
  {
    for (std::size_t i = 0; i < N; ++i) {
      write_u32(arr[i]);
    }
  }

private:
  void ensure_space(uint32_t n)
  {
    if (offset_ + n > capacity_) {
      throw std::runtime_error("fapi_serial::buffer_writer overflow: need " + std::to_string(n) +
                               " bytes, have " + std::to_string(capacity_ - offset_));
    }
  }
};

class buffer_reader
{
  const uint8_t* data_;
  uint32_t       size_;
  uint32_t       offset_ = 0;

public:
  buffer_reader(const void* data, uint32_t size) : data_(static_cast<const uint8_t*>(data)), size_(size) {}

  /// Returns the number of bytes read so far.
  uint32_t bytes_read() const { return offset_; }

  /// Returns remaining bytes available.
  uint32_t remaining() const { return size_ - offset_; }

  /// Returns pointer to current read position.
  const uint8_t* current() const { return data_ + offset_; }

  uint8_t read_u8()
  {
    ensure_available(1);
    return data_[offset_++];
  }

  int8_t read_i8()
  {
    ensure_available(1);
    return static_cast<int8_t>(data_[offset_++]);
  }

  bool read_bool() { return read_u8() != 0; }

  uint16_t read_u16()
  {
    ensure_available(2);
    uint16_t v;
    std::memcpy(&v, data_ + offset_, 2);
    offset_ += 2;
    return v;
  }

  int16_t read_i16()
  {
    ensure_available(2);
    int16_t v;
    std::memcpy(&v, data_ + offset_, 2);
    offset_ += 2;
    return v;
  }

  uint32_t read_u32()
  {
    ensure_available(4);
    uint32_t v;
    std::memcpy(&v, data_ + offset_, 4);
    offset_ += 4;
    return v;
  }

  int32_t read_i32()
  {
    ensure_available(4);
    int32_t v;
    std::memcpy(&v, data_ + offset_, 4);
    offset_ += 4;
    return v;
  }

  uint64_t read_u64()
  {
    ensure_available(8);
    uint64_t v;
    std::memcpy(&v, data_ + offset_, 8);
    offset_ += 8;
    return v;
  }

  int64_t read_i64()
  {
    ensure_available(8);
    int64_t v;
    std::memcpy(&v, data_ + offset_, 8);
    offset_ += 8;
    return v;
  }

  float read_float()
  {
    ensure_available(4);
    float v;
    std::memcpy(&v, data_ + offset_, 4);
    offset_ += 4;
    return v;
  }

  double read_double()
  {
    ensure_available(8);
    double v;
    std::memcpy(&v, data_ + offset_, 8);
    offset_ += 8;
    return v;
  }

  /// Read raw bytes into a destination buffer.
  void read_bytes(void* dst, uint32_t len)
  {
    if (len == 0) {
      return;
    }
    ensure_available(len);
    std::memcpy(dst, data_ + offset_, len);
    offset_ += len;
  }

  /// Read a fixed-size array of uint8_t.
  template <std::size_t N>
  void read_array_u8(std::array<uint8_t, N>& arr)
  {
    read_bytes(arr.data(), N);
  }

  /// Read a fixed-size array of uint16_t.
  template <std::size_t N>
  void read_array_u16(std::array<uint16_t, N>& arr)
  {
    for (std::size_t i = 0; i < N; ++i) {
      arr[i] = read_u16();
    }
  }

  /// Read a fixed-size array of uint32_t.
  template <std::size_t N>
  void read_array_u32(std::array<uint32_t, N>& arr)
  {
    for (std::size_t i = 0; i < N; ++i) {
      arr[i] = read_u32();
    }
  }

  /// Skip over bytes without reading them.
  void skip(uint32_t n)
  {
    ensure_available(n);
    offset_ += n;
  }

private:
  void ensure_available(uint32_t n)
  {
    if (offset_ + n > size_) {
      throw std::runtime_error("fapi_serial::buffer_reader underflow: need " + std::to_string(n) +
                               " bytes, have " + std::to_string(size_ - offset_));
    }
  }
};

} // namespace fapi_serial
} // namespace ocudu
