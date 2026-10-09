// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_SENDER_DATA_SESSION_H_
#define CAST_STREAMING_IMPL_SENDER_DATA_SESSION_H_

#include <memory>
#include <string_view>

#include "cast/streaming/impl/data_session_base.h"
#include "cast/streaming/public/data_channel.h"
#include "cast/streaming/public/data_session.h"
#include "platform/api/task_runner.h"
#include "platform/api/web_transport.h"
#include "platform/base/error.h"
#include "platform/base/ip_address.h"

namespace openscreen::cast {

// The sender side of a negotiated data session. Connects a WebTransportClient
// to the data transport advertised in the receiver's ANSWER, pinning the
// advertised certificate fingerprint, and opens data channels on it once
// connected. A failed connection is reported as closure, without a preceding
// OnDataSessionOpened().
class SenderDataSession final : public DataSessionBase, public DataSession {
 public:
  // Starts connecting `client` to `https://<remote_endpoint>/cast-mirroring`,
  // accepting only a server certificate matching `fingerprint`. Returns
  // nullptr if `client` is null; otherwise the outcome is reported through
  // `delegate`. `delegate` and `task_runner` must outlive the returned object.
  static std::unique_ptr<SenderDataSession> Create(
      Delegate& delegate,
      TaskRunner& task_runner,
      std::unique_ptr<WebTransportClient> client,
      const IPEndpoint& remote_endpoint,
      std::string_view fingerprint);

  SenderDataSession(Delegate& delegate, TaskRunner& task_runner);
  SenderDataSession(const SenderDataSession&) = delete;
  SenderDataSession& operator=(const SenderDataSession&) = delete;
  ~SenderDataSession() override;

  // DataSession overrides. Succeeds from OnDataSessionOpened() until the data
  // session closes.
  ErrorOr<std::unique_ptr<DataChannel>> CreateDataChannel(
      std::string_view label) override;

 private:
  void Connect(std::unique_ptr<WebTransportClient> client,
               const IPEndpoint& remote_endpoint,
               std::string_view fingerprint);

  // Handlers for the WebTransportClient::Connect() result.
  void OnConnected(std::unique_ptr<WebTransportSession> session);
  void OnConnectionFailed(const Error& error);

  // DataChannelSession::Visitor overrides.
  void OnNewInputChannel(std::unique_ptr<DataChannel> channel) override;

  std::unique_ptr<WebTransportClient> client_;
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_SENDER_DATA_SESSION_H_
