// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <iostream>
#include <thread>

#include "absl/types/optional.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/api/trace_logging.h"
#include "platform/test/trace_logging_helpers.h"

// TODO(issue/52): Remove duplicate code from trace logging+internal unit tests
namespace openscreen {
namespace platform {
namespace internal {

using ::testing::_;
using ::testing::DoAll;
using ::testing::Invoke;

// These tests validate that parameters are passed correctly by using the Trace
// Internals.
constexpr TraceCategory::Value category = TraceCategory::mDNS;
constexpr uint32_t line = 10;

TEST(TraceLoggingInternalTest, CreatingNoTraceObjectValid) {
  TraceInstanceHelper<SynchronousTraceLogger>::Empty();
}

TEST(TraceLoggingInternalTest, TestMacroStyleInitializationTrue) {
  constexpr uint32_t delay_in_ms = 50;
  MockLoggingPlatform platform;
  TRACE_SET_DEFAULT_PLATFORM(&platform);
  EXPECT_CALL(platform, LogTrace(_, _, _, _, _, _, _))
      .Times(1)
      .WillOnce(DoAll(Invoke(ValidateTraceTimestampDiff<delay_in_ms>),
                      Invoke(ValidateTraceErrorCode<Error::Code::kNone>)));

  {
    uint8_t temp[sizeof(SynchronousTraceLogger)];
    auto ptr = true ? TraceInstanceHelper<SynchronousTraceLogger>::Create(
                          temp, category, "Name", __FILE__, line)
                    : TraceInstanceHelper<SynchronousTraceLogger>::Empty();
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_in_ms));
    auto ids = ScopedTraceOperation::hierarchy();
    EXPECT_NE(ids.current, kEmptyTraceId);
  }
  auto ids2 = ScopedTraceOperation::hierarchy();
  EXPECT_EQ(ids2.current, kEmptyTraceId);
  EXPECT_EQ(ids2.parent, kEmptyTraceId);
  EXPECT_EQ(ids2.root, kEmptyTraceId);
}

TEST(TraceLoggingInternalTest, TestMacroStyleInitializationFalse) {
  MockLoggingPlatform platform;
  TRACE_SET_DEFAULT_PLATFORM(&platform);
  EXPECT_CALL(platform, LogTrace(_, _, _, _, _, _, _)).Times(0);

  {
    uint8_t temp[sizeof(SynchronousTraceLogger)];
    auto ptr = false ? TraceInstanceHelper<SynchronousTraceLogger>::Create(
                           temp, category, "Name", __FILE__, line)
                     : TraceInstanceHelper<SynchronousTraceLogger>::Empty();
    auto ids = ScopedTraceOperation::hierarchy();
    EXPECT_EQ(ids.current, kEmptyTraceId);
    EXPECT_EQ(ids.parent, kEmptyTraceId);
    EXPECT_EQ(ids.root, kEmptyTraceId);
  }
  auto ids2 = ScopedTraceOperation::hierarchy();
  EXPECT_EQ(ids2.current, kEmptyTraceId);
  EXPECT_EQ(ids2.parent, kEmptyTraceId);
  EXPECT_EQ(ids2.root, kEmptyTraceId);
}

TEST(TraceLoggingInternalTest, ExpectParametersPassedToResult) {
  MockLoggingPlatform platform;
  TRACE_SET_DEFAULT_PLATFORM(&platform);
  EXPECT_CALL(platform, LogTrace(testing::StrEq("Name"), line,
                                 testing::StrEq(__FILE__), _, _, _, _))
      .WillOnce(Invoke(ValidateTraceErrorCode<Error::Code::kNone>));

  {
    TRACE_INTERNAL_IGNORE_UNUSED_VAR const TraceBase&
        TRACE_INTERNAL_UNIQUE_VAR_NAME(trace_ref) =
            SynchronousTraceLogger{category, "Name", __FILE__, line};
  }
}

TEST(TraceLoggingInternalTest, CheckTraceAsyncStartLogsCorrectly) {
  MockLoggingPlatform platform;
  TRACE_SET_DEFAULT_PLATFORM(&platform);
  EXPECT_CALL(platform, LogAsyncStart(testing::StrEq("Name"), line,
                                      testing::StrEq(__FILE__), _, _))
      .Times(1);

  { AsynchronousTraceLogger{category, "Name", __FILE__, line}; }
}

TEST(TraceLoggingInternalTest, ValidateGettersValidOnEmptyStack) {
  EXPECT_EQ(ScopedTraceOperation::current_id(), kEmptyTraceId);
  EXPECT_EQ(ScopedTraceOperation::root_id(), kEmptyTraceId);

  auto ids = ScopedTraceOperation::hierarchy();
  EXPECT_EQ(ids.current, kEmptyTraceId);
  EXPECT_EQ(ids.parent, kEmptyTraceId);
  EXPECT_EQ(ids.root, kEmptyTraceId);
}

TEST(TraceLoggingInternalTest, ValidateSetResultDoesntSegfaultOnEmptyStack) {
  Error error = Error::Code::kNone;
  ScopedTraceOperation::set_result(error);

  ScopedTraceOperation::set_result(Error::Code::kNone);
}

}  // namespace internal
}  // namespace platform
}  // namespace openscreen
