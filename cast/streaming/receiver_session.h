// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_RECEIVER_SESSION_H_
#define CAST_STREAMING_RECEIVER_SESSION_H_

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "cast/common/public/message_port.h"
#include "cast/streaming/capture_configs.h"
#include "cast/streaming/constants.h"
#include "cast/streaming/offer_messages.h"
#include "cast/streaming/receiver_packet_router.h"
#include "cast/streaming/resolution.h"
#include "cast/streaming/sender_message.h"
#include "cast/streaming/session_config.h"
#include "cast/streaming/session_messager.h"

namespace openscreen {
namespace cast {

class Environment;
class Receiver;

// This class is responsible for listening for streaming (both mirroring and
// remoting) requests from Cast Sender devices, then negotiating capture
// constraints and instantiating audio and video Receiver objects.
//   The owner of this session is expected to provide a client for
// updates, an environment for getting UDP socket information (as well as
// other OS dependencies), and a set of preferences to be used for
// negotiation.
//
// NOTE: In some cases, the session initialization may be pending waiting for
// the UDP socket to be ready. In this case, the receivers and the answer
// message will not be configured and sent until the UDP socket has finished
// binding.
class ReceiverSession final : public Environment::SocketSubscriber {
 public:
  // Upon successful negotiation, a set of configured receivers is constructed
  // for handling audio and video. Note that either receiver may be null.
  struct ConfiguredReceivers {
    // In practice, we may have 0, 1, or 2 receivers configured, depending
    // on if the device supports audio and video, and if we were able to
    // successfully negotiate a receiver configuration.

    // NOTES ON LIFETIMES: The audio and video Receiver pointers are owned by
    // ReceiverSession, not the Client, and references to these pointers must be
    // cleared before a call to Client::OnReceiversDestroying() returns.

    // If the receiver is audio- or video-only, or we failed to negotiate
    // an acceptable session configuration with the sender, then either of the
    // receivers may be nullptr. In this case, the associated config is default
    // initialized and should be ignored.
    Receiver* audio_receiver;
    AudioCaptureConfig audio_config;

    Receiver* video_receiver;
    VideoCaptureConfig video_config;
  };

  // The embedder should provide a client for handling connections.
  // When a connection is established, the OnNegotiated callback is called.
  class Client {
   public:
    enum ReceiversDestroyingReason { kEndOfSession, kRenegotiated };

    // Called when a new set of receivers has been negotiated. This may be
    // called multiple times during a session, as renegotiations occur.
    virtual void OnNegotiated(const ReceiverSession* session,
                              ConfiguredReceivers receivers) = 0;

    // Called immediately preceding the destruction of this session's receivers.
    // If |reason| is |kEndOfSession|, OnNegotiated() will never be called
    // again; if it is |kRenegotiated|, OnNegotiated() will be called again
    // soon with a new set of Receivers to use.
    //
    // Before returning, the implementation must ensure that all references to
    // the Receivers, from the last call to OnNegotiated(), have been cleared.
    virtual void OnReceiversDestroying(const ReceiverSession* session,
                                       ReceiversDestroyingReason reason) = 0;

    virtual void OnError(const ReceiverSession* session, Error error) = 0;

   protected:
    virtual ~Client();
  };

  // Information about the display the receiver is attached to.
  struct Display {
    // The display limitations of the actual screen, used to provide upper
    // bounds on mirroring and remoting streams. For example, we will never
    // send 60FPS if it is going to be displayed on a 30FPS screen.
    // Note that we may exceed the display width and height for standard
    // content sizes like 720p or 1080p.
    Dimensions dimensions;

    // Whether the embedder is capable of scaling content. If set to false,
    // the sender will manage the aspect ratio scaling.
    bool can_scale_content = false;
  };

  // Codec-specific audio limits for playback.
  struct AudioLimits {
    // Whether or not these limits apply to all codecs.
    bool applies_to_all_codecs = false;

    // Audio codec these limits apply to. Note that if |applies_to_all_codecs|
    // is true this field is ignored.
    AudioCodec codec;

