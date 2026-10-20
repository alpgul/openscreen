// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "cast/common/public/message_port.h"
#include "cast/streaming/capture_configs.h"
#include "cast/streaming/public/encoded_frame.h"
#include "cast/streaming/public/environment.h"
#include "cast/streaming/public/receiver.h"
#include "cast/streaming/public/receiver_constraints.h"
#include "cast/streaming/public/receiver_session.h"
#include "cast/streaming/public/sender.h"
#include "cast/streaming/public/sender_session.h"
#include "gtest/gtest.h"
#include "platform/api/time.h"
#include "platform/base/ip_address.h"
#include "platform/impl/platform_client_posix.h"
#include "platform/impl/task_runner.h"
#include "testing/util/task_util.h"
#include "util/alarm.h"
#include "util/osp_logging.h"
#include "util/raw_ptr.h"
#include "util/raw_ref.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {
namespace {

// One end of an asynchronous message port between the sender and receiver
// sessions, which delivers messages in separate TaskRunner tasks, like a real
// network-backed MessagePort would. Ports are created in connected pairs by
// AsyncMessagePortPair. Must be used and destroyed on the TaskRunner.
class AsyncMessagePort final : public MessagePort {
 public:
  AsyncMessagePort(TaskRunner& task_runner, std::string_view destination_id)
      : task_runner_(task_runner), destination_id_(destination_id) {}

  ~AsyncMessagePort() override = default;

  // Connects `a` and `b` to each other. Each port may only be connected once.
  static void Connect(AsyncMessagePort& a, AsyncMessagePort& b) {
    OSP_CHECK(!a.other_end_);
    OSP_CHECK(!b.other_end_);
    a.other_end_ = b.weak_factory_.GetWeakPtr();
    b.other_end_ = a.weak_factory_.GetWeakPtr();
  }

  void SetClient(MessagePort::Client& client) override { client_ = &client; }

  void ResetClient() override { client_ = nullptr; }

  void PostMessage(const std::string& sender_id,
                   const std::string& message_namespace,
                   const std::string& message) override {
    // Messages may only be posted while this port is connected to a live
    // other end. The other end may still be destroyed, or have its client
    // reset, before the message is delivered, in which case the message is
    // dropped.
    OSP_CHECK(other_end_);
    task_runner_->PostTask(
        [other_end = other_end_, message_namespace, message] {
          if (other_end && other_end->client_) {
            other_end->client_->OnMessage(other_end->destination_id_,
                                          message_namespace, message);
          }
        });
  }

 private:
  const raw_ref<TaskRunner> task_runner_;
  const std::string destination_id_;
  raw_ptr<MessagePort::Client> client_ = nullptr;
  WeakPtr<AsyncMessagePort> other_end_;
  WeakPtrFactory<AsyncMessagePort> weak_factory_{this};
};

// Owns a pair of connected AsyncMessagePorts.
class AsyncMessagePortPair {
 public:
  AsyncMessagePortPair(TaskRunner& task_runner,
                       std::string_view left_id,
                       std::string_view right_id)
      : left_(task_runner, right_id), right_(task_runner, left_id) {
    AsyncMessagePort::Connect(left_, right_);
  }

  AsyncMessagePort& left() { return left_; }
  AsyncMessagePort& right() { return right_; }

 private:
  AsyncMessagePort left_;
  AsyncMessagePort right_;
};

// Enqueues `count` fake encoded frames on a Sender, one every `interval`.
// Destroying it stops any remaining frames from being sent. Must be created,
// used, and destroyed on the TaskRunner.
class PacedFrameSender {
 public:
  enum class Kind { kAudio, kVideo };

  PacedFrameSender(TaskRunner& task_runner,
                   Sender& sender,
                   Kind kind,
                   int count,
                   std::chrono::milliseconds interval)
      : sender_(sender),
        kind_(kind),
        count_(count),
        interval_(interval),
        payload_(kind == Kind::kVideo ? 1024 : 256, 0x42),
        start_time_(Clock::now()),
        alarm_(&Clock::now, task_runner) {
    SendNextFrame();
  }

