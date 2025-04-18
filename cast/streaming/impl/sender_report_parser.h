// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_SENDER_REPORT_PARSER_H_
#define CAST_STREAMING_IMPL_SENDER_REPORT_PARSER_H_

#include <optional>

#include "cast/streaming/impl/rtcp_common.h"
#include "cast/streaming/impl/rtcp_session.h"
#include "cast/streaming/impl/rtp_defines.h"
#include "cast/streaming/rtp_time.h"
#include "platform/base/span.h"

namespace openscreen::cast {

// Parses RTCP packets from a Sender to extract Sender Reports. Ignores anything
// else, since that is all a Receiver would be interested in.
class SenderReportParser {
 public:
  // Returned by Parse(), to also separately expose the StatusReportId. The
  // report ID isn't included in the common RtcpSenderReport struct because it's
  // not an input to SenderReportBuilder (it is generated by the builder).
  struct SenderReportWithId : public RtcpSenderReport {
    SenderReportWithId();
    ~SenderReportWithId();

    StatusReportId report_id{};
  };

  explicit SenderReportParser(RtcpSession& session);
  ~SenderReportParser();

  // Parses the RTCP `packet`, and returns a parsed sender report if the packet
  // contained one. Returns nullopt if the data is corrupt or the packet did not
  // contain a sender report.
  std::optional<SenderReportWithId> Parse(ByteView packet);

 private:
  RtcpSession& session_;

  // Tracks the recently-parsed RTP timestamps so that the truncated values can
  // be re-expanded into full-form.
  RtpTimeTicks last_parsed_rtp_timestamp_;
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_SENDER_REPORT_PARSER_H_