    // Maximum audio sample rate.
    int max_sample_rate = kDefaultAudioSampleRate;

    // Maximum audio channels, default is currently stereo.
    int max_channels = kDefaultAudioChannels;

    // Minimum and maximum bitrates. Generally capture is done at the maximum
    // bit rate, since audio bandwidth is much lower than video for most
    // content.
    int min_bit_rate = kDefaultAudioMinBitRate;
    int max_bit_rate = kDefaultAudioMaxBitRate;

    // Max playout delay in milliseconds.
    std::chrono::milliseconds max_delay = kDefaultMaxDelayMs;
  };

  // Codec-specific video limits for playback.
  struct VideoLimits {
    // Whether or not these limits apply to all codecs.
    bool applies_to_all_codecs = false;

    // Video codec these limits apply to. Note that if |applies_to_all_codecs|
    // is true this field is ignored.
    VideoCodec codec;

    // Maximum pixels per second. Value is the standard amount of pixels
    // for 1080P at 30FPS.
    int max_pixels_per_second = 1920 * 1080 * 30;

    // Maximum dimensions. Minimum dimensions try to use the same aspect
    // ratio and are generated from the spec.
    Dimensions max_dimensions = {1920, 1080, {kDefaultFrameRate, 1}};

    // Minimum and maximum bitrates. Default values are based on default min and
    // max dimensions, embedders that support different display dimensions
    // should strongly consider setting these fields.
    int min_bit_rate = kDefaultVideoMinBitRate;
    int max_bit_rate = kDefaultVideoMaxBitRate;

    // Max playout delay in milliseconds.
    std::chrono::milliseconds max_delay = kDefaultMaxDelayMs;
  };

  // This struct is used to provide preferences for setting up and running
  // remoting streams. The kludgy properties are based on the current control
  // protocol and allow remoting with current senders. Once libcast has
  // been adopted in Chrome, new, cleaner APIs will be added here to replace
  // these.
  //
  // TODO(issuetracker.google.com/184759616): Chrome should use libcast
  // for mirroring and remoting.
  // TODO(issuetracker.google.com/184429130): the mirroring control
  // protocol needs to be updated to allow more discrete support.
  struct RemotingPreferences {
    // Current remoting senders take an "all or nothing" support for audio
    // codec support. While Opus and AAC support is handled in our Preferences'
    // |audio_codecs| property, support for the following codecs must be
    // enabled or disabled all together:
    // MP3
    // PCM, including Mu-Law, S16BE, S24BE, and ALAW variants
    // Ogg Vorbis
    // FLAC
    // AMR, including narrow band (NB) and wide band (WB) variants
    // GSM Mobile Station (MS)
    // EAC3 (Dolby Digital Plus)
    // ALAC (Apple Lossless)
    // AC-3 (Dolby Digital)
    // These properties are tied directly to what Chrome supports. See:
    // https://source.chromium.org/chromium/chromium/src/+/master:media/base/audio_codecs.h
    bool supports_chrome_audio_codecs = false;

    // Current remoting senders assume that the receiver supports 4K for all
    // video codecs supplied in |video_codecs|, or none of them.
    bool supports_4k = false;
  };

  // Note: embedders are required to implement the following
  // codecs to be Cast V2 compliant: H264, VP8, AAC, Opus.
  struct Preferences {
    Preferences();
    Preferences(std::vector<VideoCodec> video_codecs,
                std::vector<AudioCodec> audio_codecs);
    Preferences(std::vector<VideoCodec> video_codecs,
                std::vector<AudioCodec> audio_codecs,
                std::vector<AudioLimits> audio_limits,
                std::vector<VideoLimits> video_limits,
                std::unique_ptr<Display> description);

    Preferences(Preferences&&) noexcept;
    Preferences(const Preferences&) = delete;
    Preferences& operator=(Preferences&&) noexcept;
    Preferences& operator=(const Preferences&) = delete;

    std::vector<VideoCodec> video_codecs{VideoCodec::kVp8, VideoCodec::kH264};
    std::vector<AudioCodec> audio_codecs{AudioCodec::kOpus, AudioCodec::kAac};

