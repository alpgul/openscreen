// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_RECEIVER_DATA_SESSION_H_
#define CAST_STREAMING_IMPL_RECEIVER_DATA_SESSION_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <string_view>

#include "cast/streaming/impl/data_session_base.h"
#include "platform/api/task_runner.h"
#include "platform/api/web_transport.h"
#include "platform/base/ip_address.h"

namespace openscreen::cast {

// The receiver side of a negotiated data session. Runs a WebTransportServer
// for the sender to connect to, accepts a single WebTransport session on it,
// and hands the data channels the sender opens to its Delegate. Closure of
// either the server or the accepted session closes the data session.
class ReceiverDataSession final : public DataSessionBase,
                                  public WebTransportServer::Delegate {
 public:
  // Starts `server` on `local_endpoint`. Returns nullptr if `server` is null,
  // fails to start, or has no certificate fingerprint, since the ANSWER can't
  // advertise a transport the sender can't authenticate. `delegate` and
  // `task_runner` must outlive the returned object.
  static std::unique_ptr<ReceiverDataSession> Create(
      DataSessionBase::Delegate& delegate,
      TaskRunner& task_runner,
      std::unique_ptr<WebTransportServer> server,
      const IPEndpoint& local_endpoint);

  ReceiverDataSession(DataSessionBase::Delegate& delegate,
                      TaskRunner& task_runner);
  ReceiverDataSession(const ReceiverDataSession&) = delete;
  ReceiverDataSession& operator=(const ReceiverDataSession&) = delete;
  ~ReceiverDataSession() override;

  // The port and certificate fingerprint to advertise in the ANSWER.
  uint16_t port() const { return port_; }
  std::string_view fingerprint() const { return fingerprint_; }

  // WebTransportServer::Delegate overrides.
  void OnSessionCreated(std::unique_ptr<WebTransportSession> session) override;
  void OnServerClosed() override;

 private:
  bool Start(std::unique_ptr<WebTransportServer> server,
             const IPEndpoint& local_endpoint);

  std::unique_ptr<WebTransportServer> server_;
  uint16_t port_ = 0;
  std::string fingerprint_;
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_RECEIVER_DATA_SESSION_H_
