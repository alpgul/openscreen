// Copyright 2023 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_BASE_SPAN_H_
#define PLATFORM_BASE_SPAN_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <cassert>
#include <span>
#include <type_traits>
#include <vector>

#include "platform/base/type_util.h"

namespace openscreen {

// In Open Screen code, use these aliases for the most common types of Spans.
using ByteView = std::span<const uint8_t>;
using ByteBuffer = std::span<uint8_t>;

// Contains a pointer and length to a span of contiguous data. Currently just
// a using statement for std::span.
// TODO(crbug.com/364687926): remove in favor of direct use of std::span.
//
// The API is a slimmed-down version of a C++20 std::span<T> and is intended to
// be forwards-compatible with very slight modifications.  We don't intend to
// add support for static extents.
//
// - Unit tests that want to compare the bytes behind two ByteViews can use
//   ExpectByteViewsHaveSameBytes().
template <typename T>
using Span = std::span<T>;

}  // namespace openscreen

#endif  // PLATFORM_BASE_SPAN_H_
