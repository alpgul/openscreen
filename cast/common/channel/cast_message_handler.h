// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_COMMON_CHANNEL_CAST_MESSAGE_HANDLER_H_
#define CAST_COMMON_CHANNEL_CAST_MESSAGE_HANDLER_H_

#include "cast/common/channel/proto/cast_channel.pb.h"

namespace openscreen::cast {

class CastSocket;
class VirtualConnectionRouter;

class CastMessageHandler {
 public:
  virtual ~CastMessageHandler() = default;

  // `socket` is null if the source of the message is a local peer.
  virtual void OnMessage(VirtualConnectionRouter* router,
                         CastSocket* socket,
                         proto::CastMessage message) = 0;
};

}  // namespace openscreen::cast

#endif  // CAST_COMMON_CHANNEL_CAST_MESSAGE_HANDLER_H_