 private:
  void SendNextFrame() {
    if (index_ >= count_) {
      return;
    }

    // Every audio frame is a key frame. Video starts with a key frame, and
    // each later frame depends on the previous one.
    const bool is_key_frame = kind_ == Kind::kAudio || index_ == 0;
    const FrameId frame_id = sender_->GetNextFrameId();
    EncodedFrame frame;
    frame.frame_id = frame_id;
    frame.referenced_frame_id = is_key_frame ? frame_id : frame_id - 1;
    frame.dependency = is_key_frame ? EncodedFrame::Dependency::kKeyFrame
                                    : EncodedFrame::Dependency::kDependent;
    frame.rtp_timestamp = RtpTimeTicks::FromTimeSinceOrigin(
        index_ * interval_, sender_->config().rtp_timebase);
    frame.reference_time = start_time_ + index_ * interval_;
    const Clock::time_point now = Clock::now();
    frame.capture_begin_time = now - std::chrono::microseconds(600);
    frame.capture_end_time = now - std::chrono::microseconds(400);
    frame.data = payload_;
    EXPECT_EQ(sender_->EnqueueFrame(frame), Sender::EnqueueFrameResult::OK);

    ++index_;
    alarm_.ScheduleFromNow([this] { SendNextFrame(); }, interval_);
  }

  const raw_ref<Sender> sender_;
  const Kind kind_;
  const int count_;
  const std::chrono::milliseconds interval_;
  const std::vector<uint8_t> payload_;
  const Clock::time_point start_time_;
  int index_ = 0;
  Alarm alarm_;
};

// In-process consumer that records received frames and reports playout events.
class TestFrameConsumer final : public Receiver::Consumer {
 public:
  explicit TestFrameConsumer(Receiver& receiver) : receiver_(receiver) {
    receiver_->SetConsumer(this);
  }

  ~TestFrameConsumer() override { receiver_->SetConsumer(nullptr); }

  void OnFramesReady(size_t next_frame_buffer_size) override {
    buffer_.resize(next_frame_buffer_size);
    const EncodedFrame frame = receiver_->ConsumeNextFrame(buffer_);
    receiver_->ReportPlayoutEvent(frame.frame_id, frame.rtp_timestamp,
                                  Clock::now());
    std::lock_guard<std::mutex> lock(mutex_);
    received_frames_.push_back(frame.frame_id);
  }

  std::vector<FrameId> received_frames() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_frames_;
  }

  size_t frame_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_frames_.size();
  }

 private:
  const raw_ref<Receiver> receiver_;
  std::vector<uint8_t> buffer_;
  mutable std::mutex mutex_;
  std::vector<FrameId> received_frames_;
};

class TestSenderSessionClient final : public SenderSession::Client {
 public:
  void OnNegotiated(const SenderSession* session,
                    SenderSession::ConfiguredSenders senders,
                    capture_recommendations::Recommendations
                        capture_recommendations) override {
    std::lock_guard<std::mutex> lock(mutex_);
    senders_ = std::move(senders);
    negotiated_ = true;
  }

  void OnError(const SenderSession* session, const Error& error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
    error_occurred_ = true;
    OSP_LOG_ERROR << "SenderSession error: " << error;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    senders_ = {};
    negotiated_ = false;
    error_occurred_ = false;
  }

  bool is_negotiated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return negotiated_;
  }
  bool has_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_occurred_;
  }
  SenderSession::ConfiguredSenders& senders() {
    std::lock_guard<std::mutex> lock(mutex_);
    return senders_;
  }

 private:
  mutable std::mutex mutex_;
  SenderSession::ConfiguredSenders senders_;
  bool negotiated_ = false;
  bool error_occurred_ = false;
  Error last_error_;
};

class TestReceiverSessionClient final : public ReceiverSession::Client {
 public:
  void OnNegotiated(const ReceiverSession* session,
                    ReceiverSession::ConfiguredReceivers receivers) override {
    std::lock_guard<std::mutex> lock(mutex_);
    receivers_ = std::move(receivers);
    negotiated_ = true;
  }

  void OnReceiversDestroying(const ReceiverSession* session,
                             ReceiversDestroyingReason reason) override {
    std::lock_guard<std::mutex> lock(mutex_);
    receivers_ = {};
  }

  void OnError(const ReceiverSession* session, const Error& error) override {
    std::lock_guard<std::mutex> lock(mutex_);
    last_error_ = error;
    error_occurred_ = true;
    OSP_LOG_ERROR << "ReceiverSession error: " << error;
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    receivers_ = {};
    negotiated_ = false;
    error_occurred_ = false;
  }

  bool is_negotiated() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return negotiated_;
  }
  bool has_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_occurred_;
  }
  const ReceiverSession::ConfiguredReceivers& receivers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return receivers_;
  }

 private:
  mutable std::mutex mutex_;
  ReceiverSession::ConfiguredReceivers receivers_;
  bool negotiated_ = false;
  bool error_occurred_ = false;
  Error last_error_;
};

}  // namespace

