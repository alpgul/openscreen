// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/sender_data_session.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "cast/streaming/public/data_channel.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "platform/api/web_transport.h"
#include "platform/base/ip_address.h"
#include "platform/test/fake_clock.h"
#include "platform/test/fake_task_runner.h"
#include "util/weak_ptr.h"

namespace openscreen::cast {
namespace {

using ::testing::_;
using ::testing::StrictMock;

MATCHER_P(CodeEquals, code, "Checks error codes but not messages.") {
  return arg.code() == code;
}

class MockDelegate : public DataSessionBase::Delegate {
 public:
  MOCK_METHOD(void,
              OnDataSessionOpened,
              (DataSessionBase * data_session),
              (override));
  MOCK_METHOD(void,
              OnDataSessionClosed,
              (DataSessionBase * data_session, const Error& error),
              (override));
};

class FakeWebTransportStream : public WebTransportStream {
 public:
  void SetDelegate(Delegate* delegate) override { delegate_ = delegate; }
  bool Write(ByteView data) override { return true; }
  void Close() override {}

 private:
  raw_ptr<Delegate> delegate_ = nullptr;
};

class FakeWebTransportSession : public WebTransportSession {
 public:
  void SetDelegate(Delegate* delegate) override { delegate_ = delegate; }
  ErrorOr<std::unique_ptr<WebTransportStream>> CreateOutgoingStream() override {
    return std::unique_ptr<WebTransportStream>(
        std::make_unique<FakeWebTransportStream>());
  }
  void Close(const Error& error) override { SimulateClosed(error); }

  // Like a real transport, keeps using its own state after notifying the
  // delegate, so destroying this session from inside the notification is
  // caught by ASAN.
  void SimulateClosed(const Error& error) {
    if (delegate_) {
      delegate_->OnSessionClosed(this, error);
    }
    ++close_notification_count_;
  }

  void SimulateReady() {
    ASSERT_TRUE(delegate_);
    delegate_->OnSessionReady(this);
  }

  WeakPtr<FakeWebTransportSession> GetWeakPtr() const {
    return weak_factory_.GetWeakPtr();
  }

 private:
  raw_ptr<Delegate> delegate_ = nullptr;
  int close_notification_count_ = 0;
  WeakPtrFactory<FakeWebTransportSession> weak_factory_{this};
};

// Completes Connect() synchronously with a new session or `connect_error_`,
// unless `defer_connect_` is set, in which case CompleteConnect() must be
// called.
class FakeWebTransportClient : public WebTransportClient {
 public:
  void Connect(std::string_view url,
               const WebTransportOptions& options,
               ConnectCallback callback) override {
    last_url_ = std::string(url);
    last_options_ = options;
    callback_ = std::move(callback);
    if (!defer_connect_) {
      CompleteConnect();
    }
  }

  void CompleteConnect() {
    ConnectCallback callback = std::move(callback_);
    if (connect_error_.has_value()) {
      std::move(callback)(connect_error_.value());
      return;
    }
    auto session = std::make_unique<FakeWebTransportSession>();
    last_session_ = session->GetWeakPtr();
    std::move(callback)(
        std::unique_ptr<WebTransportSession>(std::move(session)));
  }

  WeakPtr<FakeWebTransportClient> GetWeakPtr() const {
    return weak_factory_.GetWeakPtr();
  }

  bool defer_connect_ = false;
  std::optional<Error> connect_error_;
  std::string last_url_;
  WebTransportOptions last_options_;
  WeakPtr<FakeWebTransportSession> last_session_;

 private:
  ConnectCallback callback_;
  WeakPtrFactory<FakeWebTransportClient> weak_factory_{this};
};

// Runs `on_closed_`, if set, when the channel reports that it closed.
class FakeDataChannelDelegate : public DataChannel::Delegate {
 public:
  void OnMessage(ByteView data) override {}
  void OnStateChange(DataChannel::State state) override {
    if (state == DataChannel::State::kClosed && on_closed_) {
      on_closed_();
    }
  }
  void OnWriteReady() override {}

