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

namespace openscreen {
namespace platform {
namespace internal {

using ::testing::_;
using ::testing::Invoke;

class MockLoggingPlatform : public TraceLoggingPlatform {
 public:
  MOCK_METHOD9(LogTrace,
               void(const char*,
                    const uint32_t,
                    const char* file,
                    Clock::time_point,
                    Clock::time_point,
                    TraceId,
                    TraceId,
                    TraceId,
                    Error::Code));
  MOCK_METHOD7(LogAsyncStart,
               void(const char*,
                    const uint32_t,
                    const char* file,
                    Clock::time_point,
                    TraceId,
                    TraceId,
                    TraceId));
  MOCK_METHOD5(LogAsyncEnd,
               void(const uint32_t,
                    const char* file,
                    Clock::time_point,
                    TraceId,
                    Error::Code));
};

// Methods to validate the results of platform-layer calls.
template <uint64_t milliseconds>
void ValidateTraceTimestampDiff(const char* name,
                                const uint32_t line,
                                const char* file,
                                Clock::time_point start_time,
                                Clock::time_point end_time,
                                TraceId trace_id,
                                TraceId parent_id,
                                TraceId root_id,
                                Error error) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      end_time - start_time);
  EXPECT_GE(elapsed.count(), milliseconds);
}

template <Error::Code result>
void ValidateTraceErrorCode(const char* name,
                            const uint32_t line,
                            const char* file,
                            Clock::time_point start_time,
                            Clock::time_point end_time,
                            TraceId trace_id,
                            TraceId parent_id,
                            TraceId root_id,
                            Error error) {
  EXPECT_EQ(error.code(), result);
}

// These tests validate that parameters are passed correctly by using the Trace
// Internals.
constexpr TraceCategory::Value category =
    TraceCategory::Value::CastPlatformLayer;
constexpr uint32_t line = 10;

TEST(TraceLoggingInternalTest, CreatingNoTraceObjectValid) {
  TraceInstanceHelper<SynchronousTraceLogger>::Empty();
}

TEST(TraceLoggingInternalTest, TestMacroStyleInitializationTrue) {
  constexpr uint32_t delay_in_ms = 50;
  MockLoggingPlatform platform;
  TRACE_SET_DEFAULT_PLATFORM(&platform);
  EXPECT_CALL(platform, LogTrace(_, _, _, _, _, _, _, _, _))
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
  EXPECT_CALL(platform, LogTrace(_, _, _, _, _, _, _, _, _)).Times(0);

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
                                 testing::StrEq(__FILE__), _, _, _, _, _, _))
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
                                      testing::StrEq(__FILE__), _, _, _, _))
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
