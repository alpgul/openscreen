// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/looping_file_sender.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "cast/standalone_common/ffmpeg_glue.h"
#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/standalone_sender/testing/fake_sender.h"
#include "cast/streaming/public/environment.h"
#include "cast/streaming/public/sender.h"
#include "cast/streaming/resolution.h"
#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/base/ip_address.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "util/osp_logging.h"

namespace openscreen::cast {
namespace {

// Captures what LoopingFileSender hands to the encoder. The plane pointers in
// StreamingVideoEncoder::VideoFrame are only valid for the duration of the
// EncodeAndSend() call, so the pixel data is copied into owned buffers and only
// the plane addresses are retained (for zero-copy identity assertions).
class FakeVideoEncoder : public StreamingVideoEncoder {
 public:
  FakeVideoEncoder(TaskRunner& task_runner, std::unique_ptr<Sender> sender)
      : StreamingVideoEncoder(Parameters{}, task_runner, std::move(sender)) {}

  int GetTargetBitrate() const override { return target_bitrate_; }
  void SetTargetBitrate(int new_bitrate) override {
    target_bitrate_ = new_bitrate;
  }

  void EncodeAndSend(const VideoFrame& frame,
                     Clock::time_point reference_time,
                     std::function<void(Stats)> stats_callback) override {
    last_width_ = frame.width;
    last_height_ = frame.height;
    planes_.width = frame.width;
    planes_.height = frame.height;
    const size_t uv_height = (frame.height + 1) / 2;
    CopyPlane(frame.yuv_planes[0], frame.yuv_strides[0], frame.height, last_y_,
              planes_.y);
    CopyPlane(frame.yuv_planes[1], frame.yuv_strides[1], uv_height, last_u_,
              planes_.u);
    CopyPlane(frame.yuv_planes[2], frame.yuv_strides[2], uv_height, last_v_,
              planes_.v);
    for (int i = 0; i < 3; ++i) {
      last_plane_addrs_[i] = frame.yuv_planes[i];
    }
    ++frames_encoded_;
  }

  int last_width() const { return last_width_; }
  int last_height() const { return last_height_; }
  const YuvPlanes& last_planes() const { return planes_; }
  const void* const* last_plane_addrs() const { return last_plane_addrs_; }
  int frames_encoded() const { return frames_encoded_; }

 private:
  static void CopyPlane(const uint8_t* source,
                        int stride,
                        size_t rows,
                        std::vector<uint8_t>& storage,
                        Plane& plane) {
    storage.assign(source, source + static_cast<size_t>(stride) * rows);
    plane = Plane{storage, stride};
  }