  std::function<void()> on_closed_;
};

constexpr char kFingerprint[] = "fingerprint";

}  // namespace

class SenderDataSessionTest : public ::testing::Test {
 public:
  SenderDataSessionTest() : clock_(Clock::time_point{}), task_runner_(clock_) {}

 protected:
  // Creates a data session backed by a FakeWebTransportClient, which is
  // exposed through `client_`.
  std::unique_ptr<SenderDataSession> CreateDataSession(
      bool defer_connect = false,
      std::optional<Error> connect_error = std::nullopt) {
    auto client = std::make_unique<FakeWebTransportClient>();
    client->defer_connect_ = defer_connect;
    client->connect_error_ = std::move(connect_error);
    client_ = client->GetWeakPtr();
    return SenderDataSession::Create(delegate_, task_runner_, std::move(client),
                                     remote_endpoint_, kFingerprint);
  }

  const IPEndpoint remote_endpoint_{{192, 168, 1, 2}, 33534};
  StrictMock<MockDelegate> delegate_;
  WeakPtr<FakeWebTransportClient> client_;
  FakeClock clock_;
  // Declared last so that tasks still pending at the end of a test are
  // destroyed while the objects above are alive.
  FakeTaskRunner task_runner_;
};

TEST_F(SenderDataSessionTest, CreateFailsWithoutClient) {
  EXPECT_FALSE(SenderDataSession::Create(delegate_, task_runner_, nullptr,
                                         remote_endpoint_, kFingerprint));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(SenderDataSessionTest, ConnectsWithPinnedFingerprint) {
  std::unique_ptr<SenderDataSession> data_session =
      CreateDataSession(/*defer_connect=*/true);
  ASSERT_TRUE(data_session);
  ASSERT_TRUE(client_);
  EXPECT_EQ("https://192.168.1.2:33534/cast-mirroring", client_->last_url_);
  ASSERT_EQ(1u, client_->last_options_.server_certificate_hashes.size());
  EXPECT_EQ(kFingerprint, client_->last_options_.server_certificate_hashes[0]);
}

TEST_F(SenderDataSessionTest, ReportsOpenedFromItsOwnTask) {
  // The fake connects synchronously, but the opening is only reported from a
  // separate task, after Create() has returned.
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);

  std::unique_ptr<DataChannel> channel;
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()))
      .WillOnce([&](DataSessionBase*) {
        auto channel_or_error = data_session->CreateDataChannel("label");
        ASSERT_TRUE(channel_or_error.is_value());
        channel = std::move(channel_or_error.value());
      });
  task_runner_.RunTasksUntilIdle();
  ASSERT_TRUE(channel);
  EXPECT_EQ("label", channel->label());
}

TEST_F(SenderDataSessionTest, ReportsOpenedOnlyOnce) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  ASSERT_TRUE(client_->last_session_);

  // OnSessionReady() after the connection completed is not reported again.
  client_->last_session_->SimulateReady();
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(SenderDataSessionTest, CreateDataChannelFailsBeforeOpened) {
  std::unique_ptr<SenderDataSession> data_session =
      CreateDataSession(/*defer_connect=*/true);
  ASSERT_TRUE(data_session);

  auto channel_or_error = data_session->CreateDataChannel("label");
  ASSERT_TRUE(channel_or_error.is_error());
  EXPECT_EQ(Error::Code::kConnectionFailed, channel_or_error.error().code());

  client_->CompleteConnect();
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();
  EXPECT_TRUE(data_session->CreateDataChannel("label").is_value());
}

TEST_F(SenderDataSessionTest, ReportsSynchronousConnectFailure) {
  // Even a synchronous failure is reported from a separate task, and
  // OnDataSessionOpened() is never called.
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession(
      /*defer_connect=*/false,
      Error(Error::Code::kSocketConnectFailure, "connect failed"));
  ASSERT_TRUE(data_session);

  EXPECT_CALL(delegate_, OnDataSessionClosed(
                             data_session.get(),
                             CodeEquals(Error::Code::kSocketConnectFailure)));
  task_runner_.RunTasksUntilIdle();
  EXPECT_TRUE(data_session->CreateDataChannel("label").is_error());
}

