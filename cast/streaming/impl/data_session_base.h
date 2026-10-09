// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CAST_STREAMING_IMPL_DATA_SESSION_BASE_H_
#define CAST_STREAMING_IMPL_DATA_SESSION_BASE_H_

#include <memory>
#include <type_traits>
#include <utility>

#include "cast/streaming/impl/data_channel_session.h"
#include "cast/streaming/public/data_channel.h"
#include "platform/api/task_runner.h"
#include "platform/api/web_transport.h"
#include "platform/base/error.h"
#include "util/raw_ptr.h"
#include "util/raw_ref.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {

// Shared implementation of both ends of a negotiated data session. Runs a
// DataChannelSession over a WebTransport session and reports its lifecycle to
// a Delegate. Subclasses only set up the WebTransport session:
// ReceiverDataSession accepts it on a WebTransportServer, and
// SenderDataSession connects it with a WebTransportClient.
class DataSessionBase : public DataChannelSession::Visitor {
 public:
  // Not called for a data session that has been passed to Reset().
  class Delegate {
   public:
    // Called at most once, from its own task, when the WebTransport session
    // is ready. Not called if the data session closes first. The
    // implementation may destroy `data_session`, or its own owner, from here.
    virtual void OnDataSessionOpened(DataSessionBase* data_session) {}

    // Called when the peer opens a new data channel. Runs synchronously from
    // within the transport, so the delegate must be set on `channel` before
    // returning (see DataChannelSession::Visitor), and the implementation
    // must not destroy `data_session` from here. The default implementation
    // drops the channel.
    virtual void OnNewDataChannel(DataSessionBase* data_session,
                                  std::unique_ptr<DataChannel> channel) {}

    // Called at most once, from its own task, when the connection fails or
    // the session closes. `error` is ok if the peer closed the session
    // gracefully. `data_session` is no longer usable, and the implementation
    // may destroy it, or its own owner, from here.
    virtual void OnDataSessionClosed(DataSessionBase* data_session,
                                     const Error& error) = 0;

   protected:
    virtual ~Delegate();
  };

  // Drops `data_session`, if any. It may be on the call stack (e.g. when the
  // owner renegotiates from a DataChannel callback), so it stops reporting to
  // its Delegate immediately and is destroyed from a separate task.
  // `data_session` is null on return.
  template <typename T>
  static void Reset(std::unique_ptr<T>& data_session) {
    static_assert(std::is_base_of_v<DataSessionBase, T>,
                  "Reset() only accepts data sessions");
    if (!data_session) {
      return;
    }
    DataSessionBase& base = *data_session;
    base.delegate_ = nullptr;
    TaskRunner& task_runner = *base.task_runner_;
    task_runner.PostTask(
        [doomed = std::move(data_session)]() mutable { doomed.reset(); });
  }

  DataSessionBase(const DataSessionBase&) = delete;
  DataSessionBase& operator=(const DataSessionBase&) = delete;
  ~DataSessionBase() override;

 protected:
  // `delegate` and `task_runner` must outlive this object.
  DataSessionBase(Delegate& delegate, TaskRunner& task_runner);

  // Runs a DataChannelSession over `session`, which must already be
  // connected, and reports this data session as opened.
  void StartChannelSession(std::unique_ptr<WebTransportSession> session);

  // Destroys the DataChannelSession, which detaches it from the
  // WebTransportSession. Subclasses must call this from their destructor,
  // before tearing down the transport that owns the underlying connection.
  void StopChannelSession();

  // Reports closure to the delegate. The transport may report closure through
  // more than one path, so only the first call is reported.
  void NotifyClosed(const Error& error);

  // Null until StartChannelSession() is called.
  DataChannelSession* channel_session() { return channel_session_.get(); }

  // True once closure has been reported by the transport.
  bool closed() const { return closed_; }

  // DataChannelSession::Visitor overrides.
  void OnNewInputChannel(std::unique_ptr<DataChannel> channel) override;
  void OnConnectionEstablished() override;
  void OnConnectionClosed(const Error& error) override;

 private:
  // Both StartChannelSession() and the transport may report that the session
  // is ready; only notify the delegate once, and never after closure.
  void NotifyOpened();

  // Null once detached by Reset().
  raw_ptr<Delegate> delegate_ = nullptr;
  const raw_ref<TaskRunner> task_runner_;
  std::unique_ptr<DataChannelSession> channel_session_;
  bool opened_ = false;
  bool closed_ = false;

  WeakPtrFactory<DataSessionBase> weak_factory_{this};
};

}  // namespace openscreen::cast

#endif  // CAST_STREAMING_IMPL_DATA_SESSION_BASE_H_
