// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_LOOPING_FILE_SENDER_H_
#define CAST_STANDALONE_SENDER_LOOPING_FILE_SENDER_H_

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cast/standalone_common/ffmpeg_glue.h"
#include "cast/standalone_sender/connection_settings.h"
#include "cast/standalone_sender/constants.h"
#include "cast/standalone_sender/file_sender.h"
#include "cast/standalone_sender/simulated_capturer.h"
#include "cast/standalone_sender/streaming_opus_encoder.h"
#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/streaming/public/sender_session.h"
#include "cast/streaming/resolution.h"
#include "util/raw_ptr.h"

namespace openscreen::cast {

// If `source` is larger than `target`, scales it down to fit while keeping the
// same aspect ratio. If `source` already fits, returns `source` as is.
Resolution GetMaybeDownscaledResolution(Resolution source, Resolution target);

// Returns the visible area (inside the crop rectangle) of the YUV420P `frame`.
// The planes are views into the frame's own buffers, so no pixels are copied.
YuvPlanes GetVisiblePlanes(const AVFrame& frame);

// Plays the media file at a given path over and over again, transcoding and
// streaming its audio/video.
class LoopingFileSender final : public FileSender,
                                public SimulatedAudioCapturer::Client,
                                public SimulatedVideoCapturer::Client {
 public:
  // `video_encoder` may be provided to inject a specific encoder
  // implementation (e.g. a fake in unit tests). When null, an encoder is
  // created based on `settings.codec`.
  LoopingFileSender(
      Environment& environment,
      ConnectionSettings settings,
      const SenderSession* session,
      SenderSession::ConfiguredSenders senders,
      ShutdownCallback shutdown_callback,
      std::unique_ptr<StreamingVideoEncoder> video_encoder = nullptr);

  ~LoopingFileSender() final;

  void SetPlaybackRate(double rate) override;

  void OnInputMessage(InputMessage message) override;

  // SimulatedAudioCapturer::Client overrides.
  void OnAudioData(const float* interleaved_samples,
                   int num_samples,
                   Clock::time_point capture_begin_time,
                   Clock::time_point capture_end_time,
                   Clock::time_point reference_time) final;

  // SimulatedVideoCapturer::Client overrides.
  void OnVideoFrame(const AVFrame& av_frame,
                    Clock::time_point capture_begin_time,
                    Clock::time_point capture_end_time,
                    Clock::time_point reference_time) final;

 private:
  void UpdateEncoderBitrates();
  void ControlForNetworkCongestion();
  void SendFileAgain();

  void UpdateStatusOnConsole();

  // Draws any active animations (like mouse clicks) onto `planes`, which must
  // hold a YUV420P image.
  void DrawAnimations(YuvPlanes& planes);

  // Scales the YUV420P image in `source` to `dest_size`. The returned planes
  // point into `scaled_yuv_buffer_`, so they stay valid until the next call.
  YuvPlanes Downscale(const YuvPlanes& source, Resolution dest_size);

  // SimulatedCapturer::Client overrides.
  void OnEndOfFile(SimulatedCapturer* capturer) final;
  void OnError(SimulatedCapturer* capturer, const std::string& message) final;

  const char* ToTrackName(SimulatedCapturer* capturer) const;

  std::unique_ptr<StreamingVideoEncoder> CreateVideoEncoder(
      const StreamingVideoEncoder::Parameters& params,
      TaskRunner& task_runner,
      std::unique_ptr<Sender> sender);

  // Holds the required injected dependencies (clock, task runner) used for Cast
  // Streaming, and owns the UDP socket over which all communications occur with
  // the remote's Receivers.
  Environment& env_;

  // The connection settings used for this session.
  const ConnectionSettings settings_;

  // Session to query for bandwidth information.
  const raw_ptr<const SenderSession> session_;

  // Callback for tearing down the sender process.
  ShutdownCallback shutdown_callback_;

  int bandwidth_estimate_ = 0;
  int bandwidth_being_utilized_;

  std::unique_ptr<StreamingOpusEncoder> audio_encoder_;
  std::unique_ptr<StreamingVideoEncoder> video_encoder_;

  int num_capturers_running_ = 0;
  Clock::time_point capture_begin_time_{};
  Clock::time_point latest_frame_time_{};

  // The resolution negotiated with the receiver, if any. This is an upper
  // bound: frames larger than this are downscaled, smaller ones are sent at
  // their native resolution rather than being upscaled.
  std::optional<Resolution> target_resolution_;

  // Kept across frames by Downscale(), so they are only reallocated when the
  // frame size changes.
  SwsContext* sws_context_ = nullptr;
  std::vector<uint8_t> scaled_yuv_buffer_;
  std::unique_ptr<SimulatedAudioCapturer> audio_capturer_;
  std::unique_ptr<SimulatedVideoCapturer> video_capturer_;

  struct Click {
    float x;
    float y;
    Clock::time_point start_time;
    Clock::time_point end_time;
  };
  std::vector<Click> active_clicks_;

  Alarm next_task_;
  Alarm console_update_task_;
};

}  // namespace openscreen::cast

#endif  // CAST_STANDALONE_SENDER_LOOPING_FILE_SENDER_H_