TEST_F(SenderDataSessionTest, ReportsAsynchronousConnectFailure) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession(
      /*defer_connect=*/true,
      Error(Error::Code::kSocketConnectFailure, "connect failed"));
  ASSERT_TRUE(data_session);
  task_runner_.RunTasksUntilIdle();

  client_->CompleteConnect();
  EXPECT_CALL(delegate_, OnDataSessionClosed(
                             data_session.get(),
                             CodeEquals(Error::Code::kSocketConnectFailure)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(SenderDataSessionTest, ReportsClosureAfterOpened) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();

  // Nothing is reported from inside the transport's notification.
  client_->last_session_->SimulateClosed(
      Error(Error::Code::kConnectionFailed, "reset"));
  EXPECT_CALL(delegate_,
              OnDataSessionClosed(data_session.get(),
                                  CodeEquals(Error::Code::kConnectionFailed)));
  task_runner_.RunTasksUntilIdle();

  auto channel_or_error = data_session->CreateDataChannel("label");
  ASSERT_TRUE(channel_or_error.is_error());
  EXPECT_EQ(Error::Code::kConnectionFailed, channel_or_error.error().code());
}

TEST_F(SenderDataSessionTest, ReportsGracefulClosure) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();

  client_->last_session_->SimulateClosed(Error::None());
  EXPECT_CALL(delegate_, OnDataSessionClosed(data_session.get(),
                                             CodeEquals(Error::Code::kNone)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(SenderDataSessionTest, SkipsOpenedIfClosedFirst) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);

  // The session closes before the opening task runs, so only the closure is
  // reported.
  client_->last_session_->SimulateClosed(Error::None());
  client_->last_session_->SimulateClosed(Error::None());
  EXPECT_CALL(delegate_, OnDataSessionClosed(data_session.get(),
                                             CodeEquals(Error::Code::kNone)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(SenderDataSessionTest, DelegateMayDestroyFromOnDataSessionOpened) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);

  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()))
      .WillOnce([&](DataSessionBase*) { data_session.reset(); });
  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(client_);
}

TEST_F(SenderDataSessionTest, DelegateMayDestroyFromOnDataSessionClosed) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();

  client_->last_session_->SimulateClosed(Error::None());
  EXPECT_CALL(delegate_, OnDataSessionClosed(data_session.get(), _))
      .WillOnce([&](DataSessionBase*, const Error&) { data_session.reset(); });
  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(client_);
}

TEST_F(SenderDataSessionTest, ResetDefersDestructionAndStopsReporting) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  WeakPtr<FakeWebTransportSession> session = client_->last_session_;

  // Neither the pending opening nor a later closure is reported.
  SenderDataSession::Reset(data_session);
  EXPECT_FALSE(data_session);
  ASSERT_TRUE(session);
  session->SimulateClosed(Error(Error::Code::kConnectionFailed, "reset"));
  EXPECT_TRUE(client_);

  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(client_);
  EXPECT_FALSE(session);
}

TEST_F(SenderDataSessionTest, ResetFromDataChannelCallbackIsSafe) {
  std::unique_ptr<SenderDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();

  // Declared before `channel` so that it outlives it.
  FakeDataChannelDelegate channel_delegate;
  channel_delegate.on_closed_ = [&] { SenderDataSession::Reset(data_session); };
  auto channel_or_error = data_session->CreateDataChannel("label");
  ASSERT_TRUE(channel_or_error.is_value());
  std::unique_ptr<DataChannel> channel = std::move(channel_or_error.value());
  channel->SetDelegate(&channel_delegate);

  // The channel reports its closure from inside the data session's closure
  // handling, so Reset() must not destroy the data session synchronously.
  WeakPtr<FakeWebTransportSession> session = client_->last_session_;
  session->SimulateClosed(Error::None());
  EXPECT_FALSE(data_session);
  EXPECT_TRUE(session);
  EXPECT_EQ(DataChannel::State::kClosed, channel->state());

  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(session);
}

TEST_F(SenderDataSessionTest, ResetOfNullIsNoOp) {
  std::unique_ptr<SenderDataSession> data_session;
  SenderDataSession::Reset(data_session);
  EXPECT_FALSE(data_session);
}

}  // namespace openscreen::cast
