// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file

#include "platform/base/udp_packet.h"

#include "util/logging.h"

namespace openscreen {
namespace platform {

UdpPacket::UdpPacket() : std::vector<uint8_t>() {}

UdpPacket::UdpPacket(size_type size) : std::vector<uint8_t>(size) {
  OSP_DCHECK_LE(size, kUdpMaxPacketSize);
}

UdpPacket::UdpPacket(size_type size, uint8_t fill_value)
    : std::vector<uint8_t>(size, fill_value) {
  OSP_DCHECK_LE(size, kUdpMaxPacketSize);
}

UdpPacket::UdpPacket(UdpPacket&& other) = default;

UdpPacket::UdpPacket(std::initializer_list<uint8_t> init)
    : std::vector<uint8_t>(init) {
  OSP_DCHECK_LE(size(), kUdpMaxPacketSize);
}

UdpPacket::~UdpPacket() = default;

UdpPacket& UdpPacket::operator=(UdpPacket&& other) = default;

}  // namespace platform
}  // namespace openscreen
