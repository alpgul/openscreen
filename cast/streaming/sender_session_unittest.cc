// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/sender_session.h"

#include <cstdio>
#include <utility>

#include "cast/streaming/capture_configs.h"
#include "cast/streaming/capture_recommendations.h"
#include "cast/streaming/mock_environment.h"
#include "cast/streaming/testing/simple_message_port.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/base/ip_address.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "util/chrono_helpers.h"
#include "util/stringprintf.h"

using ::testing::_;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrictMock;

namespace openscreen {
namespace cast {

namespace {
constexpr char kMalformedAnswerMessage[] = R"({
  "type": "ANSWER",
  "seqNum": 1,
  "answer": {
    "castMode": "mirroring",
    "udpPort": 1234,
    "sendIndexes": [1, 3],
    "ssrcs": [1, 2]
})";

constexpr char kValidJsonInvalidFormatAnswerMessage[] = R"({
  "type": "ANSWER",
  "seqNum": 1,
  "answer-2": {
    "castMode": "mirroring",
    "udpPort": 1234,
    "sendIndexes": [1, 3],
    "ssrcs": [1, 2]
  }
})";

constexpr char kValidJsonInvalidAnswerMessage[] = R"({
  "type": "ANSWER",
  "seqNum": 1,
  "answer": {
    "castMode": "mirroring",
    "udpPort": -1234,
    "sendIndexes": [1, 3],
    "ssrcs": [1, 2]
  }
})";

constexpr char kMissingAnswerMessage[] = R"({
  "type": "ANSWER",
  "seqNum": 1
})";

constexpr char kInvalidSequenceNumberMessage[] = R"({
  "type": "ANSWER",
  "seqNum": "not actually a number"
})";

constexpr char kUnknownTypeMessage[] = R"({
  "type": "ANSWER_VERSION_2",
  "seqNum": 1
})";

constexpr char kInvalidTypeMessage[] = R"({
  "type": 39,
  "seqNum": 1
})";

constexpr char kInvalidTypeMessageWithNoSeqNum[] = R"({
  "type": 39
})";

constexpr char kErrorAnswerMessage[] = R"({
  "seqNum": 1,
  "type": "ANSWER",
  "result": "error",
  "error": {
    "code": 123,
    "description": "something bad happened"
  }
})";

const AudioCaptureConfig kAudioCaptureConfigInvalidChannels{
    AudioCodec::kAac, -1 /* channels */, 44000 /* bit_rate */,
    96000 /* sample_rate */
};

const AudioCaptureConfig kAudioCaptureConfigValid{
    AudioCodec::kOpus, 5 /* channels */, 32000 /* bit_rate */,
    44000 /* sample_rate */
};

const VideoCaptureConfig kVideoCaptureConfigMissingResolutions{
    VideoCodec::kHevc, FrameRate{60, 1}, 300000 /* max_bit_rate */,
    std::vector<DisplayResolution>{}};

const VideoCaptureConfig kVideoCaptureConfigInvalid{
    VideoCodec::kHevc, FrameRate{60, 1}, -300000 /* max_bit_rate */,
    std::vector<DisplayResolution>{DisplayResolution{1920, 1080},
                                   DisplayResolution{1280, 720}}};

const VideoCaptureConfig kVideoCaptureConfigValid{
    VideoCodec::kHevc, FrameRate{60, 1}, 300000 /* max_bit_rate */,
    std::vector<DisplayResolution>{DisplayResolution{1280, 720},
                                   DisplayResolution{1920, 1080}}};

const VideoCaptureConfig kVideoCaptureConfigValidSimplest{
    VideoCodec::kHevc, FrameRate{60, 1}, 300000 /* max_bit_rate */,
    std::vector<DisplayResolution>{DisplayResolution{1920, 1080}}};

class FakeClient : public SenderSession::Client {
 public:
  MOCK_METHOD(void,
              OnNegotiated,
              (const SenderSession*,
               SenderSession::ConfiguredSenders,
               capture_recommendations::Recommendations),
              (override));
  MOCK_METHOD(void, OnError, (const SenderSession*, Error error), (override));
};

}  // namespace

class SenderSessionTest : public ::testing::Test {
 public:
  SenderSessionTest() : clock_(Clock::time_point{}), task_runner_(&clock_) {}

  std::unique_ptr<MockEnvironment> MakeEnvironment() {
    auto environment_ = std::make_unique<NiceMock<MockEnvironment>>(
        &FakeClock::now, &task_runner_);
    ON_CALL(*environment_, GetBoundLocalEndpoint())
        .WillByDefault(
            Return(IPEndpoint{IPAddress::Parse("127.0.0.1").value(), 12345}));
    return environment_;
  }

  void SetUp() {
    message_port_ = std::make_unique<SimpleMessagePort>("receiver-12345");
    environment_ = MakeEnvironment();
    session_ = std::make_unique<SenderSession>(IPAddress::kV4LoopbackAddress(),
                                               &client_, environment_.get(),
                                               message_port_.get());
  }

