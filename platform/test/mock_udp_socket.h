// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_TEST_MOCK_UDP_SOCKET_H_
#define PLATFORM_TEST_MOCK_UDP_SOCKET_H_

#include <algorithm>
#include <queue>

#include "gmock/gmock.h"
#include "platform/api/udp_socket.h"

namespace openscreen {

class MockUdpSocket : public UdpSocket {
 public:
  MockUdpSocket() = default;
  ~MockUdpSocket() override = default;

  MOCK_METHOD(bool, IsIPv4, (), (const, override));
  MOCK_METHOD(bool, IsIPv6, (), (const, override));
  MOCK_METHOD(IPEndpoint, GetLocalEndpoint, (), (const, override));
  MOCK_METHOD(void, Bind, (), (override));
  MOCK_METHOD(void,
              SetMulticastOutboundInterface,
              (NetworkInterfaceIndex),
              (override));
  MOCK_METHOD(void,
              JoinMulticastGroup,
              (const IPAddress&, NetworkInterfaceIndex),
              (override));
  MOCK_METHOD(void, SendMessage, (ByteView, const IPEndpoint&), (override));
  MOCK_METHOD(void, SetDscp, (UdpSocket::DscpMode), (override));
};

}  // namespace openscreen

#endif  // PLATFORM_TEST_MOCK_UDP_SOCKET_H_
