// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/streaming/impl/receiver_data_session.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
              OnNewDataChannel,
              (DataSessionBase * data_session,
               std::unique_ptr<DataChannel> channel),
              (override));
  MOCK_METHOD(void,
              OnDataSessionClosed,
              (DataSessionBase * data_session, const Error& error),
              (override));
};

class FakeWebTransportStream : public WebTransportStream {
 public:
  explicit FakeWebTransportStream(std::vector<uint8_t> incoming)
      : incoming_(std::move(incoming)) {}

  // Delivers the incoming bytes as soon as a delegate is set.
  void SetDelegate(Delegate* delegate) override {
    delegate_ = delegate;
    if (delegate_ && !incoming_.empty()) {
      std::vector<uint8_t> data = std::move(incoming_);
      incoming_.clear();
      delegate_->OnRead(this, ByteView(data.data(), data.size()));
    }
  }
  bool Write(ByteView data) override { return true; }
  void Close() override {}

 private:
  raw_ptr<Delegate> delegate_ = nullptr;
  std::vector<uint8_t> incoming_;
};

class FakeWebTransportSession : public WebTransportSession {
 public:
  // If set, `close_error` records the error passed to Close(); it must outlive
  // this session.
  explicit FakeWebTransportSession(std::optional<Error>* close_error = nullptr)
      : close_error_(close_error) {}

  void SetDelegate(Delegate* delegate) override { delegate_ = delegate; }
  ErrorOr<std::unique_ptr<WebTransportStream>> CreateOutgoingStream() override {
    return Error(Error::Code::kNotImplemented);
  }
  void Close(const Error& error) override {
    if (close_error_) {
      *close_error_ = error;
    }
  }

  // Like a real transport, keeps using its own state after notifying the
  // delegate, so destroying this session from inside the notification is
  // caught by ASAN.
  void SimulateClosed(const Error& error) {
    if (delegate_) {
      delegate_->OnSessionClosed(this, error);
    }
    ++close_notification_count_;
  }

  void AddIncomingStream(std::unique_ptr<WebTransportStream> stream) {
    WebTransportStream* ptr = stream.get();
    incoming_streams_.push_back(std::move(stream));
    ASSERT_TRUE(delegate_);
    delegate_->OnIncomingStream(ptr);
  }

  WeakPtr<FakeWebTransportSession> GetWeakPtr() const {
    return weak_factory_.GetWeakPtr();
  }

 private:
  raw_ptr<std::optional<Error>> close_error_ = nullptr;
  raw_ptr<Delegate> delegate_ = nullptr;
  int close_notification_count_ = 0;
  std::vector<std::unique_ptr<WebTransportStream>> incoming_streams_;
  WeakPtrFactory<FakeWebTransportSession> weak_factory_{this};
};

class FakeWebTransportServer : public WebTransportServer {
 public:
  // `stop_count` counts calls to Stop(); it must outlive this server.
  FakeWebTransportServer(uint16_t port,
                         std::string fingerprint,
                         int* stop_count)
      : port_(port),
        fingerprint_(std::move(fingerprint)),
        stop_count_(stop_count) {}

  void SetDelegate(Delegate* delegate) override { delegate_ = delegate; }
  uint16_t Start(const IPEndpoint& local_endpoint) override {
    started_endpoint_ = local_endpoint;
    return port_;
  }
  void Stop() override { ++*stop_count_; }
  std::string GetCertificateFingerprint() const override {
    return fingerprint_;
  }

  // Simulates a sender connecting to this server.
  void AcceptSession(std::unique_ptr<WebTransportSession> session) {
    ASSERT_TRUE(delegate_);
    delegate_->OnSessionCreated(std::move(session));
  }

  void SimulateServerClosed() {
    ASSERT_TRUE(delegate_);
    delegate_->OnServerClosed();
  }

  const std::optional<IPEndpoint>& started_endpoint() const {
    return started_endpoint_;
  }

  WeakPtr<FakeWebTransportServer> GetWeakPtr() const {
    return weak_factory_.GetWeakPtr();
  }

