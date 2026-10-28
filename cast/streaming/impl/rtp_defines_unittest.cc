// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/rtp_defines.h"

#include "gtest/gtest.h"
#include "platform/api/runtime_options.h"
#include "platform/test/scoped_runtime_options.h"

namespace openscreen::cast {
namespace {

TEST(RtpDefinesTest, DefaultParserVersionFollowsRuntimeOptions) {
  RuntimeOptions options;
  options.use_rust_rtp_parser = true;
  {
    ScopedRuntimeOptions scoped(options);
#if defined(USE_RUST_RTP_PARSER)
    EXPECT_EQ(RtpParserVersion::kV2, GetDefaultRtpParserVersion());
#else
    // The Rust parser is not built, so the C++ parser is used.
    EXPECT_EQ(RtpParserVersion::kV1, GetDefaultRtpParserVersion());
#endif  // defined(USE_RUST_RTP_PARSER)
  }

  options.use_rust_rtp_parser = false;
  {
    ScopedRuntimeOptions scoped(options);
    EXPECT_EQ(RtpParserVersion::kV1, GetDefaultRtpParserVersion());
  }
}

}  // namespace
}  // namespace openscreen::cast
