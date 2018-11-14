// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_API_LOGGING_H_
#define PLATFORM_API_LOGGING_H_

#include <sstream>

namespace openscreen {
namespace platform {

enum class LogLevel {
  kVerbose = 0,
  kInfo,
  kWarning,
  kError,
  kFatal,
};

std::string LogLevelToString(LogLevel level);

//
// PLATFORM IMPLEMENTATION
// The follow functions must be implemented by the platform.
void SetLogLevel(LogLevel level, int verbose_level = 0);
void LogWithLevel(LogLevel level,
                  int verbose_level,
                  const char* file,
                  int line,
                  const char* msg);
void Break();
//
// END PLATFORM IMPLEMENTATION
//

// The stream-based logging macros below are adapted from Chromium's
// base/logging.h.
class LogMessage {
 public:
  LogMessage(LogLevel level, int verbose_level, const char* file, int line);
  ~LogMessage();

  std::ostream& stream() { return stream_; }

 private:
  const LogLevel level_;
  const int verbose_level_;
  const char* const file_;
  const int line_;
  std::ostringstream stream_;
};

#define OSP_VLOG(l)                                                      \
  ::openscreen::platform::LogMessage(                                    \
      ::openscreen::platform::LogLevel::kVerbose, l, __FILE__, __LINE__) \
      .stream()
#define OSP_LOG_INFO                                                          \
  ::openscreen::platform::LogMessage(::openscreen::platform::LogLevel::kInfo, \
                                     0, __FILE__, __LINE__)                   \
      .stream()
#define OSP_LOG_WARN                                                     \
  ::openscreen::platform::LogMessage(                                    \
      ::openscreen::platform::LogLevel::kWarning, 0, __FILE__, __LINE__) \
      .stream()
#define OSP_LOG_ERROR                                                          \
  ::openscreen::platform::LogMessage(::openscreen::platform::LogLevel::kError, \
                                     0, __FILE__, __LINE__)                    \
      .stream()
#define OSP_LOG_FATAL                                                          \
  ::openscreen::platform::LogMessage(::openscreen::platform::LogLevel::kFatal, \
                                     0, __FILE__, __LINE__)                    \
      .stream()

namespace detail {

class Voidify {
 public:
  Voidify() = default;
  void operator&(std::ostream&) {}
};

}  // namespace detail

#define OSP_LAZY_STREAM(stream, condition) \
  !(condition) ? (void)0 : ::openscreen::platform::detail::Voidify() & (stream)
#define OSP_EAT_STREAM OSP_LAZY_STREAM(OSP_LOG_INFO, false)
#define OSP_LOG_IF(level, condition) \
  OSP_LAZY_STREAM(OSP_LOG_##level, (condition))
#define OSP_VLOG_IF(level, condition) \
  OSP_LAZY_STREAM(OSP_VLOG(level), (condition))

// TODO(btolsch): Add tests for (D)OSP_CHECK and possibly logging.
#define OSP_CHECK(condition)                   \
  OSP_LAZY_STREAM(OSP_LOG_FATAL, !(condition)) \
      << "OSP_CHECK(" << #condition << ") failed: "

#define OSP_CHECK_EQ(a, b) OSP_CHECK((a) == (b)) << a << " vs. " << b << ": "
#define OSP_CHECK_NE(a, b) OSP_CHECK((a) != (b)) << a << " vs. " << b << ": "
#define OSP_CHECK_LT(a, b) OSP_CHECK((a) < (b)) << a << " vs. " << b << ": "
#define OSP_CHECK_LE(a, b) OSP_CHECK((a) <= (b)) << a << " vs. " << b << ": "
#define OSP_CHECK_GT(a, b) OSP_CHECK((a) > (b)) << a << " vs. " << b << ": "
#define OSP_CHECK_GE(a, b) OSP_CHECK((a) >= (b)) << a << " vs. " << b << ": "

#if defined(_DEBUG) || defined(OSP_DCHECK_ALWAYS_ON)
#define OSP_DCHECK_IS_ON() 1
#define OSP_DCHECK(condition) OSP_CHECK(condition)
#define OSP_DCHECK_EQ(a, b) OSP_CHECK_EQ(a, b)
#define OSP_DCHECK_NE(a, b) OSP_CHECK_NE(a, b)
#define OSP_DCHECK_LT(a, b) OSP_CHECK_LT(a, b)
#define OSP_DCHECK_LE(a, b) OSP_CHECK_LE(a, b)
#define OSP_DCHECK_GT(a, b) OSP_CHECK_GT(a, b)
#define OSP_DCHECK_GE(a, b) OSP_CHECK_GE(a, b)
#else
#define OSP_DCHECK_IS_ON() 0
#define OSP_DCHECK(condition) OSP_EAT_STREAM
#define OSP_DCHECK_EQ(a, b) OSP_EAT_STREAM
#define OSP_DCHECK_NE(a, b) OSP_EAT_STREAM
#define OSP_DCHECK_LT(a, b) OSP_EAT_STREAM
#define OSP_DCHECK_LE(a, b) OSP_EAT_STREAM
#define OSP_DCHECK_GT(a, b) OSP_EAT_STREAM
#define OSP_DCHECK_GE(a, b) OSP_EAT_STREAM
#endif

#define OSP_DLOG_INFO OSP_LOG_IF(INFO, OSP_DCHECK_IS_ON())
#define OSP_DLOG_WARN OSP_LOG_IF(WARN, OSP_DCHECK_IS_ON())
#define OSP_DLOG_ERROR OSP_LOG_IF(ERROR, OSP_DCHECK_IS_ON())
#define OSP_DLOG_FATAL OSP_LOG_IF(FATAL, OSP_DCHECK_IS_ON())
#define OSP_DLOG_IF(level, condition) \
  OSP_LOG_IF(level, OSP_DCHECK_IS_ON() && (condition))
#define OSP_DVLOG(l) OSP_VLOG_IF(l, OSP_DCHECK_IS_ON())
#define OSP_DVLOG_IF(l, condition) \
  OSP_VLOG_IF(l, OSP_DCHECK_IS_ON() && (condition))

#define OSP_UNIMPLEMENTED() OSP_LOG_ERROR << __func__ << ": unimplemented"

}  // namespace platform
}  // namespace openscreen

#endif  // PLATFORM_API_LOGGING_H_