 private:
  const uint16_t port_;
  const std::string fingerprint_;
  raw_ptr<int> stop_count_ = nullptr;
  raw_ptr<Delegate> delegate_ = nullptr;
  std::optional<IPEndpoint> started_endpoint_;
  WeakPtrFactory<FakeWebTransportServer> weak_factory_{this};
};

}  // namespace

class ReceiverDataSessionTest : public ::testing::Test {
 public:
  ReceiverDataSessionTest()
      : clock_(Clock::time_point{}), task_runner_(clock_) {}

 protected:
  // Creates a data session backed by a FakeWebTransportServer, which is
  // exposed through `server_`.
  std::unique_ptr<ReceiverDataSession> CreateDataSession(
      uint16_t port = 33534,
      std::string fingerprint = "fingerprint") {
    auto server = std::make_unique<FakeWebTransportServer>(
        port, std::move(fingerprint), &stop_count_);
    server_ = server->GetWeakPtr();
    return ReceiverDataSession::Create(delegate_, task_runner_,
                                       std::move(server), local_endpoint_);
  }

  // Creates a data session and accepts a session on it, which is exposed
  // through `session_`.
  std::unique_ptr<ReceiverDataSession> CreateConnectedDataSession() {
    std::unique_ptr<ReceiverDataSession> data_session = CreateDataSession();
    EXPECT_TRUE(data_session);
    auto session = std::make_unique<FakeWebTransportSession>();
    session_ = session->GetWeakPtr();
    server_->AcceptSession(std::move(session));
    return data_session;
  }

  const IPEndpoint local_endpoint_{{127, 0, 0, 1}, 0};
  StrictMock<MockDelegate> delegate_;
  int stop_count_ = 0;
  WeakPtr<FakeWebTransportServer> server_;
  WeakPtr<FakeWebTransportSession> session_;
  FakeClock clock_;
  // Declared last so that tasks still pending at the end of a test are
  // destroyed while the objects above are alive.
  FakeTaskRunner task_runner_;
};

TEST_F(ReceiverDataSessionTest, CreateFailsWithoutServer) {
  EXPECT_FALSE(ReceiverDataSession::Create(delegate_, task_runner_, nullptr,
                                           local_endpoint_));
}

TEST_F(ReceiverDataSessionTest, CreateFailsIfServerDoesNotStart) {
  EXPECT_FALSE(CreateDataSession(/*port=*/0));
  EXPECT_FALSE(server_);
  EXPECT_EQ(1, stop_count_);
}

TEST_F(ReceiverDataSessionTest, CreateFailsWithEmptyFingerprint) {
  EXPECT_FALSE(CreateDataSession(33534, /*fingerprint=*/""));
  EXPECT_FALSE(server_);
  EXPECT_EQ(1, stop_count_);
}

TEST_F(ReceiverDataSessionTest, ExposesPortAndFingerprint) {
  std::unique_ptr<ReceiverDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  EXPECT_EQ(33534, data_session->port());
  EXPECT_EQ("fingerprint", data_session->fingerprint());
  ASSERT_TRUE(server_);
  EXPECT_EQ(local_endpoint_, server_->started_endpoint());
}

TEST_F(ReceiverDataSessionTest, DestructorStopsServer) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);
  data_session.reset();
  EXPECT_FALSE(server_);
  EXPECT_FALSE(session_);
  EXPECT_EQ(1, stop_count_);
}

TEST_F(ReceiverDataSessionTest, ForwardsIncomingChannelsToDelegate) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  // Open Frame: Type (0x01) + Length (0x0005) + "label".
  EXPECT_CALL(delegate_, OnNewDataChannel(data_session.get(), _))
      .WillOnce([](DataSessionBase*, std::unique_ptr<DataChannel> channel) {
        ASSERT_TRUE(channel);
        EXPECT_EQ("label", channel->label());
        EXPECT_EQ(DataChannel::State::kOpen, channel->state());
      });
  session_->AddIncomingStream(std::make_unique<FakeWebTransportStream>(
      std::vector<uint8_t>{1, 0, 5, 'l', 'a', 'b', 'e', 'l'}));
}

