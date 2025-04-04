// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_COMMON_CHANNEL_TESTING_MOCK_SOCKET_ERROR_HANDLER_H_
#define CAST_COMMON_CHANNEL_TESTING_MOCK_SOCKET_ERROR_HANDLER_H_

#include "cast/common/channel/virtual_connection_router.h"
#include "gmock/gmock.h"
#include "platform/base/error.h"

namespace openscreen::cast {

class MockSocketErrorHandler
    : public VirtualConnectionRouter::SocketErrorHandler {
 public:
  MOCK_METHOD(void, OnClose, (CastSocket * socket), (override));
  MOCK_METHOD(void,
              OnError,
              (CastSocket * socket, const Error& error),
              (override));
};

}  // namespace openscreen::cast

#endif  // CAST_COMMON_CHANNEL_TESTING_MOCK_SOCKET_ERROR_HANDLER_H_