class InProcessStreamingE2ETest : public ::testing::Test {
 public:
  void SetUp() override {
    PlatformClientPosix::Create(std::chrono::milliseconds(10));
    task_runner_ = &PlatformClientPosix::GetInstance()->GetTaskRunner();

    RunOnTaskRunner(*task_runner_, [this]() {
      message_ports_ = std::make_unique<AsyncMessagePortPair>(
          *task_runner_, "sender-session", "receiver-session");

      // Construct local loopback UDP environments for sender and receiver.
      sender_env_ = std::make_unique<Environment>(
          &Clock::now, *task_runner_,
          IPEndpoint{IPAddress::kV4LoopbackAddress(), 0});
      receiver_env_ = std::make_unique<Environment>(
          &Clock::now, *task_runner_,
          IPEndpoint{IPAddress::kV4LoopbackAddress(), 0});

      ReceiverConstraints constraints;
      constraints.video_codecs = {VideoCodec::kVp8, VideoCodec::kVp9};
      constraints.audio_codecs = {AudioCodec::kOpus};

      receiver_session_ = std::make_unique<ReceiverSession>(
          receiver_client_, *receiver_env_, message_ports_->right(),
          std::move(constraints));

      SenderSession::Configuration sender_config{
          IPAddress::kV4LoopbackAddress(),
          sender_client_,
          sender_env_.get(),
          &message_ports_->left(),
          "sender-session",
          "receiver-session"};
      sender_session_ =
          std::make_unique<SenderSession>(std::move(sender_config));
    });
  }

  void TearDown() override {
    // Everything is destroyed on the TaskRunner, since these objects must only
    // be used and destroyed there, in reverse dependency order, even if the
    // test failed part-way through. RunOnTaskRunner() blocks until this task
    // has completed, so this all happens before the fixture is destroyed. Any
    // tasks that are still queued afterwards (e.g., message deliveries) are
    // no-ops.
    RunOnTaskRunner(*task_runner_, [this]() {
      audio_frames_.reset();
      video_frames_.reset();
      audio_consumer_.reset();
      video_consumer_.reset();
      sender_client_.Reset();
      receiver_client_.Reset();
      sender_session_.reset();
      receiver_session_.reset();
      sender_env_.reset();
      receiver_env_.reset();
      message_ports_.reset();
    });
    PlatformClientPosix::ShutDown();
  }

  void WaitForNegotiation() {
    ASSERT_NO_FATAL_FAILURE(WaitForCondition(
        [this]() {
          return sender_client_.is_negotiated() &&
                 receiver_client_.is_negotiated();
        },
        std::chrono::milliseconds(50), 100));
    EXPECT_FALSE(sender_client_.has_error());
    EXPECT_FALSE(receiver_client_.has_error());
  }

 protected:
  raw_ptr<TaskRunner> task_runner_;
  std::unique_ptr<AsyncMessagePortPair> message_ports_;
  std::unique_ptr<Environment> sender_env_;
  std::unique_ptr<Environment> receiver_env_;
  TestSenderSessionClient sender_client_;
  TestReceiverSessionClient receiver_client_;
  std::unique_ptr<SenderSession> sender_session_;
  std::unique_ptr<ReceiverSession> receiver_session_;

  // Created by the tests, on the TaskRunner.
  std::unique_ptr<TestFrameConsumer> audio_consumer_;
  std::unique_ptr<TestFrameConsumer> video_consumer_;
  std::unique_ptr<PacedFrameSender> audio_frames_;
  std::unique_ptr<PacedFrameSender> video_frames_;
};

// Tests Offer/Answer negotiation and full in-process RTP packet streaming
// for video without requiring external codecs or media files.
//
// TODO(issuetracker.google.com/567666727): Expand configuration coverage,
// focusing on parameters with custom handling, such as non-VP8 video codecs,
// multiple and constrained resolutions, audio channel counts and sample rates,
// and receiver constraints that cause streams to be rejected.
TEST_F(InProcessStreamingE2ETest, NegotiateAndStreamVideo) {
  VideoCaptureConfig video_config;
  video_config.codec = VideoCodec::kVp8;
  video_config.resolutions = {{1280, 720}};

  RunOnTaskRunner(*task_runner_, [this, &video_config]() {
    EXPECT_EQ(sender_session_->Negotiate({}, {video_config}), Error::None());
  });
  ASSERT_NO_FATAL_FAILURE(WaitForNegotiation());

  ASSERT_TRUE(sender_client_.senders().video_sender);
  ASSERT_TRUE(receiver_client_.receivers().video_receiver);

  static constexpr int kNumFrames = 15;
  RunOnTaskRunner(*task_runner_, [this]() {
    video_consumer_ = std::make_unique<TestFrameConsumer>(
        *receiver_client_.receivers().video_receiver);
    video_frames_ = std::make_unique<PacedFrameSender>(
        *task_runner_, *sender_client_.senders().video_sender,
        PacedFrameSender::Kind::kVideo, kNumFrames,
        std::chrono::milliseconds(16));
  });

  ASSERT_NO_FATAL_FAILURE(WaitForCondition(
      [this]() { return video_consumer_->frame_count() >= kNumFrames; },
      std::chrono::milliseconds(50), 100));

  const std::vector<FrameId> received = video_consumer_->received_frames();
  ASSERT_EQ(received.size(), static_cast<size_t>(kNumFrames));
  for (int i = 0; i < kNumFrames; ++i) {
    EXPECT_EQ(received[i], FrameId::first() + i);
  }
}

