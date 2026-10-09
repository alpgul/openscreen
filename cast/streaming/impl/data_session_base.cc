// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/data_session_base.h"

#include "util/osp_logging.h"

namespace openscreen::cast {

DataSessionBase::Delegate::~Delegate() = default;

DataSessionBase::DataSessionBase(Delegate& delegate, TaskRunner& task_runner)
    : delegate_(&delegate), task_runner_(task_runner) {}

DataSessionBase::~DataSessionBase() {
  // Subclasses destroy the DataChannelSession before their transport.
  OSP_DCHECK(!channel_session_);
}

void DataSessionBase::StartChannelSession(
    std::unique_ptr<WebTransportSession> session) {
  OSP_CHECK(!channel_session_);
  channel_session_ =
      std::make_unique<DataChannelSession>(std::move(session), this);
  channel_session_->Start();
  // Don't rely on Start() reporting OnSessionReady() for a session that is
  // already connected.
  NotifyOpened();
}

void DataSessionBase::StopChannelSession() {
  channel_session_.reset();
}

void DataSessionBase::OnNewInputChannel(std::unique_ptr<DataChannel> channel) {
  if (delegate_) {
    delegate_->OnNewDataChannel(this, std::move(channel));
  }
}

void DataSessionBase::OnConnectionEstablished() {
  NotifyOpened();
}

void DataSessionBase::OnConnectionClosed(const Error& error) {
  NotifyClosed(error);
}

void DataSessionBase::NotifyOpened() {
  if (opened_ || closed_) {
    return;
  }
  opened_ = true;
  // Called from within the transport, possibly before the subclass's Create()
  // returns, so notify the delegate from a separate task. Skip the
  // notification if this object has since been dropped or has closed.
  task_runner_->PostTask([weak_this = weak_factory_.GetWeakPtr()] {
    if (weak_this && weak_this->delegate_ && !weak_this->closed_) {
      weak_this->delegate_->OnDataSessionOpened(weak_this.get());
    }
  });
}

void DataSessionBase::NotifyClosed(const Error& error) {
  if (closed_) {
    return;
  }
  closed_ = true;
  // Called from within the transport, possibly before the subclass's Create()
  // returns, so notify the delegate from a separate task; it may destroy this
  // object or its own owner from OnDataSessionClosed(). Skip the notification
  // if this object has since been dropped.
  task_runner_->PostTask([weak_this = weak_factory_.GetWeakPtr(), error] {
    if (weak_this && weak_this->delegate_) {
      weak_this->delegate_->OnDataSessionClosed(weak_this.get(), error);
    }
  });
}

}  // namespace openscreen::cast
