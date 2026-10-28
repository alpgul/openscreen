// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/api/runtime_options.h"

#include "gtest/gtest.h"
#include "platform/test/scoped_runtime_options.h"

namespace openscreen {
namespace {

TEST(RuntimeOptionsTest, ReturnsLibraryDefaults) {
  const RuntimeOptions defaults;
  const RuntimeOptions options = GetRuntimeOptions();
  EXPECT_EQ(defaults.use_rust_json, options.use_rust_json);
  EXPECT_EQ(defaults.use_rust_rtp_parser, options.use_rust_rtp_parser);
}

TEST(RuntimeOptionsTest, OverridesAndRestores) {
  const RuntimeOptions before = GetRuntimeOptions();
  {
    RuntimeOptions overrides;
    overrides.use_rust_json = !before.use_rust_json;
    ScopedRuntimeOptions scoped(overrides);

    const RuntimeOptions options = GetRuntimeOptions();
    EXPECT_EQ(!before.use_rust_json, options.use_rust_json);
    EXPECT_EQ(before.use_rust_rtp_parser, options.use_rust_rtp_parser);
  }
  const RuntimeOptions after = GetRuntimeOptions();
  EXPECT_EQ(before.use_rust_json, after.use_rust_json);
  EXPECT_EQ(before.use_rust_rtp_parser, after.use_rust_rtp_parser);
}

}  // namespace
}  // namespace openscreen
