// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UTIL_BIG_ENDIAN_H_
#define UTIL_BIG_ENDIAN_H_

#include <stdint.h>

#include <algorithm>
#include <cstring>
#include <type_traits>

#include "platform/base/span.h"
#include "util/raw_ptr.h"

namespace openscreen {

////////////////////////////////////////////////////////////////////////////////
// Note: All of the functions here are defined inline, as any half-decent
// compiler will optimize them to a single integer constant or single
// instruction on most architectures.
////////////////////////////////////////////////////////////////////////////////

// Returns true if this code is running on a big-endian architecture.
inline bool IsBigEndianArchitecture() {
  const uint16_t kTestWord = 0x0100;
  uint8_t bytes[sizeof(kTestWord)];
  memcpy(bytes, &kTestWord, sizeof(bytes));
  return !!bytes[0];
}

namespace internal {

template <int size>
struct MakeSizedUnsignedInteger;

template <>
struct MakeSizedUnsignedInteger<1> {
  using type = uint8_t;
};

template <>
struct MakeSizedUnsignedInteger<2> {
  using type = uint16_t;
};

template <>
struct MakeSizedUnsignedInteger<4> {
  using type = uint32_t;
};

template <>
struct MakeSizedUnsignedInteger<8> {
  using type = uint64_t;
};

template <int size>
inline typename MakeSizedUnsignedInteger<size>::type ByteSwap(
    typename MakeSizedUnsignedInteger<size>::type x) {
  static_assert(size <= 8,
                "ByteSwap() specialization missing in " __FILE__
                ". "
                "Are you trying to use an integer larger than 64 bits?");
}

template <>
inline uint8_t ByteSwap<1>(uint8_t x) {
  return x;
}

#if defined(__clang__) || defined(__GNUC__)

template <>
inline uint64_t ByteSwap<8>(uint64_t x) {
  return __builtin_bswap64(x);
}
template <>
inline uint32_t ByteSwap<4>(uint32_t x) {
  return __builtin_bswap32(x);
}
template <>
inline uint16_t ByteSwap<2>(uint16_t x) {
  return __builtin_bswap16(x);
}

#elif defined(_MSC_VER)

template <>
inline uint64_t ByteSwap<8>(uint64_t x) {
  return _byteswap_uint64(x);
}
template <>
inline uint32_t ByteSwap<4>(uint32_t x) {
  return _byteswap_ulong(x);
}
template <>
inline uint16_t ByteSwap<2>(uint16_t x) {
  return _byteswap_ushort(x);
}

#else

#include <byteswap.h>

template <>
inline uint64_t ByteSwap<8>(uint64_t x) {
  return bswap_64(x);
}
template <>
inline uint32_t ByteSwap<4>(uint32_t x) {
  return bswap_32(x);
}
template <>
inline uint16_t ByteSwap<2>(uint16_t x) {
  return bswap_16(x);
}

#endif

}  // namespace internal

// Returns the bytes of `x` in reverse order. This is only defined for 16-, 32-,
// and 64-bit unsigned integers.
template <typename Integer>
inline std::enable_if_t<std::is_unsigned<Integer>::value, Integer> ByteSwap(
    Integer x) {
  return internal::ByteSwap<sizeof(Integer)>(x);
}

// Read a POD integer from `src` in big-endian byte order, returning the integer
// in native byte order.
template <typename Integer>
inline Integer ReadBigEndian(const void* src) {
  Integer result;
  memcpy(&result, src, sizeof(result));
  if (!IsBigEndianArchitecture()) {
    result = ByteSwap<typename std::make_unsigned<Integer>::type>(result);
  }
  return result;
}

// Write a POD integer `val` to `dest` in big-endian byte order.
template <typename Integer>
inline void WriteBigEndian(Integer val, void* dest) {
  if (!IsBigEndianArchitecture()) {
    val = ByteSwap<typename std::make_unsigned<Integer>::type>(val);
  }
  memcpy(dest, &val, sizeof(val));
}

template <class T>
class BigEndianBuffer {
 public:
  class Cursor {
   public:
    explicit Cursor(BigEndianBuffer* buffer)
        : buffer_(buffer), origin_offset_(buffer_->offset()) {}
    Cursor(const Cursor& other) = delete;
    Cursor(Cursor&& other) noexcept = delete;
    ~Cursor() { buffer_->set_offset(origin_offset_); }

    Cursor& operator=(const Cursor& other) = delete;
    Cursor& operator=(Cursor&& other) noexcept = delete;

    void Commit() { origin_offset_ = buffer_->offset(); }

    size_t origin_offset() const { return origin_offset_; }
    T* origin() const { return buffer_->begin() + origin_offset_; }
    size_t delta() const { return buffer_->offset() - origin_offset_; }

   private:
    raw_ptr<BigEndianBuffer<T>> buffer_;
    size_t origin_offset_;
  };

  bool Skip(size_t length) {
    if (length > remaining()) {
      return false;
    }
    offset_ += length;
    return true;
  }

  Span<T> buffer() const { return buffer_; }
  Span<T> remaining_span() const { return buffer_.subspan(offset_); }
  // TODO(crbug.com/520101123): Remove unsafe raw pointer and length methods.
  T* begin() const { return buffer_.data(); }
  T* current() const { return buffer_.data() + offset_; }
  T* end() const { return buffer_.data() + buffer_.size(); }
  size_t length() const { return buffer_.size(); }
  size_t remaining() const { return buffer_.size() - offset_; }
  size_t offset() const { return offset_; }

  explicit BigEndianBuffer(Span<T> buffer) : buffer_(buffer) {}
  // TODO(crbug.com/520101123): Remove unsafe raw pointer and length methods.
  BigEndianBuffer(T* buffer, size_t length) : buffer_(buffer, length) {}
  BigEndianBuffer(const BigEndianBuffer&) = delete;
  BigEndianBuffer& operator=(const BigEndianBuffer&) = delete;

 protected:
  void set_offset(size_t offset) { offset_ = offset; }

 private:
  Span<T> buffer_;
  size_t offset_ = 0;
};

class BigEndianReader : public BigEndianBuffer<const uint8_t> {
 public:
  explicit BigEndianReader(ByteView buffer);
  // TODO(crbug.com/520101123): Remove unsafe raw pointer and length methods.
  BigEndianReader(const uint8_t* buffer, size_t length);

  template <typename T>
  bool Read(T* out) {
    ByteView view = remaining_span();
    if (view.size() >= sizeof(T)) {
      *out = ReadBigEndian<T>(view.data());
      Skip(sizeof(T));
      return true;
    }
    return false;
  }

  // TODO(crbug.com/520101123): Remove unsafe raw pointer and length methods.
  bool Read(size_t length, void* out);
  bool Read(ByteBuffer out);
};

class BigEndianWriter : public BigEndianBuffer<uint8_t> {
 public:
  explicit BigEndianWriter(ByteBuffer buffer);
  // TODO(crbug.com/520101123): Remove unsafe raw pointer and length methods.
  BigEndianWriter(uint8_t* buffer, size_t length);

  template <typename T>
  bool Write(T value) {
    ByteBuffer view = remaining_span();
    if (view.size() >= sizeof(T)) {
      WriteBigEndian<T>(value, view.data());
      Skip(sizeof(T));
      return true;
    }
    return false;
  }

  // TODO(crbug.com/520101123): Remove unsafe raw pointer and length methods.
  bool Write(const void* buffer, size_t length);
  bool Write(ByteView buffer);
};

}  // namespace openscreen

#endif  // UTIL_BIG_ENDIAN_H_