    // Optional limitation fields that help the sender provide a delightful
    // cast experience. Although optional, highly recommended.
    // NOTE: embedders that wish to apply the same limits for all codecs can
    // pass a vector of size 1 with the |applies_to_all_codecs| field set to
    // true.
    std::vector<AudioLimits> audio_limits;
    std::vector<VideoLimits> video_limits;
    std::unique_ptr<Display> display_description;

    // Libcast remoting support is opt-in: embedders wishing to field remoting
    // offers may provide a set of remoting preferences, or leave nullptr for
    // all remoting OFFERs to be rejected in favor of continuing mirroring.
    std::unique_ptr<RemotingPreferences> remoting;
  };

  ReceiverSession(Client* const client,
                  Environment* environment,
                  MessagePort* message_port,
                  Preferences preferences);
  ReceiverSession(const ReceiverSession&) = delete;
  ReceiverSession(ReceiverSession&&) noexcept = delete;
  ReceiverSession& operator=(const ReceiverSession&) = delete;
  ReceiverSession& operator=(ReceiverSession&&) = delete;
  ~ReceiverSession();

  const std::string& session_id() const { return session_id_; }

  // Environment::SocketSubscriber event callbacks.
  void OnSocketReady() override;
  void OnSocketInvalid(Error error) override;

 private:
  // In some cases, such as waiting for the UDP socket to be bound, we
  // may have a pending session that cannot start yet. This class provides
  // all necessary info to instantiate a session.
  struct SessionProperties {
    // The cast mode the OFFER was sent for.
    CastMode mode;

    // The selected audio and video streams from the original OFFER message.
    std::unique_ptr<AudioStream> selected_audio;
    std::unique_ptr<VideoStream> selected_video;

    // The sequence number of the OFFER that produced these properties.
    int sequence_number;

    // To be valid either the audio or video must be selected, and we must
    // have a sequence number we can reference.
    bool IsValid() const;
  };

  // Specific message type handler methods.
  void OnOffer(SenderMessage message);
  void OnCapabilitiesRequest(SenderMessage message);

  // Creates receivers and sends an appropriate Answer message using the
  // session properties.
  void InitializeSession(const SessionProperties& properties);

  // Used by SpawnReceivers to generate a receiver for a specific stream.
  std::unique_ptr<Receiver> ConstructReceiver(const Stream& stream);

  // Creates a set of configured receivers from a given pair of audio and
  // video streams. NOTE: either audio or video may be null, but not both.
  ConfiguredReceivers SpawnReceivers(const SessionProperties& properties);

  // Creates an ANSWER object. Assumes at least one stream is not nullptr.
  Answer ConstructAnswer(const SessionProperties& properties);

  // Creates a ReceiverCapability version 2 object. This will be deprecated
  // as part of https://issuetracker.google.com/184429130.
  ReceiverCapability CreateRemotingCapabilityV2();

  // Handles resetting receivers and notifying the client.
  void ResetReceivers(Client::ReceiversDestroyingReason reason);

  // Sends an error answer reply and notifies the client of the error.
  void SendErrorAnswerReply(int sequence_number, const char* message);

  Client* const client_;
  Environment* const environment_;
  const Preferences preferences_;

  // The sender_id of this session.
  const std::string session_id_;

  // The session messager used for the lifetime of this session.
  ReceiverSessionMessager messager_;

  // The packet router to be used for all Receivers spawned by this session.
  ReceiverPacketRouter packet_router_;

  // Any session pending while the UDP socket is being bound.
  std::unique_ptr<SessionProperties> pending_session_;

  // The negotiated receivers we own, clients are notified of destruction
  // through |Client::OnReceiversDestroying|.
  std::unique_ptr<Receiver> current_audio_receiver_;
  std::unique_ptr<Receiver> current_video_receiver_;
};

}  // namespace cast
}  // namespace openscreen

#endif  // CAST_STREAMING_RECEIVER_SESSION_H_
