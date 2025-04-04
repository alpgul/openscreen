// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/sender_report_parser.h"

#include "cast/streaming/impl/packet_util.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

SenderReportParser::SenderReportWithId::SenderReportWithId() = default;
SenderReportParser::SenderReportWithId::~SenderReportWithId() = default;

SenderReportParser::SenderReportParser(RtcpSession& session)
    : session_(session) {}

SenderReportParser::~SenderReportParser() = default;

std::optional<SenderReportParser::SenderReportWithId> SenderReportParser::Parse(
    ByteView buffer) {
  std::optional<SenderReportWithId> sender_report;

  // The data contained in `buffer` can be a "compound packet," which means that
  // it can be the concatenation of multiple RTCP packets. The loop here
  // processes each one-by-one.
  while (!buffer.empty()) {
    const auto header = RtcpCommonHeader::Parse(buffer);
    if (!header) {
      return std::nullopt;
    }
    buffer.remove_prefix(kRtcpCommonHeaderSize);
    if (static_cast<int>(buffer.size()) < header->payload_size) {
      return std::nullopt;
    }
    auto chunk = buffer.subspan(0, header->payload_size);
    buffer.remove_prefix(header->payload_size);

    // Only process Sender Reports with a matching SSRC.
    if (header->packet_type != RtcpPacketType::kSenderReport) {
      continue;
    }
    if (header->payload_size < kRtcpSenderReportSize) {
      return std::nullopt;
    }
    if (ConsumeField<uint32_t>(chunk) != session_.sender_ssrc()) {
      continue;
    }
    SenderReportWithId& report = sender_report.emplace();
    const NtpTimestamp ntp_timestamp = ConsumeField<uint64_t>(chunk);
    report.report_id = ToStatusReportId(ntp_timestamp);
    report.reference_time = session_.ntp_converter().ToLocalTime(ntp_timestamp);
    report.rtp_timestamp =
        last_parsed_rtp_timestamp_.Expand(ConsumeField<uint32_t>(chunk));
    report.send_packet_count = ConsumeField<uint32_t>(chunk);
    report.send_octet_count = ConsumeField<uint32_t>(chunk);
    report.report_block = RtcpReportBlock::ParseOne(
        chunk, header->with.report_count, session_.receiver_ssrc());
  }

  // At this point, the packet is known to be well-formed. Cache the
  // most-recently parsed RTP timestamp value for bit-expansion in future
  // parses.
  if (sender_report) {
    last_parsed_rtp_timestamp_ = sender_report->rtp_timestamp;
  }
  return sender_report;
}

}  // namespace openscreen::cast