  int target_bitrate_ = 2000000;
  int last_width_ = 0;
  int last_height_ = 0;
  const void* last_plane_addrs_[3] = {};
  std::vector<uint8_t> last_y_;
  std::vector<uint8_t> last_u_;
  std::vector<uint8_t> last_v_;
  YuvPlanes planes_;
  int frames_encoded_ = 0;
};

AVFrameUniquePtr CreateSyntheticYuvFrame(int width, int height, uint8_t y_val) {
  AVFrameUniquePtr frame = MakeUniqueAVFrame();
  frame->format = AV_PIX_FMT_YUV420P;
  frame->width = width;
  frame->height = height;
  // An alignment of 0 lets FFmpeg pick the best one for this CPU.
  const int ret = av_frame_get_buffer(frame.get(), 0);
  OSP_CHECK_EQ(ret, 0);

  // Fills whole rows, including the stride padding, which is never read.
  const int uv_height = (height + 1) / 2;
  std::fill_n(frame->data[0], frame->linesize[0] * height, y_val);
  std::fill_n(frame->data[1], frame->linesize[1] * uv_height, 90);
  std::fill_n(frame->data[2], frame->linesize[2] * uv_height, 90);
  return frame;
}

TEST(GetMaybeDownscaledResolutionTest, ReturnsSourceThatFits) {
  EXPECT_EQ(GetMaybeDownscaledResolution({1280, 720}, {1920, 1080}),
            (Resolution{1280, 720}));
  EXPECT_EQ(GetMaybeDownscaledResolution({1920, 1080}, {1920, 1080}),
            (Resolution{1920, 1080}));
  EXPECT_EQ(GetMaybeDownscaledResolution({1921, 1081}, {3840, 2160}),
            (Resolution{1921, 1081}));
}

TEST(GetMaybeDownscaledResolutionTest, KeepsAspectRatio) {
  EXPECT_EQ(GetMaybeDownscaledResolution({3840, 2160}, {1920, 1080}),
            (Resolution{1920, 1080}));
  // Only the height is too large, but both sides are scaled by 1080 / 1440.
  EXPECT_EQ(GetMaybeDownscaledResolution({1280, 1440}, {1920, 1080}),
            (Resolution{960, 1080}));
  // 1080 * (1080 / 1920) = 607.5, which rounds to 608.
  EXPECT_EQ(GetMaybeDownscaledResolution({1080, 1920}, {1920, 1080}),
            (Resolution{608, 1080}));
}

TEST(GetMaybeDownscaledResolutionTest, ReturnsEvenSizesOfAtLeastTwo) {
  // 1921x1081 scales to 65x37, which becomes 64x36.
  EXPECT_EQ(GetMaybeDownscaledResolution({1921, 1081}, {65, 65}),
            (Resolution{64, 36}));
  // 4000x10 scales to 100x0.25, and the height is kept at two pixels.
  EXPECT_EQ(GetMaybeDownscaledResolution({4000, 10}, {100, 100}),
            (Resolution{100, 2}));
}

TEST(GetVisiblePlanesTest, UncroppedFrameUsesWholePlanes) {
  AVFrameUniquePtr frame = CreateSyntheticYuvFrame(64, 48, 120);

  const YuvPlanes planes = GetVisiblePlanes(*frame);

  EXPECT_EQ(planes.width, 64);
  EXPECT_EQ(planes.height, 48);
  // The planes are views into the frame's own buffers.
  EXPECT_EQ(planes.y.data.data(), frame->data[0]);
  EXPECT_EQ(planes.u.data.data(), frame->data[1]);
  EXPECT_EQ(planes.v.data.data(), frame->data[2]);
  EXPECT_EQ(planes.y.stride, frame->linesize[0]);
  EXPECT_EQ(planes.u.stride, frame->linesize[1]);
  EXPECT_EQ(planes.v.stride, frame->linesize[2]);
  // Each span ends at the last pixel of the image, not at the end of the
  // stride padding.
  EXPECT_EQ(planes.y.data.size(), frame->linesize[0] * size_t{47} + 64);
  EXPECT_EQ(planes.u.data.size(), frame->linesize[1] * size_t{23} + 32);
  EXPECT_EQ(planes.v.data.size(), frame->linesize[2] * size_t{23} + 32);
}

TEST(GetVisiblePlanesTest, SkipsCroppedArea) {
  AVFrameUniquePtr frame = CreateSyntheticYuvFrame(64, 48, 120);
  frame->crop_left = 4;
  frame->crop_right = 6;
  frame->crop_top = 2;
  frame->crop_bottom = 8;

  const YuvPlanes planes = GetVisiblePlanes(*frame);

  // The visible area is 54x38, so the chroma planes are 27x19.
  EXPECT_EQ(planes.width, 54);
  EXPECT_EQ(planes.height, 38);
  const size_t y_stride = frame->linesize[0];
  const size_t u_stride = frame->linesize[1];
  const size_t v_stride = frame->linesize[2];
  const std::span<uint8_t> y(frame->data[0], y_stride * 48);
  const std::span<uint8_t> u(frame->data[1], u_stride * 24);
  const std::span<uint8_t> v(frame->data[2], v_stride * 24);
  // Luma starts 2 rows down and 4 pixels in, chroma 1 row down and 2 in.
  EXPECT_EQ(planes.y.data.data(), y.subspan(2 * y_stride + 4).data());
  EXPECT_EQ(planes.u.data.data(), u.subspan(1 * u_stride + 2).data());
  EXPECT_EQ(planes.v.data.data(), v.subspan(1 * v_stride + 2).data());
  EXPECT_EQ(planes.y.data.size(), y_stride * 37 + 54);
  EXPECT_EQ(planes.u.data.size(), u_stride * 18 + 27);
  EXPECT_EQ(planes.v.data.size(), v_stride * 18 + 27);
}

class LoopingFileSenderTest : public ::testing::Test {
 protected:
  LoopingFileSenderTest()
      : clock_(Clock::now()),
        task_runner_(clock_),
        env_(&FakeClock::now,
             task_runner_,
             IPEndpoint{IPAddress::kAnyV4(), 0}) {}

