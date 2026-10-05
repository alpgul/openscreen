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

  std::atomic<bool> stats1_received{false};
  encoder.EncodeAndSend(
      frame, FakeClock::now(),
      [&stats1_received](const StreamingVideoEncoder::Stats& stats) {
        stats1_received.store(true);
      });

  std::atomic<bool> stats2_received{false};
  encoder.EncodeAndSend(
      frame, FakeClock::now() + std::chrono::milliseconds(33),
      [&stats2_received](const StreamingVideoEncoder::Stats& stats) {
        stats2_received.store(true);
      });

  WaitForStats(stats2_received);
  EXPECT_TRUE(stats1_received.load());
  EXPECT_TRUE(stats2_received.load());
  EXPECT_GT(fake_sender->enqueued_count(), 0);
}

}  // namespace openscreen::cast
