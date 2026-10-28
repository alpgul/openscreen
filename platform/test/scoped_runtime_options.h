// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_TEST_SCOPED_RUNTIME_OPTIONS_H_
#define PLATFORM_TEST_SCOPED_RUNTIME_OPTIONS_H_

#include "platform/api/runtime_options.h"

namespace openscreen {

// Sets the runtime options for the life of this object, then restores the
// previous options.
class ScopedRuntimeOptions {
 public:
  explicit ScopedRuntimeOptions(const RuntimeOptions& options)
      : previous_(GetRuntimeOptions()) {
    SetRuntimeOptions(options);
  }

  ScopedRuntimeOptions(const ScopedRuntimeOptions&) = delete;
  ScopedRuntimeOptions& operator=(const ScopedRuntimeOptions&) = delete;

  ~ScopedRuntimeOptions() { SetRuntimeOptions(previous_); }

 private:
  const RuntimeOptions previous_;
};

}  // namespace openscreen

#endif  // PLATFORM_TEST_SCOPED_RUNTIME_OPTIONS_H_