  std::unique_ptr<LoopingFileSender> CreateSenderWithTargetResolution(
      Resolution target_res,
      FakeVideoEncoder** out_encoder) {
    ConnectionSettings settings;
    settings.path_to_file = "";
    settings.should_include_audio = false;
    settings.codec = VideoCodec::kVp8;
    settings.max_bitrate = 5000000;

    SenderSession::ConfiguredSenders senders;
    senders.video_sender = std::make_unique<FakeSender>();
    senders.video_config.codec = VideoCodec::kVp8;
    senders.video_config.resolutions = {target_res};

    auto fake_encoder = std::make_unique<FakeVideoEncoder>(
        task_runner_, std::make_unique<FakeSender>());
    *out_encoder = fake_encoder.get();
    return std::make_unique<LoopingFileSender>(
        env_, settings, /*session=*/nullptr, std::move(senders),
        /*shutdown_callback=*/[] {}, std::move(fake_encoder));
  }

  FakeClock clock_;
  FakeTaskRunner task_runner_;
  Environment env_;
};

TEST_F(LoopingFileSenderTest, UnscaledPassThroughUsesZeroCopy) {
  FakeVideoEncoder* fake_encoder = nullptr;
  auto sender = CreateSenderWithTargetResolution({1920, 1080}, &fake_encoder);

  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(1920, 1080, 120);
  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  EXPECT_EQ(fake_encoder->frames_encoded(), 1);
  EXPECT_EQ(fake_encoder->last_width(), 1920);
  EXPECT_EQ(fake_encoder->last_height(), 1080);
  // In the unscaled fast-path, the Y plane address is the original AVFrame's
  // data buffer: no scaled buffer was allocated.
  EXPECT_EQ(fake_encoder->last_plane_addrs()[0], src_frame->data[0]);
}

TEST_F(LoopingFileSenderTest, SourceSmallerThanNegotiatedIsNotUpscaled) {
  FakeVideoEncoder* fake_encoder = nullptr;
  auto sender = CreateSenderWithTargetResolution({3840, 2160}, &fake_encoder);

  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(1920, 1080, 120);
  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  // The negotiated resolution is an upper bound, so a 1080p source is sent at
  // its native resolution rather than being upscaled to 4K.
  EXPECT_EQ(fake_encoder->last_width(), 1920);
  EXPECT_EQ(fake_encoder->last_height(), 1080);
  EXPECT_EQ(fake_encoder->last_plane_addrs()[0], src_frame->data[0]);
}

TEST_F(LoopingFileSenderTest,
       DynamicScalingDownscales4kTo1080pWith32ByteAlignment) {
  FakeVideoEncoder* fake_encoder = nullptr;
  auto sender = CreateSenderWithTargetResolution({1920, 1080}, &fake_encoder);

  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(3840, 2160, 180);
  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  EXPECT_EQ(fake_encoder->frames_encoded(), 1);
  EXPECT_EQ(fake_encoder->last_width(), 1920);
  EXPECT_EQ(fake_encoder->last_height(), 1080);
  // In the scaling path, the Y plane points at the scaled buffer rather than
  // at the source AVFrame.
  EXPECT_NE(fake_encoder->last_plane_addrs()[0], src_frame->data[0]);
  // Verify 32-byte SIMD stride alignment on Y, U, and V planes.
  const auto& planes = fake_encoder->last_planes();
  EXPECT_EQ(planes.y.stride % 32, 0);
  EXPECT_EQ(planes.u.stride % 32, 0);
  EXPECT_EQ(planes.v.stride % 32, 0);
}

TEST_F(LoopingFileSenderTest,
       DynamicScalingDownscales1080pTo720pAndDrawsClickAnimation) {
  FakeVideoEncoder* fake_encoder = nullptr;
  auto sender = CreateSenderWithTargetResolution({1280, 720}, &fake_encoder);

  // Inject a mouse click event at normalized center (0.5, 0.5).
  InputMessage click_message;
  auto* event = click_message.add_events();
  event->set_type(InputMessage::INPUT_TYPE_MOUSE_DOWN);
  event->mutable_mouse_event()->mutable_location()->set_x(0.5f);
  event->mutable_mouse_event()->mutable_location()->set_y(0.5f);
  sender->OnInputMessage(click_message);

  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(1920, 1080, 50);
  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  EXPECT_EQ(fake_encoder->frames_encoded(), 1);
  EXPECT_EQ(fake_encoder->last_width(), 1280);
  EXPECT_EQ(fake_encoder->last_height(), 720);

  // DrawAnimations() draws white pixels (Y=255, U=128, V=128) on the scaled
  // buffer along the click ring, whose center is (640, 360) with an initial
  // radius of about five pixels.
  const auto& planes = fake_encoder->last_planes();
  constexpr int kPy = 360;
  bool found_white_pixel = false;
  for (int dx = 2; dx <= 6; ++dx) {
    const int px = 640 + dx;
    if (planes.y.data[kPy * planes.y.stride + px] == 255 && px % 2 == 0) {
      EXPECT_EQ(planes.u.data[(kPy / 2) * planes.u.stride + (px / 2)], 128);
      EXPECT_EQ(planes.v.data[(kPy / 2) * planes.v.stride + (px / 2)], 128);
      found_white_pixel = true;
      break;
    }
  }
  EXPECT_TRUE(found_white_pixel);
}

TEST_F(LoopingFileSenderTest, DownscaleKeepsSourceAspectRatio) {
  FakeVideoEncoder* fake_encoder = nullptr;
  auto sender = CreateSenderWithTargetResolution({1920, 1080}, &fake_encoder);

  // A 1080x1920 portrait source is taller than the 1920x1080 target, so both
  // sides are scaled by 1080 / 1920 = 0.5625: 1080 becomes 607.5, which rounds
  // to 608. The frame must not be stretched to 1920x1080.
  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(1080, 1920, 120);
  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  EXPECT_EQ(fake_encoder->frames_encoded(), 1);
  EXPECT_EQ(fake_encoder->last_width(), 608);
  EXPECT_EQ(fake_encoder->last_height(), 1080);
}

TEST_F(LoopingFileSenderTest, DownscaleReadsOnlyTheVisibleArea) {
  FakeVideoEncoder* fake_encoder = nullptr;
  auto sender = CreateSenderWithTargetResolution({32, 18}, &fake_encoder);

  // An 80x60 black frame whose visible 64x48 area, inside an 8 pixel wide
  // cropped border on the left and right and a 6 pixel one on the top and
  // bottom, is light gray.
  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(80, 60, 0);
  src_frame->crop_left = 8;
  src_frame->crop_right = 8;
  src_frame->crop_top = 6;
  src_frame->crop_bottom = 6;
  const size_t stride = src_frame->linesize[0];
  const std::span<uint8_t> y(src_frame->data[0], stride * 60);
  for (size_t row = 6; row < 54; ++row) {
    std::ranges::fill(y.subspan(row * stride + 8, 64), 200);
  }

  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  // 64x48 fits into 32x18 at 24x18. If the cropped border had been read, the
  // edges of the scaled image would be darker.
  ASSERT_EQ(fake_encoder->last_width(), 24);
  ASSERT_EQ(fake_encoder->last_height(), 18);
  const Plane& scaled_y = fake_encoder->last_planes().y;
  for (int row = 0; row < 18; ++row) {
    for (int col = 0; col < 24; ++col) {
      EXPECT_NEAR(scaled_y.data[row * scaled_y.stride + col], 200, 2)
          << "at (" << col << ", " << row << ")";
    }
  }
}

TEST_F(LoopingFileSenderTest, OddDimensionsAreMaskedToEvenWithoutOverflow) {
  FakeVideoEncoder* fake_encoder = nullptr;
  // Target resolution with odd dimensions where width % 64 == 1 (e.g. 65x65).
  auto sender = CreateSenderWithTargetResolution({65, 65}, &fake_encoder);

  // Keeping the aspect ratio scales 1921x1081 to 65x37 (both odd), which is
  // then masked to 64x36.
  AVFrameUniquePtr src_frame = CreateSyntheticYuvFrame(1921, 1081, 100);
  sender->OnVideoFrame(*src_frame, clock_.now(), clock_.now(), clock_.now());

  EXPECT_EQ(fake_encoder->frames_encoded(), 1);
  EXPECT_EQ(fake_encoder->last_width(), 64);
  EXPECT_EQ(fake_encoder->last_height(), 36);
  const auto& planes = fake_encoder->last_planes();
  EXPECT_EQ(planes.width, 64);
  EXPECT_EQ(planes.height, 36);
  EXPECT_EQ(planes.y.stride % 32, 0);
  EXPECT_EQ(planes.u.stride % 32, 0);
  EXPECT_EQ(planes.v.stride % 32, 0);
}

}  // namespace
}  // namespace openscreen::cast
