// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/receiver_data_session.h"

#include <utility>

#include "util/osp_logging.h"

namespace openscreen::cast {

// static
std::unique_ptr<ReceiverDataSession> ReceiverDataSession::Create(
    DataSessionBase::Delegate& delegate,
    TaskRunner& task_runner,
    std::unique_ptr<WebTransportServer> server,
    const IPEndpoint& local_endpoint) {
  auto data_session =
      std::make_unique<ReceiverDataSession>(delegate, task_runner);
  if (!data_session->Start(std::move(server), local_endpoint)) {
    // Nothing has been reported yet, so it is safe to destroy it here.
    return nullptr;
  }
  return data_session;
}

ReceiverDataSession::ReceiverDataSession(DataSessionBase::Delegate& delegate,
                                         TaskRunner& task_runner)
    : DataSessionBase(delegate, task_runner) {}

ReceiverDataSession::~ReceiverDataSession() {
  // Destroy the DataChannelSession first: it detaches itself from the
  // WebTransportSession, which must happen before Stop() tears down the
  // underlying QUIC session.
  StopChannelSession();
  if (server_) {
    server_->Stop();
  }
}

void ReceiverDataSession::OnSessionCreated(
    std::unique_ptr<WebTransportSession> session) {
  if (channel_session()) {
    session->Close(Error(Error::Code::kSocketInvalidState,
                         "A data session is already active"));
    return;
  }
  StartChannelSession(std::move(session));
}

void ReceiverDataSession::OnServerClosed() {
  NotifyClosed(
      Error(Error::Code::kConnectionFailed, "WebTransport server closed"));
}

bool ReceiverDataSession::Start(std::unique_ptr<WebTransportServer> server,
                                const IPEndpoint& local_endpoint) {
  if (!server) {
    OSP_LOG_ERROR << "Failed to create WebTransport server";
    return false;
  }
  server_ = std::move(server);
  server_->SetDelegate(this);

  port_ = server_->Start(local_endpoint);
  if (port_ == 0) {
    OSP_LOG_ERROR << "Failed to start WebTransport server";
    return false;
  }
  fingerprint_ = server_->GetCertificateFingerprint();
  if (fingerprint_.empty()) {
    // The destructor stops the server.
    OSP_LOG_ERROR
        << "WebTransport server returned empty certificate fingerprint";
    return false;
  }
  return true;
}

}  // namespace openscreen::cast
