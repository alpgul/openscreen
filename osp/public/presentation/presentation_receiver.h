// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OSP_PUBLIC_PRESENTATION_PRESENTATION_RECEIVER_H_
#define OSP_PUBLIC_PRESENTATION_PRESENTATION_RECEIVER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "osp/msgs/osp_messages.h"
#include "osp/public/message_demuxer.h"
#include "osp/public/presentation/presentation_common.h"
#include "osp/public/presentation/presentation_connection.h"

namespace openscreen::osp {

class ReceiverDelegate {
 public:
  ReceiverDelegate();
  ReceiverDelegate(const ReceiverDelegate&) = delete;
  ReceiverDelegate& operator=(const ReceiverDelegate&) = delete;
  ReceiverDelegate(ReceiverDelegate&&) noexcept = delete;
  ReceiverDelegate& operator=(ReceiverDelegate&&) noexcept = delete;
  virtual ~ReceiverDelegate();

  // Called when the availability (compatible, not compatible, or invalid)
  // for specific URLs is needed to be supplied by the delegate.
  // See "#presentation-protocol" spec section.
  // Returns a list of url availabilities.
  virtual std::vector<msgs::UrlAvailability> OnUrlAvailabilityRequest(
      uint64_t watch_id,
      uint64_t watch_duration,
      std::vector<std::string> urls) = 0;

  // Called when a new presentation is requested by a controller.  This should
  // return true if the presentation was accepted, false otherwise.
  virtual bool StartPresentation(
      const Connection::PresentationInfo& info,
      uint64_t source_id,
      const std::vector<msgs::HttpHeader>& http_headers) = 0;

  // Called when the receiver wants to actually connection to the presentation.
  // Should return true if the connection was successful, false otherwise.
  virtual bool ConnectToPresentation(uint64_t request_id,
                                     const std::string& id,
                                     uint64_t source_id) = 0;

  // Called when a presentation is requested to be terminated by a controller.
  virtual void TerminatePresentation(const std::string& id,
                                     TerminationSource source,
                                     TerminationReason reason) = 0;
};

class Receiver final : public MessageDemuxer::MessageCallback,
                       public Connection::Controller {
 public:
  Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;
  Receiver(Receiver&&) noexcept = delete;
  Receiver& operator=(Receiver&&) noexcept = delete;
  ~Receiver() override;

  // Connection::Controller overrides.
  Error CloseConnection(Connection* connection,
                        Connection::CloseReason reason) override;
  Error OnPresentationTerminated(const std::string& presentation_id,
                                 TerminationSource source,
                                 TerminationReason reason) override;
  void OnConnectionDestroyed(Connection* connection) override;

  // MessageDemuxer::MessageCallback overrides.
  ErrorOr<size_t> OnStreamMessage(uint64_t instance_id,
                                  uint64_t connection_id,
                                  msgs::Type message_type,
                                  const uint8_t* buffer,
                                  size_t buffer_size,
                                  Clock::time_point now) override;

  void Init();
  void Deinit();

  // Sets the object to call when a new receiver connection is available.
  // `delegate` must either outlive PresentationReceiver or live until a new
  // delegate (possibly nullptr) is set.  Setting the delegate to nullptr will
  // automatically ignore all future receiver requests.
  void SetReceiverDelegate(ReceiverDelegate* delegate);

  // Called by the embedder to report its response to StartPresentation.
  Error OnPresentationStarted(const std::string& presentation_id,
                              Connection* connection,
                              ResponseResult result);

  Error OnConnectionCreated(uint64_t request_id,
                            Connection* connection,
                            ResponseResult result);

 private:
  struct QueuedResponse {
    enum class Type { kInitiation, kConnection };

    Type type;
    uint64_t request_id = 0u;
    uint64_t connection_id = 0u;
    uint64_t instance_id = 0u;
  };

  struct Presentation {
    uint64_t instance_id = 0u;
    MessageDemuxer::MessageWatch terminate_watch;
    uint64_t terminate_request_id = 0u;
    std::vector<Connection*> connections;
  };

  using QueuedResponseIterator = std::vector<QueuedResponse>::const_iterator;

  void DeleteQueuedResponse(const std::string& presentation_id,
                            QueuedResponseIterator response);
  ErrorOr<QueuedResponseIterator> GetQueuedResponse(
      const std::string& presentation_id,
      uint64_t request_id) const;

  uint64_t GetNextConnectionId();

  ReceiverDelegate* delegate_ = nullptr;

  // TODO(jophba): scope requests by endpoint, not presentation. This doesn't
  // work properly for multiple controllers.
  std::map<std::string, std::vector<QueuedResponse>> queued_responses_by_id_;

  // Presentations are added when the embedder starts the presentation,
  // and ended when a new receiver delegate is set or when
  // a presentation is called to be terminated (OnPresentationTerminated).
  std::map<std::string, Presentation> started_presentations_by_id_;

  std::unique_ptr<ConnectionManager> connection_manager_;

  MessageDemuxer::MessageWatch availability_watch_;
  MessageDemuxer::MessageWatch initiation_watch_;
  MessageDemuxer::MessageWatch connection_watch_;
};

}  // namespace openscreen::osp

#endif  // OSP_PUBLIC_PRESENTATION_PRESENTATION_RECEIVER_H_