  std::string NegotiateOfferAndConstructAnswer() {
    const Error error = session_->Negotiate(
        std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
        std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});
    if (!error.ok()) {
      return {};
    }

    const auto& messages = message_port_->posted_messages();
    if (messages.size() != 1) {
      return {};
    }
    auto message_body = json::Parse(messages[0]);
    if (message_body.is_error()) {
      return {};
    }
    const Json::Value offer = std::move(message_body.value());
    EXPECT_EQ("OFFER", offer["type"].asString());
    EXPECT_LT(0, offer["seqNum"].asInt());

    const Json::Value& offer_body = offer["offer"];
    if (!offer_body.isObject()) {
      return {};
    }

    const Json::Value& streams = offer_body["supportedStreams"];
    EXPECT_TRUE(streams.isArray());
    EXPECT_EQ(2u, streams.size());

    const Json::Value& audio_stream = streams[0];
    const int audio_index = audio_stream["index"].asInt();
    const int audio_ssrc = audio_stream["ssrc"].asUInt();

    const Json::Value& video_stream = streams[1];
    const int video_index = video_stream["index"].asInt();
    const int video_ssrc = video_stream["ssrc"].asUInt();

    constexpr char kAnswerTemplate[] = R"({
        "type": "ANSWER",
        "seqNum": %d,
        "result": "ok",
        "answer": {
          "castMode": "mirroring",
          "udpPort": 1234,
          "sendIndexes": [%d, %d],
          "ssrcs": [%d, %d]
        }
        })";
    return StringPrintf(kAnswerTemplate, offer["seqNum"].asInt(), audio_index,
                        video_index, audio_ssrc + 1, video_ssrc + 1);
  }

 protected:
  StrictMock<FakeClient> client_;
  FakeClock clock_;
  std::unique_ptr<MockEnvironment> environment_;
  std::unique_ptr<SimpleMessagePort> message_port_;
  std::unique_ptr<SenderSession> session_;
  FakeTaskRunner task_runner_;
};

TEST_F(SenderSessionTest, ComplainsIfNoConfigsToOffer) {
  const Error error = session_->Negotiate(std::vector<AudioCaptureConfig>{},
                                          std::vector<VideoCaptureConfig>{});

  EXPECT_EQ(error,
            Error(Error::Code::kParameterInvalid,
                  "Need at least one audio or video config to negotiate."));
}

TEST_F(SenderSessionTest, ComplainsIfInvalidAudioCaptureConfig) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigInvalidChannels},
      std::vector<VideoCaptureConfig>{});

  EXPECT_EQ(error,
            Error(Error::Code::kParameterInvalid, "Invalid configs provided."));
}

TEST_F(SenderSessionTest, ComplainsIfInvalidVideoCaptureConfig) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigInvalid});
  EXPECT_EQ(error,
            Error(Error::Code::kParameterInvalid, "Invalid configs provided."));
}

TEST_F(SenderSessionTest, ComplainsIfMissingResolutions) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigMissingResolutions});
  EXPECT_EQ(error,
            Error(Error::Code::kParameterInvalid, "Invalid configs provided."));
}

TEST_F(SenderSessionTest, SendsOfferWithZeroBitrateOptions) {
  VideoCaptureConfig video_config = kVideoCaptureConfigValid;
  video_config.max_bit_rate = 0;
  AudioCaptureConfig audio_config = kAudioCaptureConfigValid;
  audio_config.bit_rate = 0;

  const Error error =
      session_->Negotiate(std::vector<AudioCaptureConfig>{audio_config},
                          std::vector<VideoCaptureConfig>{video_config});
  EXPECT_TRUE(error.ok());

  const auto& messages = message_port_->posted_messages();
  ASSERT_EQ(1u, messages.size());
  auto message_body = json::Parse(messages[0]);
  ASSERT_TRUE(message_body.is_value());
  const Json::Value offer = std::move(message_body.value());
  EXPECT_EQ("OFFER", offer["type"].asString());
}

TEST_F(SenderSessionTest, SendsOfferWithSimpleVideoOnly) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});
  EXPECT_TRUE(error.ok());

  const auto& messages = message_port_->posted_messages();
  ASSERT_EQ(1u, messages.size());
  auto message_body = json::Parse(messages[0]);
  ASSERT_TRUE(message_body.is_value());
  const Json::Value offer = std::move(message_body.value());
  EXPECT_EQ("OFFER", offer["type"].asString());
}

TEST_F(SenderSessionTest, SendsOfferAudioOnly) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{});
  EXPECT_TRUE(error.ok());

  const auto& messages = message_port_->posted_messages();
  ASSERT_EQ(1u, messages.size());
  auto message_body = json::Parse(messages[0]);
  ASSERT_TRUE(message_body.is_value());
  const Json::Value offer = std::move(message_body.value());
  EXPECT_EQ("OFFER", offer["type"].asString());
}

