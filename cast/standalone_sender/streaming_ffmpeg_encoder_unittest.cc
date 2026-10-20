// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/streaming_ffmpeg_encoder.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "cast/standalone_sender/testing/fake_sender.h"
#include "cast/streaming/public/constants.h"
#include "gtest/gtest.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"

namespace openscreen::cast {

class StreamingFfmpegEncoderTest : public ::testing::Test {
 protected:
  StreamingFfmpegEncoderTest() : clock_(Clock::now()), task_runner_(clock_) {}

  void WaitForStats(std::atomic<bool>& received) {
    for (int i = 0; i < 50; ++i) {
      task_runner_.RunTasksUntilIdle();
      if (received.load()) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    task_runner_.RunTasksUntilIdle();
  }

  FakeClock clock_;
  FakeTaskRunner task_runner_;
};

TEST_F(StreamingFfmpegEncoderTest, InstantiationAndBitrateH264) {
  StreamingVideoEncoder::Parameters params;
  params.codec = VideoCodec::kH264;
  params.num_encode_threads = 2;

  auto sender = std::make_unique<FakeSender>();
  StreamingFfmpegEncoder encoder(params, task_runner_, std::move(sender));

  EXPECT_EQ(encoder.GetTargetBitrate(), 5000000);
  encoder.SetTargetBitrate(8000000);
  EXPECT_EQ(encoder.GetTargetBitrate(), 8000000);
}

TEST_F(StreamingFfmpegEncoderTest, InstantiationAndBitrateHevc) {
  StreamingVideoEncoder::Parameters params;
  params.codec = VideoCodec::kHevc;
  params.num_encode_threads = 2;

  auto sender = std::make_unique<FakeSender>();
  StreamingFfmpegEncoder encoder(params, task_runner_, std::move(sender));

  EXPECT_EQ(encoder.GetTargetBitrate(), 5000000);
  encoder.SetTargetBitrate(6000000);
  EXPECT_EQ(encoder.GetTargetBitrate(), 6000000);
}

TEST_F(StreamingFfmpegEncoderTest, EncodeAndSendH264Frame) {
  StreamingVideoEncoder::Parameters params;
  params.codec = VideoCodec::kH264;
  params.num_encode_threads = 2;

  auto sender = std::make_unique<FakeSender>();
  auto* fake_sender = sender.get();
  StreamingFfmpegEncoder encoder(params, task_runner_, std::move(sender));

  StreamingVideoEncoder::VideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.duration = std::chrono::milliseconds(33);

  std::vector<uint8_t> y_plane(frame.width * frame.height, 128);
  std::vector<uint8_t> u_plane((frame.width / 2) * (frame.height / 2), 64);
  std::vector<uint8_t> v_plane((frame.width / 2) * (frame.height / 2), 64);

  frame.yuv_planes[0] = y_plane.data();
  frame.yuv_planes[1] = u_plane.data();
  frame.yuv_planes[2] = v_plane.data();

  frame.yuv_strides[0] = frame.width;
  frame.yuv_strides[1] = frame.width / 2;
  frame.yuv_strides[2] = frame.width / 2;

  std::atomic<bool> stats_received{false};
  encoder.EncodeAndSend(
      frame, FakeClock::now(),
      [&stats_received](const StreamingVideoEncoder::Stats& stats) {
        stats_received.store(true);
      });

  WaitForStats(stats_received);
  EXPECT_TRUE(stats_received.load());
  EXPECT_GT(fake_sender->enqueued_count(), 0);
}

TEST_F(StreamingFfmpegEncoderTest, EncodeAndSendHevcFrame) {
  StreamingVideoEncoder::Parameters params;
  params.codec = VideoCodec::kHevc;
  params.num_encode_threads = 2;

  auto sender = std::make_unique<FakeSender>();
  auto* fake_sender = sender.get();
  StreamingFfmpegEncoder encoder(params, task_runner_, std::move(sender));

  StreamingVideoEncoder::VideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.duration = std::chrono::milliseconds(33);

  std::vector<uint8_t> y_plane(frame.width * frame.height, 128);
  std::vector<uint8_t> u_plane((frame.width / 2) * (frame.height / 2), 64);
  std::vector<uint8_t> v_plane((frame.width / 2) * (frame.height / 2), 64);

  frame.yuv_planes[0] = y_plane.data();
  frame.yuv_planes[1] = u_plane.data();
  frame.yuv_planes[2] = v_plane.data();

  frame.yuv_strides[0] = frame.width;
  frame.yuv_strides[1] = frame.width / 2;
  frame.yuv_strides[2] = frame.width / 2;

  double stats1_target_size = 0.0;
  std::atomic<bool> stats1_received{false};
  encoder.EncodeAndSend(frame, FakeClock::now(),
                        [&](const StreamingVideoEncoder::Stats& stats) {
                          stats1_target_size = stats.target_size;
                          stats1_received.store(true);
                        });
  WaitForStats(stats1_received);
  EXPECT_TRUE(stats1_received.load());

  // A minor bitrate change (< 2x from 5 Mbps to 3.5 Mbps) should not tear down
  // the encoder or force a keyframe on the next frame, and stats should reflect
  // the encoder's active 5 Mbps bitrate.
  encoder.SetTargetBitrate(3500000);

  double stats2_target_size = 0.0;
  std::atomic<bool> stats2_received{false};
  encoder.EncodeAndSend(frame, FakeClock::now() + std::chrono::milliseconds(33),
                        [&](const StreamingVideoEncoder::Stats& stats) {
                          stats2_target_size = stats.target_size;
                          stats2_received.store(true);
                        });
  WaitForStats(stats2_received);
  EXPECT_TRUE(stats2_received.load());

  // A cumulative bitrate change of >= 2x downward from the open encoder's
  // 5 Mbps rate (down to 2 Mbps) should recreate the encoder and emit a
  // keyframe.
  encoder.SetTargetBitrate(2000000);

  double stats3_target_size = 0.0;
  std::atomic<bool> stats3_received{false};
  encoder.EncodeAndSend(frame, FakeClock::now() + std::chrono::milliseconds(66),
                        [&](const StreamingVideoEncoder::Stats& stats) {
                          stats3_target_size = stats.target_size;
                          stats3_received.store(true);
                        });
  WaitForStats(stats3_received);
  EXPECT_TRUE(stats3_received.load());

  std::atomic<bool> stats4_received{false};
  encoder.EncodeAndSend(
      frame, FakeClock::now() + std::chrono::milliseconds(99),
      [&stats4_received](const StreamingVideoEncoder::Stats& stats) {
        stats4_received.store(true);
      });
  WaitForStats(stats4_received);
  EXPECT_TRUE(stats4_received.load());

  // A >= 2x upward step (from 2 Mbps up to 4 Mbps) should also recreate the
  // encoder and emit a keyframe.
  encoder.SetTargetBitrate(4000000);

  double stats5_target_size = 0.0;
  std::atomic<bool> stats5_received{false};
  encoder.EncodeAndSend(frame,
                        FakeClock::now() + std::chrono::milliseconds(132),
                        [&](const StreamingVideoEncoder::Stats& stats) {
                          stats5_target_size = stats.target_size;
                          stats5_received.store(true);
                        });
  WaitForStats(stats5_received);
  EXPECT_TRUE(stats5_received.load());

  ASSERT_EQ(fake_sender->enqueued_count(), 5);
  EXPECT_EQ(fake_sender->dependencies()[0],
            EncodedFrame::Dependency::kKeyFrame);
  EXPECT_EQ(fake_sender->dependencies()[1],
            EncodedFrame::Dependency::kDependent);
  EXPECT_EQ(fake_sender->dependencies()[2],
            EncodedFrame::Dependency::kKeyFrame);
  EXPECT_EQ(fake_sender->dependencies()[3],
            EncodedFrame::Dependency::kDependent);
  EXPECT_EQ(fake_sender->dependencies()[4],
            EncodedFrame::Dependency::kKeyFrame);

  // Frame 1 uses `kMaxFrameDuration` (125 ms) for its predicted duration when
  // `reference_time == start_time_`, but Frame 2 and later use `frame.duration`
  // (33 ms). Check that Frame 2 still reports the active 5 Mbps target size
  // rather than the unapplied 3.5 Mbps target size.
  constexpr double kFrameDurationSeconds = 0.033;
  EXPECT_NEAR(stats2_target_size, (5000000.0 * kFrameDurationSeconds) / 8.0,
              1.0);
  EXPECT_NEAR(stats3_target_size, (2000000.0 * kFrameDurationSeconds) / 8.0,
              1.0);
  EXPECT_NEAR(stats5_target_size, (4000000.0 * kFrameDurationSeconds) / 8.0,
              1.0);
}

TEST_F(StreamingFfmpegEncoderTest, ShouldReinitHevcEncoderBoundaries) {
  // Immediate >= 2x changes reinitialize on the very first frame.
  EXPECT_TRUE(ShouldReinitHevcEncoder(2000000, 4000000, 1));
  EXPECT_TRUE(ShouldReinitHevcEncoder(5000000, 2500000, 1));
  EXPECT_FALSE(ShouldReinitHevcEncoder(2000000, 3999999, 1));
  EXPECT_FALSE(ShouldReinitHevcEncoder(5000000, 2500001, 1));

  // Sub-2x changes wait until `kHevcBitrateNumFramesBeforeSettling` (60
  // frames).
  EXPECT_FALSE(ShouldReinitHevcEncoder(
      3500000, 5000000, kHevcBitrateNumFramesBeforeSettling - 1));
  EXPECT_TRUE(ShouldReinitHevcEncoder(3500000, 5000000,
                                      kHevcBitrateNumFramesBeforeSettling));

  // Once settled for 60 frames, only changes >= 20% trigger a reinit.
  EXPECT_FALSE(ShouldReinitHevcEncoder(5000000, 5999999,
                                       kHevcBitrateNumFramesBeforeSettling));
  EXPECT_TRUE(ShouldReinitHevcEncoder(5000000, 6000000,
                                      kHevcBitrateNumFramesBeforeSettling));
  EXPECT_FALSE(ShouldReinitHevcEncoder(5000000, 4000001,
                                       kHevcBitrateNumFramesBeforeSettling));
  EXPECT_TRUE(ShouldReinitHevcEncoder(5000000, 4000000,
                                      kHevcBitrateNumFramesBeforeSettling));
}

TEST_F(StreamingFfmpegEncoderTest, EncodeAndSendHevcSettleStep) {
  StreamingVideoEncoder::Parameters params;
  params.codec = VideoCodec::kHevc;
  params.num_encode_threads = 2;

  auto sender = std::make_unique<FakeSender>();
  auto* fake_sender = sender.get();
  StreamingFfmpegEncoder encoder(params, task_runner_, std::move(sender));
  encoder.SetTargetBitrate(3500000);

  StreamingVideoEncoder::VideoFrame frame;
  frame.width = 320;
  frame.height = 240;
  frame.duration = std::chrono::milliseconds(33);

  std::vector<uint8_t> y_plane(frame.width * frame.height, 128);
  std::vector<uint8_t> u_plane((frame.width / 2) * (frame.height / 2), 64);
  std::vector<uint8_t> v_plane((frame.width / 2) * (frame.height / 2), 64);
  frame.yuv_planes[0] = y_plane.data();
  frame.yuv_planes[1] = u_plane.data();
  frame.yuv_planes[2] = v_plane.data();
  frame.yuv_strides[0] = frame.width;
  frame.yuv_strides[1] = frame.width / 2;
  frame.yuv_strides[2] = frame.width / 2;

  auto encode_one = [&](int frame_index, double* out_target_size = nullptr) {
    std::atomic<bool> received{false};
    encoder.EncodeAndSend(
        frame, FakeClock::now() + std::chrono::milliseconds(33 * frame_index),
        [&](const StreamingVideoEncoder::Stats& stats) {
          if (out_target_size) {
            *out_target_size = stats.target_size;
          }
          received.store(true);
        });
    WaitForStats(received);
    EXPECT_TRUE(received.load());
  };

  // Frame 0 opens the encoder at 3.5 Mbps.
  encode_one(0);
  ASSERT_EQ(fake_sender->enqueued_count(), 1);
  EXPECT_EQ(fake_sender->dependencies()[0],
            EncodedFrame::Dependency::kKeyFrame);

  // Step up to 5 Mbps (+42.9%, which is < 2x but >= 20%).
  encoder.SetTargetBitrate(5000000);

  // The first 59 frames at 5 Mbps keep the 3.5 Mbps encoder instance.
  double pre_settle_target_size = 0.0;
  for (int i = 1; i < kHevcBitrateNumFramesBeforeSettling; ++i) {
    encode_one(i, &pre_settle_target_size);
  }
  ASSERT_EQ(fake_sender->enqueued_count(), kHevcBitrateNumFramesBeforeSettling);
  EXPECT_EQ(fake_sender->dependencies().back(),
            EncodedFrame::Dependency::kDependent);
  EXPECT_NEAR(pre_settle_target_size, (3500000.0 * 0.033) / 8.0, 1.0);

  // The 60th frame at 5 Mbps settles and recreates the encoder at 5 Mbps.
  double settled_target_size = 0.0;
  encode_one(kHevcBitrateNumFramesBeforeSettling, &settled_target_size);
  ASSERT_EQ(fake_sender->enqueued_count(),
            kHevcBitrateNumFramesBeforeSettling + 1);
  EXPECT_EQ(fake_sender->dependencies().back(),
            EncodedFrame::Dependency::kKeyFrame);
  EXPECT_NEAR(settled_target_size, (5000000.0 * 0.033) / 8.0, 1.0);

  // Subsequent frames stay at 5 Mbps without recreating the encoder again.
  encode_one(kHevcBitrateNumFramesBeforeSettling + 1);
  ASSERT_EQ(fake_sender->enqueued_count(),
            kHevcBitrateNumFramesBeforeSettling + 2);
  EXPECT_EQ(fake_sender->dependencies().back(),
            EncodedFrame::Dependency::kDependent);
}
}  // namespace openscreen::cast