TEST_F(ReceiverDataSessionTest, ReportsOpenedFromItsOwnTask) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  // Nothing is reported from inside the server's notification.
  EXPECT_CALL(delegate_, OnDataSessionOpened(data_session.get()));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ReceiverDataSessionTest, DoesNotReportOpenedBeforeSessionAccepted) {
  std::unique_ptr<ReceiverDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ReceiverDataSessionTest, RejectsSecondSessionWhileActive) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  std::optional<Error> second_close_error;
  server_->AcceptSession(
      std::make_unique<FakeWebTransportSession>(&second_close_error));
  ASSERT_TRUE(second_close_error.has_value());
  EXPECT_EQ(Error::Code::kSocketInvalidState, second_close_error->code());
  EXPECT_TRUE(session_);
}

TEST_F(ReceiverDataSessionTest, ReportsClosureFromItsOwnTask) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  // Nothing is reported from inside the transport's notification.
  session_->SimulateClosed(Error(Error::Code::kConnectionFailed, "reset"));

  EXPECT_CALL(delegate_,
              OnDataSessionClosed(data_session.get(),
                                  CodeEquals(Error::Code::kConnectionFailed)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ReceiverDataSessionTest, ReportsGracefulClosure) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  session_->SimulateClosed(Error::None());
  EXPECT_CALL(delegate_, OnDataSessionClosed(data_session.get(),
                                             CodeEquals(Error::Code::kNone)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ReceiverDataSessionTest, ReportsServerClosure) {
  std::unique_ptr<ReceiverDataSession> data_session = CreateDataSession();
  ASSERT_TRUE(data_session);

  server_->SimulateServerClosed();
  EXPECT_CALL(delegate_,
              OnDataSessionClosed(data_session.get(),
                                  CodeEquals(Error::Code::kConnectionFailed)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ReceiverDataSessionTest, ReportsClosureOnlyOnce) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  session_->SimulateClosed(Error::None());
  server_->SimulateServerClosed();
  EXPECT_CALL(delegate_, OnDataSessionClosed(data_session.get(),
                                             CodeEquals(Error::Code::kNone)));
  task_runner_.RunTasksUntilIdle();
}

TEST_F(ReceiverDataSessionTest, DelegateMayDestroyFromOnDataSessionClosed) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  session_->SimulateClosed(Error::None());
  EXPECT_CALL(delegate_, OnDataSessionClosed(data_session.get(), _))
      .WillOnce([&](DataSessionBase*, const Error&) { data_session.reset(); });
  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(data_session);
  EXPECT_FALSE(server_);
}

TEST_F(ReceiverDataSessionTest, ResetDefersDestructionAndStopsReporting) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  ReceiverDataSession::Reset(data_session);
  EXPECT_FALSE(data_session);
  // Still alive, but no longer reporting to the delegate.
  ASSERT_TRUE(session_);
  session_->SimulateClosed(Error(Error::Code::kConnectionFailed, "reset"));
  EXPECT_TRUE(server_);

  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(server_);
  EXPECT_FALSE(session_);
  EXPECT_EQ(1, stop_count_);
}

TEST_F(ReceiverDataSessionTest, ResetFromDataChannelCallbackIsSafe) {
  std::unique_ptr<ReceiverDataSession> data_session =
      CreateConnectedDataSession();
  ASSERT_TRUE(session_);

  // The data session is on the call stack while the delegate handles a new
  // channel, so Reset() must not destroy it synchronously.
  EXPECT_CALL(delegate_, OnNewDataChannel(data_session.get(), _))
      .WillOnce([&](DataSessionBase*, std::unique_ptr<DataChannel> channel) {
        ReceiverDataSession::Reset(data_session);
      });
  session_->AddIncomingStream(std::make_unique<FakeWebTransportStream>(
      std::vector<uint8_t>{1, 0, 5, 'l', 'a', 'b', 'e', 'l'}));
  EXPECT_FALSE(data_session);
  EXPECT_TRUE(server_);

  task_runner_.RunTasksUntilIdle();
  EXPECT_FALSE(server_);
}

TEST_F(ReceiverDataSessionTest, ResetOfNullIsNoOp) {
  std::unique_ptr<ReceiverDataSession> data_session;
  ReceiverDataSession::Reset(data_session);
  EXPECT_FALSE(data_session);
}

}  // namespace openscreen::cast
