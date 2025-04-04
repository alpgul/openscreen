// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OSP_PUBLIC_PRESENTATION_PRESENTATION_CONNECTION_H_
#define OSP_PUBLIC_PRESENTATION_PRESENTATION_CONNECTION_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "osp/public/message_demuxer.h"
#include "osp/public/presentation/presentation_common.h"
#include "platform/api/time.h"
#include "platform/base/error.h"
#include "platform/base/ip_address.h"
#include "platform/base/macros.h"
#include "util/osp_logging.h"

namespace openscreen::osp {

class ProtocolConnection;

class Connection {
 public:
  enum class CloseReason {
    kClosed = 0,
    kDiscarded,
    kError,
  };

  enum class State {
    // The library is currently attempting to connect to the presentation.
    kConnecting,
    // The connection to the presentation is open and communication is possible.
    kConnected,
    // The connection is closed or could not be opened.  No communication is
    // possible but it may be possible to reopen the connection via
    // ReconnectPresentation.
    kClosed,
    // The connection is closed and the receiver has been terminated.
    kTerminated,
  };

  // An object to receive callbacks related to a single Connection. Embedder can
  // link it's presentation connection functionality through this interface.
  class Delegate {
   public:
    Delegate();
    Delegate(const Delegate&) = delete;
    Delegate& operator=(const Delegate&) = delete;
    Delegate(Delegate&&) noexcept = delete;
    Delegate& operator=(Delegate&&) noexcept = delete;
    virtual ~Delegate();

    // State changes.
    virtual void OnConnected() = 0;

    // Explicit close by other endpoint.
    virtual void OnClosedByRemote() = 0;

    // Closed because the script connection object was discarded.
    virtual void OnDiscarded() = 0;

    // Closed because of an error.
    virtual void OnError(const std::string_view message) = 0;

    // Terminated through a different connection.
    virtual void OnTerminated() = 0;

    // A UTF-8 string message was received.
    virtual void OnStringMessage(const std::string_view message) = 0;

    // A binary message was received.
    virtual void OnBinaryMessage(const std::vector<uint8_t>& data) = 0;
  };

  // Allows different close, termination, and destruction behavior for both
  // possible parents: controller and receiver.
  class Controller {
   public:
    Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    Controller(Controller&&) noexcept = delete;
    Controller& operator=(Controller&&) noexcept = delete;
    virtual ~Controller();

    virtual Error CloseConnection(Connection* connection,
                                  CloseReason reason) = 0;
    // Called by the embedder to report that a presentation has been
    // terminated.
    virtual Error OnPresentationTerminated(const std::string& presentation_id,
                                           TerminationSource source,
                                           TerminationReason reason) = 0;
    virtual void OnConnectionDestroyed(Connection* connection) = 0;
  };

  struct PresentationInfo {
    std::string id;
    std::string url;
  };

  // Constructs a new connection using `delegate` for callbacks.
  Connection(const PresentationInfo& info,
             Delegate* delegate,
             Controller* controller);
  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) noexcept = delete;
  Connection& operator=(Connection&&) noexcept = delete;
  ~Connection();

  // Returns the ID and URL of this presentation.
  const PresentationInfo& presentation_info() const {
    return presentation_info_;
  }

  State state() const { return state_; }

  ProtocolConnection* protocol_connection() const {
    return protocol_connection_.get();
  }

  // These methods should only be called when we are connected.
  uint64_t instance_id() const {
    OSP_CHECK(instance_id_);
    return instance_id_.value();
  }
  uint64_t connection_id() const {
    OSP_CHECK(connection_id_);
    return connection_id_.value();
  }

  // Sends a UTF-8 string message.
  Error SendString(std::string_view message);

  // Sends a binary message.
  Error SendBinary(std::vector<uint8_t>&& data);

  // Closes the connection.  This can be based on an explicit request from the
  // embedder or because the connection object is being discarded (page
  // navigated, object GC'd, etc.).
  Error Close(CloseReason reason);

  // Terminates the presentation associated with this connection.
  void Terminate(TerminationSource source, TerminationReason reason);

  void OnConnecting();

  // Called by the receiver when the OnPresentationStarted logic happens. This
  // notifies the delegate and updates our internal stream and ids.
  void OnConnected(uint64_t connection_id,
                   uint64_t instance_id,
                   std::unique_ptr<ProtocolConnection> stream);

  void OnClosedByError(const Error& cause);
  void OnClosedByRemote();
  void OnTerminated();

  Delegate* delegate() { return delegate_; }

 private:
  // Helper method that handles closing down our internal state.
  // Returns whether or not the connection state changed (and thus
  // whether or not delegates should be informed).
  bool OnClosed();

  PresentationInfo presentation_info_;
  State state_ = State::kConnecting;
  Delegate* delegate_ = nullptr;
  Controller* controller_ = nullptr;
  std::optional<uint64_t> connection_id_;
  std::optional<uint64_t> instance_id_;
  std::unique_ptr<ProtocolConnection> protocol_connection_;
};

class ConnectionManager final : public MessageDemuxer::MessageCallback {
 public:
  explicit ConnectionManager(MessageDemuxer& demuxer);
  ConnectionManager(const ConnectionManager&) = delete;
  ConnectionManager& operator=(const ConnectionManager&) = delete;
  ConnectionManager(ConnectionManager&&) noexcept = delete;
  ConnectionManager& operator=(ConnectionManager&&) noexcept = delete;
  ~ConnectionManager() override;

  void AddConnection(Connection* connection);
  void RemoveConnection(Connection* connection);

  // MessasgeDemuxer::MessageCallback overrides.
  ErrorOr<size_t> OnStreamMessage(uint64_t instance_id,
                                  uint64_t connection_id,
                                  msgs::Type message_type,
                                  const uint8_t* buffer,
                                  size_t buffer_size,
                                  Clock::time_point now) override;

  Connection* GetConnection(uint64_t connection_id);
  size_t ConnectionCount() const { return connections_.size(); }

 private:
  // TODO(btolsch): Connection IDs were changed to be per-instance, but this
  // table then needs to be <instance id, connection id> since connection id
  // is still not unique globally.
  std::map<uint64_t, Connection*> connections_;

  MessageDemuxer::MessageWatch message_watch_;
  MessageDemuxer::MessageWatch close_event_watch_;
};

}  // namespace openscreen::osp

#endif  // OSP_PUBLIC_PRESENTATION_PRESENTATION_CONNECTION_H_
