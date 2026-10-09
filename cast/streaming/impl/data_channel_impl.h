// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_DATA_CHANNEL_IMPL_H_
#define CAST_STREAMING_IMPL_DATA_CHANNEL_IMPL_H_

#include <string>
#include <string_view>

#include "cast/streaming/impl/data_channel_stream.h"
#include "cast/streaming/public/data_channel.h"
#include "util/raw_ptr.h"

namespace openscreen::cast {

class DataChannelImpl final : public DataChannel,
                              public DataChannelStream::Delegate {
 public:
  explicit DataChannelImpl(DataChannelStream* stream);
  ~DataChannelImpl() override;

  // DataChannel overrides.
  Error Send(ByteView data) override;
  void Close() override;
  std::string_view label() const override;
  State state() const override;
  void SetDelegate(DataChannel::Delegate* delegate) override;

  // DataChannelStream::Delegate overrides.
  void OnMessage(ByteView data) override;
  void OnStateChange(DataChannel::State state) override;
  void OnWriteReady() override;
  void OnDestroyed() override;

 private:
  // Owned by the DataChannelSession. If the stream is destroyed first, it
  // nulls this out via OnDestroyed(). If it is still set when this object is
  // destroyed, the destructor detaches from the stream and closes it.
  raw_ptr<DataChannelStream> stream_ = nullptr;
  // Owned by the client, which must keep it alive or clear it via
  // SetDelegate() while callbacks may arrive. Not used by the destructor.
  raw_ptr<DataChannel::Delegate> delegate_ = nullptr;
  std::string cached_label_;
  State cached_state_ = State::kConnecting;
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_DATA_CHANNEL_IMPL_H_
