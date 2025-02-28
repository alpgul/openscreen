// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osp/public/presentation/presentation_controller.h"

#include <algorithm>
#include <sstream>
#include <type_traits>

#include "osp/impl/presentation/presentation_utils.h"
#include "osp/impl/presentation/url_availability_requester.h"
#include "osp/msgs/osp_messages.h"
#include "osp/public/message_demuxer.h"
#include "osp/public/network_service_manager.h"
#include "osp/public/request_response_handler.h"
#include "util/osp_logging.h"
#include "util/std_util.h"

namespace openscreen::osp {

#define DECLARE_MSG_REQUEST_RESPONSE(base_name)                        \
  using RequestMsgType = msgs::Presentation##base_name##Request;       \
  using ResponseMsgType = msgs::Presentation##base_name##Response;     \
                                                                       \
  static constexpr MessageEncodingFunction<RequestMsgType> kEncoder =  \
      &msgs::EncodePresentation##base_name##Request;                   \
  static constexpr MessageDecodingFunction<ResponseMsgType> kDecoder = \
      &msgs::DecodePresentation##base_name##Response;                  \
  static constexpr msgs::Type kResponseType =                          \
      msgs::Type::kPresentation##base_name##Response

struct StartRequest {
  DECLARE_MSG_REQUEST_RESPONSE(Start);

  msgs::PresentationStartRequest request;
  RequestDelegate* delegate;
  Connection::Delegate* presentation_connection_delegate;
};

struct ConnectionOpenRequest {
  DECLARE_MSG_REQUEST_RESPONSE(ConnectionOpen);

  msgs::PresentationConnectionOpenRequest request;
  RequestDelegate* delegate;
  Connection::Delegate* presentation_connection_delegate;
  std::unique_ptr<Connection> connection;
};

struct TerminationRequest {
  DECLARE_MSG_REQUEST_RESPONSE(Termination);

