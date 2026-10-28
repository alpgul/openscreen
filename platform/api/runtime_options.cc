// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/api/runtime_options.h"

#include <atomic>

namespace openscreen {

namespace {

// Lock-free, so that it needs no locking, no exit-time destructor, and no
// libatomic. If a new field makes RuntimeOptions too big for this, the
// static_assert below fails and the storage needs a different approach.
static_assert(std::atomic<RuntimeOptions>::is_always_lock_free);
constinit std::atomic<RuntimeOptions> g_runtime_options{RuntimeOptions{}};

}  // namespace

RuntimeOptions GetRuntimeOptions() {
  return g_runtime_options.load(std::memory_order_acquire);
}

void SetRuntimeOptions(const RuntimeOptions& options) {
  g_runtime_options.store(options, std::memory_order_release);
}

}  // namespace openscreen
