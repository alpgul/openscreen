// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/sender_data_session.h"

#include <format>
#include <string>
#include <utility>

#include "util/osp_logging.h"

namespace openscreen::cast {

// static
std::unique_ptr<SenderDataSession> SenderDataSession::Create(
    Delegate& delegate,
    TaskRunner& task_runner,
    std::unique_ptr<WebTransportClient> client,
    const IPEndpoint& remote_endpoint,
    std::string_view fingerprint) {
  if (!client) {
    OSP_LOG_ERROR << "Failed to create WebTransport client";
    return nullptr;
  }
  auto data_session =
      std::make_unique<SenderDataSession>(delegate, task_runner);
  data_session->Connect(std::move(client), remote_endpoint, fingerprint);
  return data_session;
}

SenderDataSession::SenderDataSession(Delegate& delegate,
                                     TaskRunner& task_runner)
    : DataSessionBase(delegate, task_runner) {}

SenderDataSession::~SenderDataSession() {
  // Destroy the DataChannelSession before `client_`, which owns the underlying
  // connection.
  StopChannelSession();
}

ErrorOr<std::unique_ptr<DataChannel>> SenderDataSession::CreateDataChannel(
    std::string_view label) {
  if (!channel_session() || closed()) {
    return Error(Error::Code::kConnectionFailed,
                 "WebTransport session not opened");
  }
  std::unique_ptr<DataChannel> channel =
      channel_session()->CreateOutgoingDataChannel(label);
  if (!channel) {
    return Error(Error::Code::kConnectionFailed,
                 "Failed to create data channel");
  }
  return channel;
}

void SenderDataSession::OnNewInputChannel(
    std::unique_ptr<DataChannel> channel) {
  OSP_LOG_WARN << "Sender received unexpected incoming data channel";
}

void SenderDataSession::Connect(std::unique_ptr<WebTransportClient> client,
                                const IPEndpoint& remote_endpoint,
                                std::string_view fingerprint) {
  client_ = std::move(client);

  WebTransportOptions options;
  options.server_certificate_hashes.push_back(std::string(fingerprint));

  const std::string url =
      std::format("https://{}/cast-mirroring", remote_endpoint.ToString());

  // `client_` is owned by this object, so the callback cannot outlive it.
  client_->Connect(
      url, options,
      [this](ErrorOr<std::unique_ptr<WebTransportSession>> session) {
        if (session.is_value()) {
          OnConnected(std::move(session.value()));
        } else {
          OnConnectionFailed(session.error());
        }
      });
}

void SenderDataSession::OnConnected(
    std::unique_ptr<WebTransportSession> session) {
  StartChannelSession(std::move(session));
}

void SenderDataSession::OnConnectionFailed(const Error& error) {
  OSP_LOG_ERROR << "WebTransport connection failed: " << error;
  NotifyClosed(error);
}

}  // namespace openscreen::cast
