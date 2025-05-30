// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STANDALONE_SENDER_STREAMING_VPX_ENCODER_H_
#define CAST_STANDALONE_SENDER_STREAMING_VPX_ENCODER_H_

#include <vpx/vpx_encoder.h>
#include <vpx/vpx_image.h>

#include <algorithm>
#include <condition_variable>  // NOLINT
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "cast/standalone_sender/streaming_video_encoder.h"
#include "cast/streaming/public/constants.h"
#include "cast/streaming/public/frame_id.h"
#include "cast/streaming/rtp_time.h"
#include "platform/api/task_runner.h"
#include "platform/api/time.h"

namespace openscreen {

class TaskRunner;

namespace cast {

class Sender;

// Uses libvpx to encode VP8/9 video and streams it to a Sender. Includes
// extensive logic for fine-tuning the encoder parameters in real-time, to
// provide the best quality results given external, uncontrollable factors:
// CPU/network availability, and the complexity of the video frame content.
//
// Internally, a separate encode thread is created and used to prevent blocking
// the main thread while frames are being encoded. All public API methods are
// assumed to be called on the same sequence/thread as the main TaskRunner
// (injected via the constructor).
//
// Usage:
//
// 1. EncodeAndSend() is used to queue-up video frames for encoding and sending,
// which will be done on a best-effort basis.
//
// 2. The client is expected to call SetTargetBitrate() frequently based on its
// own bandwidth estimates and congestion control logic. In addition, a client
// may provide a callback for each frame's encode statistics, which can be used
// to further optimize the user experience. For example, the stats can be used
// as a signal to reduce the data volume (i.e., resolution and/or frame rate)
// coming from the video capture source.
class StreamingVpxEncoder : public StreamingVideoEncoder {
 public:
  StreamingVpxEncoder(const Parameters& params,
                      TaskRunner& task_runner,
                      std::unique_ptr<Sender> sender);

  ~StreamingVpxEncoder();

  int GetTargetBitrate() const override;
  void SetTargetBitrate(int new_bitrate) override;
  void EncodeAndSend(const VideoFrame& frame,
                     Clock::time_point reference_time,
                     std::function<void(Stats)> stats_callback) override;

 private:
  // Syntactic convenience to wrap the vpx_image_t alloc/free API in a smart
  // pointer.
  struct VpxImageDeleter {
    void operator()(vpx_image_t* ptr) const { vpx_img_free(ptr); }
  };
  using VpxImageUniquePtr = std::unique_ptr<vpx_image_t, VpxImageDeleter>;

  // Represents the state of one frame encode. This is created in
  // EncodeAndSend(), and passed to the encode thread via the `encode_queue_`.
  struct WorkUnit {
    VpxImageUniquePtr image;
    Clock::duration duration;
    Clock::time_point capture_begin_time;
    Clock::time_point capture_end_time;
    Clock::time_point reference_time;
    RtpTimeTicks rtp_timestamp;
    std::function<void(Stats)> stats_callback;
  };

  // Same as WorkUnit, but with additional fields to carry the encode results.
  struct WorkUnitWithResults : public WorkUnit {
    std::vector<uint8_t> payload;
    bool is_key_frame = false;
    Stats stats;
  };

  bool is_encoder_initialized() const { return config_.g_threads != 0; }

  // Destroys the VP8 encoder context if it has been initialized.
  void DestroyEncoder();

  // The procedure for the `encode_thread_` that loops, processing work units
  // from the `encode_queue_` by calling Encode() until it's time to end the
  // thread.
  void ProcessWorkUnitsUntilTimeToQuit();

  // If the `encoder_` is live, attempt reconfiguration to allow it to encode
  // frames at a new frame size or target bitrate. If reconfiguration is not
  // possible, destroy the existing instance and re-create a new `encoder_`
  // instance.
  void PrepareEncoder(int width, int height, int target_bitrate);

  // Wraps the complex libvpx vpx_codec_encode() call using inputs from
  // `work_unit` and populating results there.
  void EncodeFrame(bool force_key_frame, WorkUnitWithResults& work_unit);

  // Computes and populates `work_unit.stats` after the last call to
  // EncodeFrame().
  void ComputeFrameEncodeStats(Clock::duration encode_wall_time,
                               int target_bitrate,
                               WorkUnitWithResults& work_unit);

  // Assembles and enqueues an EncodedFrame with the Sender on the main thread.
  void SendEncodedFrame(WorkUnitWithResults results);

  // Allocates a vpx_image_t and copies the content from `frame` to it.
  static VpxImageUniquePtr CloneAsVpxImage(const VideoFrame& frame);

  // The reference time of the first frame passed to EncodeAndSend().
  Clock::time_point start_time_ = Clock::time_point::min();

  // The RTP timestamp of the last frame that was pushed into the
  // `encode_queue_` by EncodeAndSend(). This is used to check whether
  // timestamps are monotonically increasing.
  RtpTimeTicks last_enqueued_rtp_timestamp_;

  // Guards a few members shared by both the main and encode threads.
  std::mutex mutex_;

  // Used by the encode thread to sleep until more work is available.
  std::condition_variable cv_;  // ABSL_GUARDED_BY(mutex_)

  // These encode parameters not passed in the WorkUnit struct because it is
  // desirable for them to be applied as soon as possible, with the very next
  // WorkUnit popped from the `encode_queue_` on the encode thread, and not to
  // wait until some later WorkUnit is processed.
  bool needs_key_frame_ /* ABSL_GUARDED_BY(mutex_) */ = true;
  int target_bitrate_ /* ABSL_GUARDED_BY(mutex_) */ =
      2 << 20;  // Default: 2 Mbps.

  // The queue of frame encodes. The size of this queue is implicitly bounded by
  // EncodeAndSend(), where it checks for the total in-flight media duration and
  // maybe drops a frame.
  std::queue<WorkUnit> encode_queue_;  // ABSL_GUARDED_BY(mutex_)

  // Current VP8 encoder configuration. Most of the fields are unchanging, and
  // are populated in the ctor; but thereafter, only the encode thread accesses
  // this struct.
  //
  // The speed setting is controlled via a separate libvpx API (see members
  // below).
  vpx_codec_enc_cfg_t config_{};

  // libvpx VP8/9 encoder instance. Only the encode thread accesses this.
  vpx_codec_ctx_t encoder_;
};

}  // namespace cast
}  // namespace openscreen

#endif  // CAST_STANDALONE_SENDER_STREAMING_VPX_ENCODER_H_
