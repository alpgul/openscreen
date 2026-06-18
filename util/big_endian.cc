// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "util/big_endian.h"

namespace openscreen {

BigEndianReader::BigEndianReader(ByteView buffer) : BigEndianBuffer(buffer) {}

BigEndianReader::BigEndianReader(const uint8_t* buffer, size_t length)
    : BigEndianBuffer(buffer, length) {}

bool BigEndianReader::Read(size_t length, void* out) {
  return Read(ByteBuffer(static_cast<uint8_t*>(out), length));
}

bool BigEndianReader::Read(ByteBuffer out) {
  ByteView view = remaining_span();
  if (view.size() >= out.size()) {
    std::copy(view.begin(), view.begin() + out.size(), out.begin());
    Skip(out.size());
    return true;
  }
  return false;
}

BigEndianWriter::BigEndianWriter(ByteBuffer buffer) : BigEndianBuffer(buffer) {}

BigEndianWriter::BigEndianWriter(uint8_t* buffer, size_t length)
    : BigEndianBuffer(buffer, length) {}

bool BigEndianWriter::Write(const void* buffer, size_t length) {
  return Write(ByteView(static_cast<const uint8_t*>(buffer), length));
}

bool BigEndianWriter::Write(ByteView buffer) {
  ByteBuffer view = remaining_span();
  if (view.size() >= buffer.size()) {
    std::copy(buffer.begin(), buffer.end(), view.begin());
    Skip(buffer.size());
    return true;
  }
  return false;
}

}  // namespace openscreen