TEST_F(SenderSessionTest, SendsOfferMessage) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  const auto& messages = message_port_->posted_messages();
  ASSERT_EQ(1u, messages.size());

  auto message_body = json::Parse(messages[0]);
  ASSERT_TRUE(message_body.is_value());
  const Json::Value offer = std::move(message_body.value());
  EXPECT_EQ("OFFER", offer["type"].asString());
  EXPECT_LT(0, offer["seqNum"].asInt());

  const Json::Value& offer_body = offer["offer"];
  ASSERT_FALSE(offer_body.isNull());
  ASSERT_TRUE(offer_body.isObject());
  EXPECT_EQ("mirroring", offer_body["castMode"].asString());
  EXPECT_EQ(false, offer_body["receiverGetStatus"].asBool());

  const Json::Value& streams = offer_body["supportedStreams"];
  EXPECT_TRUE(streams.isArray());
  EXPECT_EQ(2u, streams.size());

  const Json::Value& audio_stream = streams[0];
  EXPECT_EQ("opus", audio_stream["codecName"].asString());
  EXPECT_EQ(0, audio_stream["index"].asInt());
  EXPECT_EQ(32u, audio_stream["aesKey"].asString().length());
  EXPECT_EQ(32u, audio_stream["aesIvMask"].asString().length());
  EXPECT_EQ(5, audio_stream["channels"].asInt());
  EXPECT_LT(0u, audio_stream["ssrc"].asUInt());
  EXPECT_EQ(127, audio_stream["rtpPayloadType"].asInt());

  const Json::Value& video_stream = streams[1];
  EXPECT_EQ("hevc", video_stream["codecName"].asString());
  EXPECT_EQ(1, video_stream["index"].asInt());
  EXPECT_EQ(32u, video_stream["aesKey"].asString().length());
  EXPECT_EQ(32u, video_stream["aesIvMask"].asString().length());
  EXPECT_EQ(1, video_stream["channels"].asInt());
  EXPECT_LT(0u, video_stream["ssrc"].asUInt());
  EXPECT_EQ(96, video_stream["rtpPayloadType"].asInt());
}

TEST_F(SenderSessionTest, HandlesValidAnswer) {
  std::string answer = NegotiateOfferAndConstructAnswer();

  EXPECT_CALL(client_, OnNegotiated(session_.get(), _, _));
  message_port_->ReceiveMessage(answer);
}

TEST_F(SenderSessionTest, HandlesInvalidNamespace) {
  std::string answer = NegotiateOfferAndConstructAnswer();
  message_port_->ReceiveMessage("random-namespace", answer);
}

TEST_F(SenderSessionTest, HandlesMalformedAnswer) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  // Note that unlike when we simply don't select any streams, when the answer
  // is actually malformed we have no way of knowing it was an answer at all,
  // so we report an Error and drop the message.
  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kMalformedAnswerMessage);
}

TEST_F(SenderSessionTest, HandlesImproperlyFormattedAnswer) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kValidJsonInvalidFormatAnswerMessage);
}

TEST_F(SenderSessionTest, HandlesInvalidAnswer) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kValidJsonInvalidAnswerMessage);
}

TEST_F(SenderSessionTest, HandlesNullAnswer) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  EXPECT_TRUE(error.ok());
  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kMissingAnswerMessage);
}

TEST_F(SenderSessionTest, HandlesInvalidSequenceNumber) {
  const Error error = session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  // We should just discard messages with an invalid sequence number.
  message_port_->ReceiveMessage(kInvalidSequenceNumberMessage);
}

TEST_F(SenderSessionTest, HandlesUnknownTypeMessageWithValidSeqNum) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  // If a message is of unknown type but has an expected seqnum, it's
  // probably a malformed response.
  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kUnknownTypeMessage);
}

TEST_F(SenderSessionTest, HandlesInvalidTypeMessageWithValidSeqNum) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  // If a message is of unknown type but has an expected seqnum, it's
  // probably a malformed response.
  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kInvalidTypeMessage);
}

TEST_F(SenderSessionTest, HandlesInvalidTypeMessage) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  // We should just discard messages with an invalid message type and
  // no sequence number.
  message_port_->ReceiveMessage(kInvalidTypeMessageWithNoSeqNum);
}

TEST_F(SenderSessionTest, HandlesErrorMessage) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  // We should report error answers.
  EXPECT_CALL(client_, OnError(session_.get(), _));
  message_port_->ReceiveMessage(kErrorAnswerMessage);
}

TEST_F(SenderSessionTest, DoesntCrashOnMessagePortError) {
  session_->Negotiate(
      std::vector<AudioCaptureConfig>{kAudioCaptureConfigValid},
      std::vector<VideoCaptureConfig>{kVideoCaptureConfigValid});

  message_port_->ReceiveError(Error(Error::Code::kUnknownError));
}

}  // namespace cast
}  // namespace openscreen
