// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_COMMON_CHANNEL_TESTING_FAKE_VIRTUAL_CONNECTION_POLICY_H_
#define CAST_COMMON_CHANNEL_TESTING_FAKE_VIRTUAL_CONNECTION_POLICY_H_

#include "cast/common/channel/connection_namespace_handler.h"

namespace openscreen::cast {

// A VirtualConnectionPolicy that allows every connection. Useful for tests
// that only exercise the initiator role of ConnectionNamespaceHandler (e.g.
// via VirtualConnectionRouter::OpenRemoteConnection()) and never receive an
// inbound CONNECT request, so the actual policy decision is never exercised.
class FakeVirtualConnectionPolicy
    : public ConnectionNamespaceHandler::VirtualConnectionPolicy {
 public:
  bool IsConnectionAllowed(
      const VirtualConnection& virtual_conn) const override {
    return true;
  }
};

}  // namespace openscreen::cast

#endif  // CAST_COMMON_CHANNEL_TESTING_FAKE_VIRTUAL_CONNECTION_POLICY_H_