  msgs::PresentationTerminationRequest request;
};

class Controller::MessageGroupStreams final
    : public ProtocolConnection::Observer,
      public RequestResponseHandler<StartRequest>::Delegate,
      public RequestResponseHandler<ConnectionOpenRequest>::Delegate,
      public RequestResponseHandler<TerminationRequest>::Delegate {
 public:
  MessageGroupStreams(Controller* controller,
                      std::string_view instance_name,
                      uint64_t instance_id);
  MessageGroupStreams(const MessageGroupStreams&) = delete;
  MessageGroupStreams& operator=(const MessageGroupStreams&) = delete;
  MessageGroupStreams(MessageGroupStreams&&) noexcept = delete;
  MessageGroupStreams& operator=(MessageGroupStreams&&) noexcept = delete;
  ~MessageGroupStreams();

  uint64_t SendStartRequest(StartRequest request);
  void CancelStartRequest(uint64_t request_id);
  void OnMatchedResponse(StartRequest* request,
                         msgs::PresentationStartResponse* response,
                         uint64_t instance_id) override;
  void OnError(StartRequest* request, const Error& error) override;

  uint64_t SendConnectionOpenRequest(ConnectionOpenRequest request);
  void CancelConnectionOpenRequest(uint64_t request_id);
  void OnMatchedResponse(ConnectionOpenRequest* request,
                         msgs::PresentationConnectionOpenResponse* response,
                         uint64_t instance_id) override;
  void OnError(ConnectionOpenRequest* request, const Error& error) override;

  void SendTerminationRequest(TerminationRequest request);
  void OnMatchedResponse(TerminationRequest* request,
                         msgs::PresentationTerminationResponse* response,
                         uint64_t instance_id) override;
  void OnError(TerminationRequest* request, const Error& error) override;

  // ProtocolConnection::Observer overrides.
  void OnConnectionClosed(const ProtocolConnection& connection) override;

 private:
  uint64_t GetNextInternalRequestId() { return ++next_internal_request_id_; }

  void CreateProtocolConnection(bool is_initiation);

  Controller* const controller_;
  const std::string instance_name_;
  const uint64_t instance_id_;
  uint64_t next_internal_request_id_ = 1;

  std::unique_ptr<ProtocolConnection> initiation_protocol_connection_;
  RequestResponseHandler<StartRequest> initiation_handler_;
  RequestResponseHandler<TerminationRequest> termination_handler_;

  std::unique_ptr<ProtocolConnection> connection_protocol_connection_;
  RequestResponseHandler<ConnectionOpenRequest> connection_open_handler_;
};

Controller::MessageGroupStreams::MessageGroupStreams(
    Controller* controller,
    std::string_view instance_name,
    uint64_t instance_id)
    : controller_(controller),
      instance_name_(instance_name),
      instance_id_(instance_id),
      initiation_handler_(*this),
      termination_handler_(*this),
      connection_open_handler_(*this) {}

Controller::MessageGroupStreams::~MessageGroupStreams() {
  // Both are used to avoid triggering `OnConnectionClosed` during the
  // destruction process, which may cause error that delete one instance twice.
  if (initiation_protocol_connection_) {
    initiation_protocol_connection_->SetObserver(nullptr);
  }

  if (connection_protocol_connection_) {
    connection_protocol_connection_->SetObserver(nullptr);
  }
}

uint64_t Controller::MessageGroupStreams::SendStartRequest(
    StartRequest request) {
  if (!initiation_protocol_connection_) {
    CreateProtocolConnection(/*is_initiation=*/true);
  }

  uint64_t request_id = GetNextInternalRequestId();
  initiation_handler_.WriteMessage(request_id, std::move(request));
  return request_id;
}

void Controller::MessageGroupStreams::CancelStartRequest(uint64_t request_id) {
  // TODO(btolsch): Instead, mark the `request_id` for immediate termination if
  // we get a successful response.
  initiation_handler_.CancelMessage(request_id);
}

void Controller::MessageGroupStreams::OnMatchedResponse(
    StartRequest* request,
    msgs::PresentationStartResponse* response,
    uint64_t instance_id) {
  if (response->result != msgs::PresentationStartResponse_result::kSuccess) {
    std::stringstream ss;
    ss << "presentation-start-response for " << request->request.url
       << " failed: " << static_cast<int>(response->result);
    Error error(Error::Code::kUnknownStartError, ss.str());
    OSP_LOG_INFO << error.message();
    request->delegate->OnError(error);
    return;
  }

  OSP_LOG_INFO << "presentation started for " << request->request.url;
  Controller::ControlledPresentation& presentation =
      controller_->presentations_by_id_[request->request.presentation_id];
  presentation.instance_name = instance_name_;
  presentation.url = request->request.url;
  auto connection = std::make_unique<Connection>(
      Connection::PresentationInfo{request->request.presentation_id,
                                   request->request.url},
      request->presentation_connection_delegate, controller_);
  controller_->OpenConnection(
      response->connection_id, instance_id, instance_name_, request->delegate,
      std::move(connection), CreateClientProtocolConnection(instance_id));
}

void Controller::MessageGroupStreams::OnError(StartRequest* request,
                                              const Error& error) {
  request->delegate->OnError(error);
}

uint64_t Controller::MessageGroupStreams::SendConnectionOpenRequest(
    ConnectionOpenRequest request) {
  if (!connection_protocol_connection_) {
    CreateProtocolConnection(/*is_initiation=*/false);
  }

  uint64_t request_id = GetNextInternalRequestId();
  connection_open_handler_.WriteMessage(request_id, std::move(request));
  return request_id;
}

void Controller::MessageGroupStreams::CancelConnectionOpenRequest(
    uint64_t request_id) {
  connection_open_handler_.CancelMessage(request_id);
}

void Controller::MessageGroupStreams::OnMatchedResponse(
    ConnectionOpenRequest* request,
    msgs::PresentationConnectionOpenResponse* response,
    uint64_t instance_id) {
  if (response->result !=
      msgs::PresentationConnectionOpenResponse_result::kSuccess) {
    std::stringstream ss;
    ss << "presentation-connection-open-response for " << request->request.url
       << " failed: " << static_cast<int>(response->result);
    Error error(Error::Code::kUnknownStartError, ss.str());
    OSP_LOG_INFO << error.message();
    request->delegate->OnError(error);
    return;
  }

  OSP_LOG_INFO << "presentation connection opened to "
               << request->request.presentation_id;
  if (request->presentation_connection_delegate) {
    request->connection = std::make_unique<Connection>(
        Connection::PresentationInfo{request->request.presentation_id,
                                     request->request.url},
        request->presentation_connection_delegate, controller_);
  }
  std::unique_ptr<ProtocolConnection> protocol_connection =
      CreateClientProtocolConnection(instance_id);
  request->connection->OnConnected(response->connection_id, instance_id,
                                   std::move(protocol_connection));
  controller_->AddConnection(request->connection.get());
  request->delegate->OnConnection(std::move(request->connection));
}

void Controller::MessageGroupStreams::OnError(ConnectionOpenRequest* request,
                                              const Error& error) {
  request->delegate->OnError(error);
}

void Controller::MessageGroupStreams::SendTerminationRequest(
    TerminationRequest request) {
  if (!initiation_protocol_connection_) {
    CreateProtocolConnection(/*is_initiation=*/true);
  }

  termination_handler_.WriteMessage(std::move(request));
}

void Controller::MessageGroupStreams::OnMatchedResponse(
    TerminationRequest* request,
    msgs::PresentationTerminationResponse* response,
    uint64_t instance_id) {
  OSP_VLOG << "got presentation-termination-response for "
           << request->request.presentation_id << " with result "
           << static_cast<int>(response->result);
  controller_->TerminatePresentationById(request->request.presentation_id);
}

void Controller::MessageGroupStreams::OnError(TerminationRequest* request,
                                              const Error& error) {}

void Controller::MessageGroupStreams::OnConnectionClosed(
    const ProtocolConnection& connection) {
  if (initiation_protocol_connection_ &&
      initiation_protocol_connection_.get() == &connection) {
    initiation_handler_.Reset();
    termination_handler_.Reset();
    initiation_protocol_connection_.reset();
    return;
  }

  if (connection_protocol_connection_ &&
      connection_protocol_connection_.get() == &connection) {
    connection_open_handler_.Reset();
    connection_protocol_connection_.reset();
  }
}

void Controller::MessageGroupStreams::CreateProtocolConnection(
    bool is_initiation) {
  if (is_initiation) {
    initiation_protocol_connection_ =
        CreateClientProtocolConnection(instance_id_);
    if (initiation_protocol_connection_) {
      initiation_protocol_connection_->SetObserver(this);
      initiation_handler_.SetConnection(initiation_protocol_connection_.get());
      termination_handler_.SetConnection(initiation_protocol_connection_.get());
    } else {
      OSP_LOG_WARN << "There is no valid underlying connection.";
    }
  } else {
    connection_protocol_connection_ =
        CreateClientProtocolConnection(instance_id_);
    if (connection_protocol_connection_) {
      connection_protocol_connection_->SetObserver(this);
      connection_open_handler_.SetConnection(
          connection_protocol_connection_.get());
    } else {
      OSP_LOG_WARN << "There is no valid underlying connection.";
    }
  }
}

class Controller::TerminationListener final
    : public MessageDemuxer::MessageCallback {
 public:
  TerminationListener(Controller* controller,
                      const std::string& presentation_id,
                      uint64_t instance_id);
  TerminationListener(const TerminationListener&) = delete;
  TerminationListener& operator=(const TerminationListener&) = delete;
  TerminationListener(TerminationListener&&) noexcept = delete;
  TerminationListener& operator=(TerminationListener&&) noexcept = delete;
  ~TerminationListener() override;

  // MessageDemuxer::MessageCallback overrides.
  ErrorOr<size_t> OnStreamMessage(uint64_t instance_id,
                                  uint64_t connection_id,
                                  msgs::Type message_type,
                                  const uint8_t* buffer,
                                  size_t buffer_size,
                                  Clock::time_point now) override;

 private:
  Controller* const controller_;
  std::string presentation_id_;
  MessageDemuxer::MessageWatch event_watch_;
};

Controller::TerminationListener::TerminationListener(
    Controller* controller,
    const std::string& presentation_id,
    uint64_t instance_id)
    : controller_(controller), presentation_id_(presentation_id) {
  event_watch_ = GetClientDemuxer().WatchMessageType(
      instance_id, msgs::Type::kPresentationTerminationEvent, this);
}

Controller::TerminationListener::~TerminationListener() = default;

ErrorOr<size_t> Controller::TerminationListener::OnStreamMessage(
    uint64_t instance_id,
    uint64_t connection_id,
    msgs::Type message_type,
    const uint8_t* buffer,
    size_t buffer_size,
    Clock::time_point now) {
  OSP_CHECK_EQ(static_cast<int>(msgs::Type::kPresentationTerminationEvent),
               static_cast<int>(message_type));
  msgs::PresentationTerminationEvent event;
  const msgs::CborResult result =
      msgs::DecodePresentationTerminationEvent(buffer, buffer_size, event);
  if (result < 0) {
    if (result == msgs::kParserEOF) {
      return Error::Code::kCborIncompleteMessage;
    }

    OSP_LOG_WARN << "decode presentation-termination-event error: " << result;
    return Error::Code::kCborParsing;
  } else if (event.presentation_id != presentation_id_) {
    OSP_LOG_WARN << "got presentation-termination-event for wrong id: "
                 << presentation_id_ << " vs. " << event.presentation_id;
    return result;
  }

  OSP_LOG_INFO << "termination event";
  auto presentation_entry =
      controller_->presentations_by_id_.find(event.presentation_id);
  if (presentation_entry != controller_->presentations_by_id_.end()) {
    for (auto* connection : presentation_entry->second.connections) {
      connection->OnTerminated();
    }
    controller_->presentations_by_id_.erase(presentation_entry);
  }
  controller_->termination_listener_by_id_.erase(event.presentation_id);
  return result;
}

RequestDelegate::RequestDelegate() = default;
RequestDelegate::~RequestDelegate() = default;

ReceiverObserver::ReceiverObserver() = default;
ReceiverObserver::~ReceiverObserver() = default;

Controller::ReceiverWatch::ReceiverWatch() = default;
Controller::ReceiverWatch::ReceiverWatch(Controller* controller,
                                         const std::vector<std::string>& urls,
                                         ReceiverObserver* observer)
    : urls_(urls), observer_(observer), controller_(controller) {}

Controller::ReceiverWatch::ReceiverWatch(
    Controller::ReceiverWatch&& other) noexcept {
  // Although all fields are POD, this does not use the default implementation.
  // See `operator=` for details.
  *this = std::move(other);
}

Controller::ReceiverWatch& Controller::ReceiverWatch::operator=(
    Controller::ReceiverWatch&& other) noexcept {
  // Although all fields are POD, this does not use the default `operator=`
  // implementation because it is important that we should stop current watch
  // for `observer_` before taking values from `other` and making `other`
  // invalid.
  StopWatching();
  urls_ = std::move(other.urls_);
  observer_ = other.observer_;
  controller_ = other.controller_;
  other.observer_ = nullptr;
  other.observer_ = nullptr;
  return *this;
}

Controller::ReceiverWatch::~ReceiverWatch() {
  StopWatching();
}

void Controller::ReceiverWatch::Reset() {
  StopWatching();
  urls_.clear();
  controller_ = nullptr;
}

void Controller::ReceiverWatch::StopWatching() {
  if (observer_) {
    controller_->CancelReceiverWatch(urls_, observer_);
  }
  observer_ = nullptr;
}

Controller::ConnectRequest::ConnectRequest() = default;

Controller::ConnectRequest::ConnectRequest(Controller* controller,
                                           const std::string& instance_name,
                                           bool is_reconnect,
                                           uint64_t request_id)
    : instance_name_(instance_name),
      is_reconnect_(is_reconnect),
      request_id_(request_id),
      controller_(controller) {}

Controller::ConnectRequest::ConnectRequest(ConnectRequest&& other) noexcept {
  // Although all fields are POD, this does not use the default implementation.
  // See `operator=` for details.
  *this = std::move(other);
}

Controller::ConnectRequest& Controller::ConnectRequest::operator=(
    ConnectRequest&& other) noexcept {
  // Although all fields are POD, this does not use the default `operator=`
  // implementation because it is important that we should cancel current
  // request for `controller_` before taking values from `other` and making
  // `other` invalid.
  CancelRequest();
  instance_name_ = std::move(other.instance_name_);
  is_reconnect_ = other.is_reconnect_;
  request_id_ = other.request_id_;
  controller_ = other.controller_;
  other.request_id_ = 0;
  other.is_reconnect_ = false;
  other.controller_ = nullptr;
  return *this;
}

Controller::ConnectRequest::~ConnectRequest() {
  CancelRequest();
}

void Controller::ConnectRequest::Reset() {
  CancelRequest();
  instance_name_.clear();
  is_reconnect_ = false;
  controller_ = nullptr;
}

void Controller::ConnectRequest::CancelRequest() {
  if (request_id_) {
    controller_->CancelConnectRequest(instance_name_, is_reconnect_,
                                      request_id_);
  }
  request_id_ = 0;
}

Controller::Controller(ClockNowFunctionPtr now_function) {
  availability_requester_ =
      std::make_unique<UrlAvailabilityRequester>(now_function);
  connection_manager_ = std::make_unique<ConnectionManager>(GetClientDemuxer());
  const std::vector<ServiceInfo>& receivers =
      NetworkServiceManager::Get()->GetServiceListener()->GetReceivers();
  for (const auto& info : receivers) {
    availability_requester_->AddReceiver(info);
  }

  NetworkServiceManager::Get()->GetServiceListener()->AddObserver(*this);
}

Controller::~Controller() {
  NetworkServiceManager::Get()->GetServiceListener()->RemoveObserver(*this);
}

Error Controller::CloseConnection(Connection* connection,
                                  Connection::CloseReason reason) {
  auto presentation_entry =
      presentations_by_id_.find(connection->presentation_info().id);
  if (presentation_entry == presentations_by_id_.end()) {
    std::stringstream ss;
    ss << "no presentation found when trying to close connection "
       << connection->presentation_info().id << ":"
       << connection->connection_id();
    return Error(Error::Code::kNoPresentationFound, ss.str());
  }

  std::unique_ptr<ProtocolConnection> protocol_connection =
      CreateClientProtocolConnection(connection->instance_id());
  if (!protocol_connection) {
    return Error::Code::kNoActiveConnection;
  }

  msgs::PresentationConnectionCloseEvent event = {
      .connection_id = connection->connection_id(),
      .reason = ConvertCloseEventReason(reason),
      .connection_count = connection_manager_->ConnectionCount(),
      .has_error_message = false};
  return protocol_connection->WriteMessage(
      event, msgs::EncodePresentationConnectionCloseEvent);
}

Error Controller::OnPresentationTerminated(const std::string& presentation_id,
                                           TerminationSource source,
                                           TerminationReason reason) {
  auto presentation_entry = presentations_by_id_.find(presentation_id);
  if (presentation_entry == presentations_by_id_.end()) {
    return Error::Code::kNoPresentationFound;
  }

  ControlledPresentation& presentation = presentation_entry->second;
  for (auto* connection : presentation.connections) {
    connection->OnTerminated();
  }

  TerminationRequest request = {
      .request = {.presentation_id = presentation_id,
                  .reason = msgs::PresentationTerminationReason::kUserRequest}};
  group_streams_by_instance_name_[presentation.instance_name]
      ->SendTerminationRequest(std::move(request));
  presentations_by_id_.erase(presentation_entry);
  termination_listener_by_id_.erase(presentation_id);
  return Error::None();
}

void Controller::OnConnectionDestroyed(Connection* connection) {
  auto presentation_entry =
      presentations_by_id_.find(connection->presentation_info().id);
  if (presentation_entry == presentations_by_id_.end()) {
    return;
  }

  std::vector<Connection*>& connections =
      presentation_entry->second.connections;
  connections.erase(
      std::remove(connections.begin(), connections.end(), connection),
      connections.end());

  connection_manager_->RemoveConnection(connection);
}

void Controller::BuildConnection(std::string_view instance_name) {
  std::string name(instance_name);
  auto requset_entry = connect_requests_by_instance_name_.find(name);
  if (requset_entry != connect_requests_by_instance_name_.end()) {
    OSP_LOG_WARN << "There is alreay a request in progress for connecting to "
                 << instance_name;
    return;
  }

  auto result = connect_requests_by_instance_name_.insert(
      {std::move(name), openscreen::osp::ConnectRequest()});
  NetworkServiceManager::Get()->GetProtocolConnectionClient()->Connect(
      instance_name, result.first->second, this);
}

Controller::ReceiverWatch Controller::RegisterReceiverWatch(
    const std::vector<std::string>& urls,
    ReceiverObserver* observer) {
  availability_requester_->AddObserver(urls, observer);
  return ReceiverWatch(this, urls, observer);
}

Controller::ConnectRequest Controller::StartPresentation(
    const std::string& url,
    const std::string& instance_name,
    RequestDelegate* delegate,
    Connection::Delegate* conn_delegate) {
  StartRequest request = {
      .request = {.presentation_id = MakePresentationId(url, instance_name),
                  .url = url},
      .delegate = delegate,
      .presentation_connection_delegate = conn_delegate};
  uint64_t request_id =
      group_streams_by_instance_name_[instance_name]->SendStartRequest(
          std::move(request));
  return ConnectRequest(this, instance_name, /*is_reconnect=*/false,
                        request_id);
}

Controller::ConnectRequest Controller::ReconnectPresentation(
    const std::vector<std::string>& urls,
    const std::string& presentation_id,
    const std::string& instance_name,
    RequestDelegate* delegate,
    Connection::Delegate* conn_delegate) {
  auto presentation_entry = presentations_by_id_.find(presentation_id);
  if (presentation_entry == presentations_by_id_.end()) {
    delegate->OnError(Error::Code::kNoPresentationFound);
    return ConnectRequest();
  }

  if (!Contains(urls, presentation_entry->second.url)) {
    delegate->OnError(Error::Code::kNoPresentationFound);
    return ConnectRequest();
  }

  ConnectionOpenRequest request = {
      .request = {.presentation_id = presentation_id,
                  .url = presentation_entry->second.url},
      .delegate = delegate,
      .presentation_connection_delegate = conn_delegate,
      .connection = nullptr};
  uint64_t request_id =
      group_streams_by_instance_name_[instance_name]->SendConnectionOpenRequest(
          std::move(request));
  return ConnectRequest(this, instance_name, /*is_reconnect=*/true, request_id);
}

Controller::ConnectRequest Controller::ReconnectConnection(
    std::unique_ptr<Connection> connection,
    RequestDelegate* delegate) {
  if (connection->state() != Connection::State::kClosed) {
    delegate->OnError(Error::Code::kInvalidConnectionState);
    return ConnectRequest();
  }

  const Connection::PresentationInfo& info = connection->presentation_info();
  auto presentation_entry = presentations_by_id_.find(info.id);
  if (presentation_entry == presentations_by_id_.end() ||
      presentation_entry->second.url != info.url) {
    OSP_LOG_ERROR << "missing ControlledPresentation for non-terminated "
                     "connection with info ("
                  << info.id << ", " << info.url << ")";
    delegate->OnError(Error::Code::kNoPresentationFound);
    return ConnectRequest();
  }

  OSP_CHECK(connection_manager_->GetConnection(connection->connection_id()))
      << "valid connection for reconnect is unknown to the "
         "connection manager";
  connection_manager_->RemoveConnection(connection.get());
  connection->OnConnecting();
  ConnectionOpenRequest request = {
      .request = {.presentation_id = info.id, .url = info.url},
      .delegate = delegate,
      .presentation_connection_delegate = nullptr,
      .connection = std::move(connection)};
  const std::string& instance_name = presentation_entry->second.instance_name;
  uint64_t request_id =
      group_streams_by_instance_name_[instance_name]->SendConnectionOpenRequest(
          std::move(request));
  return ConnectRequest(this, instance_name, /*is_reconnect=*/true, request_id);
}

std::string Controller::GetServiceIdForPresentationId(
    const std::string& presentation_id) const {
  auto presentation_entry = presentations_by_id_.find(presentation_id);
  if (presentation_entry == presentations_by_id_.end()) {
    return "";
  }
  return presentation_entry->second.instance_name;
}

ProtocolConnection* Controller::GetConnectionRequestGroupStream(
    const std::string& instance_name) {
  OSP_UNIMPLEMENTED();
  return nullptr;
}

void Controller::OnStarted() {}
void Controller::OnStopped() {}
void Controller::OnSuspended() {}
void Controller::OnSearching() {}

void Controller::OnReceiverAdded(const ServiceInfo& info) {}

void Controller::OnReceiverChanged(const ServiceInfo& info) {
  availability_requester_->ChangeReceiver(info);
}

void Controller::OnReceiverRemoved(const ServiceInfo& info) {
  group_streams_by_instance_name_.erase(info.instance_name);
  availability_requester_->RemoveReceiver(info);
}

void Controller::OnAllReceiversRemoved() {
  group_streams_by_instance_name_.clear();
  availability_requester_->RemoveAllReceivers();
}

void Controller::OnError(const Error&) {}

void Controller::OnConnectSucceed(uint64_t request_id,
                                  std::string_view instance_name,
                                  uint64_t instance_id) {
  auto request_entry =
      connect_requests_by_instance_name_.find(std::string(instance_name));
  if (request_entry == connect_requests_by_instance_name_.end()) {
    return;
  }

  OSP_CHECK_EQ(request_id, request_entry->second.request_id());
  request_entry->second.MarkComplete();
  connect_requests_by_instance_name_.erase(request_entry);

  group_streams_by_instance_name_.emplace(
      instance_name,
      std::make_unique<MessageGroupStreams>(this, instance_name, instance_id));
  availability_requester_->CreateReceiverRequester(instance_name, instance_id);
  OSP_LOG_INFO << "Controller succeed to build the underlying connection to: "
               << instance_name;
}

void Controller::OnConnectFailed(uint64_t request_id,
                                 std::string_view instance_name) {
  auto request_entry =
      connect_requests_by_instance_name_.find(std::string(instance_name));
  if (request_entry == connect_requests_by_instance_name_.end()) {
    return;
  }

  request_entry->second.MarkComplete();
  connect_requests_by_instance_name_.erase(request_entry);
  OSP_LOG_WARN << "Controller failed to build the underlying connection to: "
               << instance_name;
}

// static
std::string Controller::MakePresentationId(const std::string& url,
                                           const std::string& instance_name) {
  // TODO(btolsch): This is just a placeholder for the demo. It should
  // eventually become a GUID/unguessable token routine.
  std::string safe_id = instance_name;
  for (auto& c : safe_id) {
    if (c < ' ' || c > '~') {
      c = '.';
    }
  }
  return safe_id + ":" + url;
}

void Controller::AddConnection(Connection* connection) {
  connection_manager_->AddConnection(connection);
}

void Controller::OpenConnection(
    uint64_t connection_id,
    uint64_t instance_id,
    const std::string& instance_name,
    RequestDelegate* request_delegate,
    std::unique_ptr<Connection>&& connection,
    std::unique_ptr<ProtocolConnection>&& protocol_connection) {
  connection->OnConnected(connection_id, instance_id,
                          std::move(protocol_connection));
  const std::string& presentation_id = connection->presentation_info().id;
  auto presentation_entry = presentations_by_id_.find(presentation_id);
  if (presentation_entry == presentations_by_id_.end()) {
    auto emplace_entry = presentations_by_id_.emplace(
        presentation_id,
        ControlledPresentation{
            instance_name, connection->presentation_info().url, {}});
    presentation_entry = emplace_entry.first;
  }
  ControlledPresentation& presentation = presentation_entry->second;
  presentation.connections.push_back(connection.get());
  AddConnection(connection.get());

  auto terminate_entry = termination_listener_by_id_.find(presentation_id);
  if (terminate_entry == termination_listener_by_id_.end()) {
    termination_listener_by_id_.emplace(
        presentation_id, std::make_unique<TerminationListener>(
                             this, presentation_id, instance_id));
  }
  request_delegate->OnConnection(std::move(connection));
}

void Controller::TerminatePresentationById(const std::string& presentation_id) {
  auto presentation_entry = presentations_by_id_.find(presentation_id);
  if (presentation_entry != presentations_by_id_.end()) {
    for (auto* connection : presentation_entry->second.connections) {
      connection->OnTerminated();
    }
    presentations_by_id_.erase(presentation_entry);
  }
}

void Controller::CancelReceiverWatch(const std::vector<std::string>& urls,
                                     ReceiverObserver* observer) {
  availability_requester_->RemoveObserverUrls(urls, observer);
}

void Controller::CancelConnectRequest(const std::string& instance_name,
                                      bool is_reconnect,
                                      uint64_t request_id) {
  auto group_streams_entry =
      group_streams_by_instance_name_.find(instance_name);
  if (group_streams_entry == group_streams_by_instance_name_.end()) {
    return;
  }

  if (is_reconnect) {
    group_streams_entry->second->CancelConnectionOpenRequest(request_id);
  } else {
    group_streams_entry->second->CancelStartRequest(request_id);
  }
}

}  // namespace openscreen::osp
