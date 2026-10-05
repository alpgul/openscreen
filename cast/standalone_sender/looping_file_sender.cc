// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/standalone_sender/looping_file_sender.h"

#include <array>
#include <cmath>
#include <span>
#include <utility>

#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
#include "cast/standalone_sender/streaming_av1_encoder.h"
#endif
#if defined(CAST_STANDALONE_SENDER_HAVE_FFMPEG)
#include "cast/standalone_sender/streaming_ffmpeg_encoder.h"
#endif
#include "cast/standalone_sender/streaming_vpx_encoder.h"
#include "platform/base/trivial_clock_traits.h"
#include "util/osp_logging.h"
#include "util/trace_logging.h"

namespace openscreen::cast {

namespace {

// Wraps all rows of `frame` plane `index` in one span, then narrows it to the
// visible `width` x `height` rectangle, from its first pixel to its last one.
Plane GetVisiblePlane(const AVFrame& frame, int index, int width, int height) {
  // The chroma planes (1 and 2) are subsampled by two in both dimensions.
  const int shift = index == 0 ? 0 : 1;
  const int stride = frame.linesize[index];
  OSP_CHECK_GT(stride, 0);
  const size_t plane_rows = (frame.height + shift) >> shift;
  const std::span<uint8_t> plane(frame.data[index], stride * plane_rows);

  const size_t offset =
      stride * (frame.crop_top >> shift) + (frame.crop_left >> shift);
  const size_t visible_width = (width + shift) >> shift;
  const size_t visible_rows = (height + shift) >> shift;
  return Plane{
      plane.subspan(offset, stride * (visible_rows - 1) + visible_width),
      stride};
}

}  // namespace

// If `source` is larger than `target`, scales it down to fit while keeping the
// same aspect ratio. If `source` already fits, returns `source` as is.
Resolution GetMaybeDownscaledResolution(Resolution source, Resolution target) {
  // Do not upscale if the source already fits inside the target box.
  if (target.IsSupersetOf(source)) {
    return source;
  }

  // Use the smaller scale factor on both sides to keep the aspect ratio.
  const double scale =
      std::min(static_cast<double>(target.width) / source.width,
               static_cast<double>(target.height) / source.height);

  // Round to the nearest pixel, keep at least 2 pixels, and make the size even
  // (`& ~1`) because YUV420P requires even width and height.
  const int width =
      std::max(2, static_cast<int>(std::round(source.width * scale))) & ~1;
  const int height =
      std::max(2, static_cast<int>(std::round(source.height * scale))) & ~1;
  return {width, height};
}

YuvPlanes GetVisiblePlanes(const AVFrame& frame) {
  OSP_CHECK_EQ(frame.format, AV_PIX_FMT_YUV420P);
  const int width = frame.width - frame.crop_left - frame.crop_right;
  const int height = frame.height - frame.crop_top - frame.crop_bottom;
  OSP_CHECK_GT(width, 0);
  OSP_CHECK_GT(height, 0);

  return YuvPlanes{.width = width,
                   .height = height,
                   .y = GetVisiblePlane(frame, 0, width, height),
                   .u = GetVisiblePlane(frame, 1, width, height),
                   .v = GetVisiblePlane(frame, 2, width, height)};
}

LoopingFileSender::LoopingFileSender(
    Environment& environment,
    ConnectionSettings settings,
    const SenderSession* session,
    SenderSession::ConfiguredSenders senders,
    ShutdownCallback shutdown_callback,
    std::unique_ptr<StreamingVideoEncoder> video_encoder)
    : env_(environment),
      settings_(std::move(settings)),
      session_(session),
      shutdown_callback_(std::move(shutdown_callback)),
      video_encoder_(video_encoder ? std::move(video_encoder)
                                   : CreateVideoEncoder(
                                         StreamingVideoEncoder::Parameters{
                                             .codec = settings.codec},
                                         env_.task_runner(),
                                         std::move(senders.video_sender))),
      next_task_(env_.now_function(), env_.task_runner()),
      console_update_task_(env_.now_function(), env_.task_runner()) {
  if (settings_.should_include_audio) {
    OSP_CHECK(senders.audio_config.codec == AudioCodec::kOpus);
    audio_encoder_ = std::make_unique<StreamingOpusEncoder>(
        senders.audio_config.channels,
        StreamingOpusEncoder::kDefaultCastAudioFramesPerSecond,
        std::move(senders.audio_sender));
  }
  if (!senders.video_config.resolutions.empty()) {
    target_resolution_ = senders.video_config.resolutions[0];
  }
  OSP_CHECK(senders.video_config.codec == VideoCodec::kVp8 ||
            senders.video_config.codec == VideoCodec::kVp9 ||
            senders.video_config.codec == VideoCodec::kAv1 ||
            senders.video_config.codec == VideoCodec::kH264 ||
            senders.video_config.codec == VideoCodec::kHevc);
  OSP_LOG_INFO << "Max allowed media bitrate (audio + video) will be "
               << settings_.max_bitrate;
  bandwidth_being_utilized_ = settings_.max_bitrate / 2;
  UpdateEncoderBitrates();

  if (!settings_.path_to_file.empty()) {
    next_task_.Schedule([this] { SendFileAgain(); }, Alarm::kImmediately);
  }
}

LoopingFileSender::~LoopingFileSender() {
  if (sws_context_) {
    sws_freeContext(sws_context_);
    sws_context_ = nullptr;
  }
}

void LoopingFileSender::SetPlaybackRate(double rate) {
  if (video_capturer_) {
    video_capturer_->SetPlaybackRate(rate);
  }
  if (audio_capturer_) {
    audio_capturer_->SetPlaybackRate(rate);
  }
}

void LoopingFileSender::OnInputMessage(InputMessage message) {
  const auto now = env_.now();
  for (const auto& event : message.events()) {
    if (event.type() == InputMessage::INPUT_TYPE_MOUSE_DOWN &&
        event.has_mouse_event()) {
      active_clicks_.push_back(Click{event.mouse_event().location().x(),
                                     event.mouse_event().location().y(), now,
                                     now + std::chrono::milliseconds(500)});
    }
  }
}

void LoopingFileSender::UpdateEncoderBitrates() {
  int video_bitrate = bandwidth_being_utilized_;
  if (audio_encoder_) {
    if (bandwidth_being_utilized_ >= kHighBandwidthThreshold) {
      audio_encoder_->UseHighQuality();
    } else {
      audio_encoder_->UseStandardQuality();
    }
    video_bitrate -= audio_encoder_->GetBitrate();
  }
  TRACE_SCOPED2(TraceCategory::kStandaloneSender, "UpdateEncoderBitrates",
                "video_bitrate", video_bitrate, "utilized_bandwidth",
                bandwidth_being_utilized_);
  video_encoder_->SetTargetBitrate(video_bitrate);
}

void LoopingFileSender::ControlForNetworkCongestion() {
  bandwidth_estimate_ = session_->GetEstimatedNetworkBandwidth();
  if (bandwidth_estimate_ > 0) {
    // Don't ever try to use *all* of the network bandwidth! However, don't go
    // below the absolute minimum requirement either.
    constexpr double kGoodNetworkCitizenFactor = 0.8;
    const int usable_bandwidth = std::max<int>(
        kGoodNetworkCitizenFactor * bandwidth_estimate_, kMinRequiredBitrate);

    // See "congestion control" discussion in the class header comments for
    // BandwidthEstimator.
    if (usable_bandwidth > bandwidth_being_utilized_) {
      constexpr double kConservativeIncrease = 1.1;
      bandwidth_being_utilized_ = std::min<int>(
          bandwidth_being_utilized_ * kConservativeIncrease, usable_bandwidth);
    } else {
      bandwidth_being_utilized_ = usable_bandwidth;
    }

    // Repsect the user's maximum bitrate setting.
    bandwidth_being_utilized_ =
        std::min(bandwidth_being_utilized_, settings_.max_bitrate);

    UpdateEncoderBitrates();
  } else {
    // There is no current bandwidth estimate. So, nothing should be adjusted.
  }

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
}

void LoopingFileSender::SendFileAgain() {
  OSP_LOG_INFO << "Sending " << settings_.path_to_file
               << " (starts in one second)...";
  TRACE_DEFAULT_SCOPED(TraceCategory::kStandaloneSender);

  OSP_CHECK_EQ(num_capturers_running_, 0);
  num_capturers_running_ = settings_.should_include_audio ? 2 : 1;
  capture_begin_time_ = latest_frame_time_ = env_.now() + seconds(1);
  if (settings_.should_include_audio) {
    audio_capturer_ = std::make_unique<SimulatedAudioCapturer>(
        env_, settings_.path_to_file.c_str(), audio_encoder_->num_channels(),
        audio_encoder_->sample_rate(), capture_begin_time_, *this);
  }
  video_capturer_ = std::make_unique<SimulatedVideoCapturer>(
      env_, settings_.path_to_file.c_str(), capture_begin_time_, *this);

  next_task_.ScheduleFromNow([this] { ControlForNetworkCongestion(); },
                             kCongestionCheckInterval);
  console_update_task_.Schedule([this] { UpdateStatusOnConsole(); },
                                capture_begin_time_);
}

void LoopingFileSender::OnAudioData(const float* interleaved_samples,
                                    int num_samples,
                                    Clock::time_point capture_begin_time,
                                    Clock::time_point capture_end_time,
                                    Clock::time_point reference_time) {
  TRACE_SCOPED2(TraceCategory::kStandaloneSender, "OnAudioData", "num_samples",
                std::to_string(num_samples), "reference_time",
                ToString(reference_time));
  latest_frame_time_ = std::max(reference_time, latest_frame_time_);
  if (audio_encoder_) {
    audio_encoder_->EncodeAndSend(interleaved_samples, num_samples,
                                  capture_begin_time, capture_end_time,
                                  reference_time);
  }
}

void LoopingFileSender::OnVideoFrame(const AVFrame& av_frame,
                                     Clock::time_point capture_begin_time,
                                     Clock::time_point capture_end_time,
                                     Clock::time_point reference_time) {
  TRACE_SCOPED1(TraceCategory::kStandaloneSender, "OnVideoFrame",
                "reference_time", ToString(reference_time));
  latest_frame_time_ = std::max(reference_time, latest_frame_time_);

  YuvPlanes planes = GetVisiblePlanes(av_frame);
  if (target_resolution_) {
    const Resolution source_size{planes.width, planes.height};
    const Resolution dest_size =
        GetMaybeDownscaledResolution(source_size, *target_resolution_);
    if (dest_size != source_size) {
      planes = Downscale(planes, dest_size);
    }
  }
  // YUV420P subsamples chroma 2x2, and some encoders reject odd sizes, so drop
  // the last column or row of an odd-sized frame.
  planes.width &= ~1;
  planes.height &= ~1;

  DrawAnimations(planes);

  StreamingVideoEncoder::VideoFrame frame{};
  frame.width = planes.width;
  frame.height = planes.height;
  frame.yuv_planes[0] = planes.y.data.data();
  frame.yuv_planes[1] = planes.u.data.data();
  frame.yuv_planes[2] = planes.v.data.data();
  frame.yuv_strides[0] = planes.y.stride;
  frame.yuv_strides[1] = planes.u.stride;
  frame.yuv_strides[2] = planes.v.stride;
  frame.capture_begin_time = capture_begin_time;
  frame.capture_end_time = capture_end_time;

  // TODO(jophba): Add performance metrics visual overlay (based on Stats
  // callback).
  video_encoder_->EncodeAndSend(frame, reference_time, {});
}

void LoopingFileSender::UpdateStatusOnConsole() {
  const Clock::duration elapsed = latest_frame_time_ - capture_begin_time_;
  // The control codes here attempt to erase the current line the cursor is
  // on, and then print out the updated status text. If the terminal does not
  // support simple ANSI escape codes, the following will still work, but
  // there might sometimes be old status lines not getting erased (i.e., just
  // partially overwritten).
  OSP_VLOG << "LoopingFileSender: At " << elapsed
           << " in file (est. network bandwidth: " << bandwidth_estimate_ / 1024
           << ").";
  console_update_task_.ScheduleFromNow([this] { UpdateStatusOnConsole(); },
                                       kConsoleUpdateInterval);
}

void LoopingFileSender::DrawAnimations(YuvPlanes& planes) {
  const auto now = env_.now();
  // Remove clicks that have exceeded their 500ms display duration.
  std::erase_if(active_clicks_,
                [now](const Click& c) { return c.end_time < now; });

  if (active_clicks_.empty()) {
    return;
  }

  const int frame_width = planes.width;
  const int frame_height = planes.height;

  for (const auto& click : active_clicks_) {
    // Map the click coordinates from the receiver's logical display space
    // to the sender's actual video frame resolution.
    const float scale_x = static_cast<float>(frame_width);
    const float scale_y = static_cast<float>(frame_height);
    const int center_x = static_cast<int>(std::round(click.x * scale_x));
    const int center_y = static_cast<int>(std::round(click.y * scale_y));

    // Calculate animation progress (0.0 to 1.0) and the resulting ring radius.
    const double duration = (click.end_time - click.start_time).count();
    const double elapsed = (now - click.start_time).count();
    const double progress = std::clamp(elapsed / duration, 0.0, 1.0);

    const float radius = static_cast<float>(5.0 + 40.0 * progress);
    const float thickness = 3.0f;
    const float inner_radius_sq = std::pow(radius - thickness, 2);
    const float outer_radius_sq = std::pow(radius, 2);

    // Iterate over a bounding box for the ring and draw pixels that fall
    // within the calculated thickness.
    const int box_size = static_cast<int>(radius) + 1;
    for (int dy = -box_size; dy <= box_size; ++dy) {
      for (int dx = -box_size; dx <= box_size; ++dx) {
        const float dist_sq = static_cast<float>(dx * dx + dy * dy);
        if (dist_sq >= inner_radius_sq && dist_sq <= outer_radius_sq) {
          const int px = center_x + dx;
          const int py = center_y + dy;

          if (px >= 0 && px < frame_width && py >= 0 && py < frame_height) {
            // In YUV, pure white is represented by maximum luminance (Y=255)
            // and neutral chrominance (U=128, V=128).
            planes.y.data[py * planes.y.stride + px] = 255;

            // Chrominance (U/V) planes are sub-sampled 2x2 in YUV420p.
            if (px % 2 == 0 && py % 2 == 0) {
              planes.u.data[(py / 2) * planes.u.stride + (px / 2)] = 128;
              planes.v.data[(py / 2) * planes.v.stride + (px / 2)] = 128;
            }
          }
        }
      }
    }
  }
}

YuvPlanes LoopingFileSender::Downscale(const YuvPlanes& source,
                                       Resolution dest_size) {
  sws_context_ = sws_getCachedContext(sws_context_, source.width, source.height,
                                      AV_PIX_FMT_YUV420P, dest_size.width,
                                      dest_size.height, AV_PIX_FMT_YUV420P,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
  OSP_CHECK(sws_context_);

  // 32-byte stride alignment for SIMD and encoder efficiency.
  const int y_stride = (dest_size.width + 31) & ~31;
  const int uv_stride = ((dest_size.width + 1) / 2 + 31) & ~31;
  const size_t y_size = static_cast<size_t>(y_stride) * dest_size.height;
  const size_t uv_size =
      static_cast<size_t>(uv_stride) * ((dest_size.height + 1) / 2);
  scaled_yuv_buffer_.resize(y_size + 2 * uv_size);

  const std::span<uint8_t> buffer(scaled_yuv_buffer_);
  YuvPlanes dest{.width = dest_size.width,
                 .height = dest_size.height,
                 .y = {buffer.subspan(0, y_size), y_stride},
                 .u = {buffer.subspan(y_size, uv_size), uv_stride},
                 .v = {buffer.subspan(y_size + uv_size, uv_size), uv_stride}};

  // sws_scale() takes up to four planes. The fourth one is for alpha, which
  // YUV420P does not have.
  const std::array<const uint8_t*, 4> source_data = {
      source.y.data.data(), source.u.data.data(), source.v.data.data(),
      nullptr};
  const std::array<int, 4> source_strides = {source.y.stride, source.u.stride,
                                             source.v.stride, 0};
  const std::array<uint8_t*, 4> dest_data = {
      dest.y.data.data(), dest.u.data.data(), dest.v.data.data(), nullptr};
  const std::array<int, 4> dest_strides = {dest.y.stride, dest.u.stride,
                                           dest.v.stride, 0};
  const int scaled_height =
      sws_scale(sws_context_, source_data.data(), source_strides.data(), 0,
                source.height, dest_data.data(), dest_strides.data());
  OSP_CHECK_EQ(scaled_height, dest.height);
  return dest;
}

void LoopingFileSender::OnEndOfFile(SimulatedCapturer* capturer) {
  OSP_LOG_INFO << "The " << ToTrackName(capturer)
               << " capturer has reached the end of the media stream.";
  --num_capturers_running_;
  if (num_capturers_running_ == 0) {
    console_update_task_.Cancel();

    if (settings_.should_loop_video) {
      OSP_DLOG_INFO << "Starting the media stream over again.";
      next_task_.Schedule([this] { SendFileAgain(); }, Alarm::kImmediately);
    } else {
      OSP_DLOG_INFO << "Video complete. Exiting...";
      shutdown_callback_();
    }
  }
}

void LoopingFileSender::OnError(SimulatedCapturer* capturer,
                                const std::string& message) {
  OSP_LOG_ERROR << "The " << ToTrackName(capturer)
                << " has failed: " << message;
  --num_capturers_running_;
  // If both fail, the application just pauses. This accounts for things like
  // "file not found" errors. However, if only one track fails, then keep
  // going.
}

const char* LoopingFileSender::ToTrackName(SimulatedCapturer* capturer) const {
  if (capturer == audio_capturer_.get()) {
    return "audio";
  } else if (capturer == video_capturer_.get()) {
    return "video";
  } else {
    OSP_NOTREACHED();
  }
}

std::unique_ptr<StreamingVideoEncoder> LoopingFileSender::CreateVideoEncoder(
    const StreamingVideoEncoder::Parameters& params,
    TaskRunner& task_runner,
    std::unique_ptr<Sender> sender) {
  switch (params.codec) {
    case VideoCodec::kVp8:
    case VideoCodec::kVp9:
      return std::make_unique<StreamingVpxEncoder>(params, task_runner,
                                                   std::move(sender));
    case VideoCodec::kAv1:
#if defined(CAST_STANDALONE_SENDER_HAVE_LIBAOM)
      return std::make_unique<StreamingAv1Encoder>(params, task_runner,
                                                   std::move(sender));
#else
      OSP_LOG_FATAL << "AV1 codec selected, but could not be used because "
                       "LibAOM not installed.";
      return nullptr;
#endif
    case VideoCodec::kH264:
    case VideoCodec::kHevc:
#if defined(CAST_STANDALONE_SENDER_HAVE_FFMPEG)
      return std::make_unique<StreamingFfmpegEncoder>(params, task_runner,
                                                      std::move(sender));
#else
      OSP_LOG_FATAL
          << "H.264/HEVC codec selected, but could not be used because "
             "FFmpeg not installed.";
      return nullptr;
#endif
    default:
      OSP_LOG_ERROR << "Unsupported codec " << CodecToString(params.codec);
      OSP_NOTREACHED();
  }
}

}  // namespace openscreen::cast