// Tests Offer/Answer negotiation and full in-process RTP packet streaming
// for audio only.
TEST_F(InProcessStreamingE2ETest, NegotiateAndStreamAudio) {
  AudioCaptureConfig audio_config;
  audio_config.codec = AudioCodec::kOpus;

  RunOnTaskRunner(*task_runner_, [this, &audio_config]() {
    EXPECT_EQ(sender_session_->Negotiate({audio_config}, {}), Error::None());
  });
  ASSERT_NO_FATAL_FAILURE(WaitForNegotiation());

  ASSERT_TRUE(sender_client_.senders().audio_sender);
  ASSERT_FALSE(sender_client_.senders().video_sender);
  ASSERT_TRUE(receiver_client_.receivers().audio_receiver);
  ASSERT_FALSE(receiver_client_.receivers().video_receiver);

  static constexpr int kNumFrames = 15;
  RunOnTaskRunner(*task_runner_, [this]() {
    audio_consumer_ = std::make_unique<TestFrameConsumer>(
        *receiver_client_.receivers().audio_receiver);
    audio_frames_ = std::make_unique<PacedFrameSender>(
        *task_runner_, *sender_client_.senders().audio_sender,
        PacedFrameSender::Kind::kAudio, kNumFrames,
        std::chrono::milliseconds(10));
  });

  ASSERT_NO_FATAL_FAILURE(WaitForCondition(
      [this]() { return audio_consumer_->frame_count() >= kNumFrames; },
      std::chrono::milliseconds(50), 100));

  const std::vector<FrameId> received = audio_consumer_->received_frames();
  ASSERT_EQ(received.size(), static_cast<size_t>(kNumFrames));
  for (int i = 0; i < kNumFrames; ++i) {
    EXPECT_EQ(received[i], FrameId::first() + i);
  }
}

// Tests concurrent Audio and Video streaming in-process over loopback.
TEST_F(InProcessStreamingE2ETest, NegotiateAndStreamAudioVideo) {
  AudioCaptureConfig audio_config;
  audio_config.codec = AudioCodec::kOpus;

  VideoCaptureConfig video_config;
  video_config.codec = VideoCodec::kVp8;
  video_config.resolutions = {{1920, 1080}};

  RunOnTaskRunner(*task_runner_, [this, &audio_config, &video_config]() {
    EXPECT_EQ(sender_session_->Negotiate({audio_config}, {video_config}),
              Error::None());
  });
  ASSERT_NO_FATAL_FAILURE(WaitForNegotiation());

  ASSERT_TRUE(sender_client_.senders().audio_sender);
  ASSERT_TRUE(sender_client_.senders().video_sender);
  ASSERT_TRUE(receiver_client_.receivers().audio_receiver);
  ASSERT_TRUE(receiver_client_.receivers().video_receiver);

  static constexpr int kNumFrames = 20;
  RunOnTaskRunner(*task_runner_, [this]() {
    audio_consumer_ = std::make_unique<TestFrameConsumer>(
        *receiver_client_.receivers().audio_receiver);
    video_consumer_ = std::make_unique<TestFrameConsumer>(
        *receiver_client_.receivers().video_receiver);
    audio_frames_ = std::make_unique<PacedFrameSender>(
        *task_runner_, *sender_client_.senders().audio_sender,
        PacedFrameSender::Kind::kAudio, kNumFrames,
        std::chrono::milliseconds(10));
    video_frames_ = std::make_unique<PacedFrameSender>(
        *task_runner_, *sender_client_.senders().video_sender,
        PacedFrameSender::Kind::kVideo, kNumFrames,
        std::chrono::milliseconds(16));
  });

  ASSERT_NO_FATAL_FAILURE(WaitForCondition(
      [this]() {
        return audio_consumer_->frame_count() >= kNumFrames &&
               video_consumer_->frame_count() >= kNumFrames;
      },
      std::chrono::milliseconds(50), 100));

  EXPECT_EQ(audio_consumer_->frame_count(), static_cast<size_t>(kNumFrames));
  EXPECT_EQ(video_consumer_->frame_count(), static_cast<size_t>(kNumFrames));
}

}  // namespace openscreen::cast
