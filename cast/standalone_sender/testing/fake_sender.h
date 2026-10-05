// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_TESTING_FAKE_SENDER_H_
#define CAST_STANDALONE_SENDER_TESTING_FAKE_SENDER_H_

#include <stddef.h>
#include <stdint.h>

#include <array>
#include <chrono>

#include "cast/streaming/public/encoded_frame.h"
#include "cast/streaming/public/frame_id.h"
#include "cast/streaming/public/sender.h"
#include "cast/streaming/public/session_config.h"
#include "cast/streaming/rtp_time.h"
#include "platform/api/time.h"

namespace openscreen::cast {

// A Sender for encoder tests. It accepts every frame and only counts them.
class FakeSender : public Sender {
 public:
  FakeSender()
      : config_(/*sender_ssrc=*/1,
                /*receiver_ssrc=*/2,
                /*rtp_timebase=*/90000,
                /*channels=*/1,
                /*target_playout_delay=*/std::chrono::milliseconds(400),
                /*aes_secret_key=*/std::array<uint8_t, 16>{},
                /*aes_iv_mask=*/std::array<uint8_t, 16>{}) {}

  const SessionConfig& config() const override { return config_; }
  void SetObserver(Observer* observer) override {}
  size_t GetInFlightFrameCount() const override { return 0; }
  Clock::duration GetInFlightMediaDuration(RtpTimeTicks) const override {
    return Clock::duration::zero();
  }
  Clock::duration GetMaxInFlightMediaDuration() const override {
    return std::chrono::milliseconds(400);
  }
  bool NeedsKeyFrame() const override { return false; }
  FrameId GetNextFrameId() const override { return next_frame_id_; }
  Clock::duration GetCurrentRoundTripTime() const override {
    return Clock::duration::zero();
  }
  EnqueueFrameResult EnqueueFrame(const EncodedFrame& frame) override {
    enqueued_count_++;
    next_frame_id_++;
    return OK;
  }
  void CancelInFlightData() override {}
  void ReportFrameDropEvent(FrameId, RtpTimeTicks, Clock::time_point) override {
  }

  int enqueued_count() const { return enqueued_count_; }

 private:
  SessionConfig config_;
  FrameId next_frame_id_ = FrameId::first();
  int enqueued_count_ = 0;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_TESTING_FAKE_SENDER_H_
