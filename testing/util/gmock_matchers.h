// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TESTING_UTIL_GMOCK_MATCHERS_H_
#define TESTING_UTIL_GMOCK_MATCHERS_H_

#include <sstream>
#include <string>

#include "gmock/gmock.h"
#include "util/chrono_helpers.h"

namespace openscreen {

// Gmock matcher for std::chrono::duration types. This is especially useful for
// Clock::duration, as it provides much more readable error messages than the
// default Gmock matchers.
//
// Example usage:
//   EXPECT_THAT(my_duration, EqualsDuration(milliseconds(100)));
//
// Example error output:
//   Value of: my_duration
//   Expected: 100 ms
//     Actual: 98 ms (a difference of -2 ms)
template <typename T>
std::string ToString(T duration) {
  std::ostringstream ss;
  openscreen::clock_operators::operator<<(ss, duration);
  return ss.str();
}

MATCHER_P(EqualsDuration, expected, ToString(expected)) {
  if (arg == expected) {
    return true;
  }
  *result_listener << ToString(arg) << " (a difference of "
                   << ToString(arg - expected) << ")";
  return false;
}

}  // namespace openscreen

#endif  // TESTING_UTIL_GMOCK_MATCHERS_H_
