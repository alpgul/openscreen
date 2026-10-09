// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_PUBLIC_DATA_SESSION_H_
#define CAST_STREAMING_PUBLIC_DATA_SESSION_H_

#include <memory>
#include <string_view>

#include "cast/streaming/public/data_channel.h"
#include "platform/base/error.h"

namespace openscreen::cast {

// A negotiated data session with the remote endpoint. Obtained from
// SenderSession::Client::OnDataSessionOpened(); the SenderSession owns it
// and documents how long it stays valid.
class DataSession {
 public:
  virtual ~DataSession() = default;

  // Opens a new outgoing data channel with the given label. Channels keep
  // working until the data session closes, after which they move to
  // DataChannel::State::kClosed.
  virtual ErrorOr<std::unique_ptr<DataChannel>> CreateDataChannel(
      std::string_view label) = 0;
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_PUBLIC_DATA_SESSION_H_
