// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/packet_util.h"

#include "cast/streaming/impl/rtcp_common.h"
#include "cast/streaming/impl/rtp_defines.h"

#if defined(USE_RUST_RTP_PARSER)
#include "cast/streaming/impl/rtp_wire.rs.h"
#endif  // defined(USE_RUST_RTP_PARSER)

namespace openscreen::cast {

namespace {

#if defined(USE_RUST_RTP_PARSER)
// Memory-safe Rust wire parser implementation (via CXX FFI) in
// //cast/streaming/impl/rtp_wire.rs.
std::pair<ApparentPacketType, Ssrc> InspectPacketForRoutingV2(ByteView packet) {
  const rust::Slice<const uint8_t> slice(packet.data(), packet.size());
  const RoutingResult result = inspect_packet_for_routing(slice);
  switch (result.apparent_type) {
    case ApparentType::Rtp:
      return std::make_pair(ApparentPacketType::RTP, Ssrc{result.ssrc});
    case ApparentType::Rtcp:
      return std::make_pair(ApparentPacketType::RTCP, Ssrc{result.ssrc});
    default:
      return std::make_pair(ApparentPacketType::UNKNOWN, Ssrc{0});
  }
}
#endif  // defined(USE_RUST_RTP_PARSER)

// Original C++ wire parser implementation.
std::pair<ApparentPacketType, Ssrc> InspectPacketForRoutingV1(ByteView packet) {
  // Check for RTP packets first, since they are more frequent.
  if (packet.size() >= kRtpPacketMinValidSize &&
      packet[0] == kRtpRequiredFirstByte &&
      IsRtpPayloadType(packet[1] & kRtpPayloadTypeMask)) {
    constexpr int kOffsetToSsrcField = 8;
    return std::make_pair(
        ApparentPacketType::RTP,
        Ssrc{ReadBigEndian<uint32_t>(packet.subspan(kOffsetToSsrcField))});
  }

  // While RTCP packets are valid if they consist of just the RTCP Common
  // Header, all the RTCP packet types processed by this implementation will
  // also have a SSRC field immediately following the header. This is important
  // for routing the packet to the correct parser instance.
  constexpr int kRtcpPacketMinAcceptableSize =
      kRtcpCommonHeaderSize + sizeof(uint32_t);
  if (packet.size() >= kRtcpPacketMinAcceptableSize &&
      RtcpCommonHeader::Parse(packet, RtpParserVersion::kV1).has_value()) {
    return std::make_pair(
        ApparentPacketType::RTCP,
        Ssrc{ReadBigEndian<uint32_t>(packet.subspan(kRtcpCommonHeaderSize))});
  }

  return std::make_pair(ApparentPacketType::UNKNOWN, Ssrc{0});
}

}  // namespace

std::pair<ApparentPacketType, Ssrc> InspectPacketForRouting(
    ByteView packet,
    RtpParserVersion version) {
#if defined(USE_RUST_RTP_PARSER)
  if (version == RtpParserVersion::kV2) {
    return InspectPacketForRoutingV2(packet);
  }
#endif  // defined(USE_RUST_RTP_PARSER)
  return InspectPacketForRoutingV1(packet);
}

}  // namespace openscreen::cast
