// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/common/channel/virtual_connection_router.h"

#include <optional>
#include <utility>
#include <vector>

#include "cast/common/channel/cast_message_handler.h"
#include "cast/common/channel/connection_namespace_handler.h"
#include "cast/common/channel/message_util.h"
#include "cast/common/channel/proto/cast_channel.pb.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

using proto::CastMessage;

VirtualConnectionRouter::SocketErrorHandler::~SocketErrorHandler() = default;

VirtualConnectionRouter::VirtualConnectionRouter() = default;

VirtualConnectionRouter::~VirtualConnectionRouter() = default;

void VirtualConnectionRouter::AddConnection(
    VirtualConnection virtual_connection,
    VirtualConnection::AssociatedData associated_data) {
  auto& socket_map = connections_[virtual_connection.socket_id];
  auto local_entries = socket_map.equal_range(virtual_connection.local_id);
  auto it = std::find_if(
      local_entries.first, local_entries.second,
      [&virtual_connection](const std::pair<std::string, VCTail>& entry) {
        return entry.second.peer_id == virtual_connection.peer_id;
      });
  if (it == socket_map.end()) {
    socket_map.emplace(std::move(virtual_connection.local_id),
                       VCTail{std::move(virtual_connection.peer_id),
                              std::move(associated_data)});
  }
}

bool VirtualConnectionRouter::RemoveConnection(
    const VirtualConnection& virtual_connection,
    VirtualConnection::CloseReason reason) {
  auto socket_entry = connections_.find(virtual_connection.socket_id);
  if (socket_entry == connections_.end()) {
    return false;
  }

  auto& socket_map = socket_entry->second;
  auto local_entries = socket_map.equal_range(virtual_connection.local_id);
  if (local_entries.first == socket_map.end()) {
    return false;
  }
  for (auto it = local_entries.first; it != local_entries.second; ++it) {
    if (it->second.peer_id == virtual_connection.peer_id) {
      socket_map.erase(it);
      if (socket_map.empty()) {
        connections_.erase(socket_entry);
      }
      return true;
    }
  }
  return false;
}

void VirtualConnectionRouter::OpenRemoteConnection(
    VirtualConnection conn,
    std::function<void(bool)> result_callback) {
  if (connection_handler_) {
    connection_handler_->OpenRemoteConnection(std::move(conn),
                                              std::move(result_callback));
    return;
  }
  result_callback(false);
}

void VirtualConnectionRouter::CloseRemoteConnection(VirtualConnection conn) {
  if (connection_handler_) {
    connection_handler_->CloseRemoteConnection(std::move(conn));
  }
}

void VirtualConnectionRouter::RemoveConnectionsByLocalId(
    const std::string& local_id) {
  for (auto socket_entry = connections_.begin();
       socket_entry != connections_.end();) {
    auto& socket_map = socket_entry->second;
    auto local_entries = socket_map.equal_range(local_id);
    if (local_entries.first != socket_map.end()) {
      socket_map.erase(local_entries.first, local_entries.second);
      if (socket_map.empty()) {
        socket_entry = connections_.erase(socket_entry);
        continue;
      }
    }
    ++socket_entry;
  }
}

void VirtualConnectionRouter::RemoveConnectionsBySocketId(int socket_id) {
  auto entry = connections_.find(socket_id);
  if (entry != connections_.end()) {
    connections_.erase(entry);
  }
}

std::optional<const VirtualConnection::AssociatedData*>
VirtualConnectionRouter::GetConnectionData(
    const VirtualConnection& virtual_connection) const {
  auto socket_entry = connections_.find(virtual_connection.socket_id);
  if (socket_entry == connections_.end()) {
    return std::nullopt;
  }

  auto& socket_map = socket_entry->second;
  auto local_entries = socket_map.equal_range(virtual_connection.local_id);
  if (local_entries.first == socket_map.end()) {
    return std::nullopt;
  }
  for (auto it = local_entries.first; it != local_entries.second; ++it) {
    if (it->second.peer_id == virtual_connection.peer_id) {
      return &it->second.data;
    }
  }
  return std::nullopt;
}

bool VirtualConnectionRouter::AddHandlerForLocalId(
    std::string local_id,
    CastMessageHandler* endpoint) {
  return endpoints_.emplace(std::move(local_id), endpoint).second;
}

bool VirtualConnectionRouter::RemoveHandlerForLocalId(
    const std::string& local_id) {
  return endpoints_.erase(local_id) == 1u;
}

void VirtualConnectionRouter::TakeSocket(SocketErrorHandler* error_handler,
                                         std::unique_ptr<CastSocket> socket) {
  int id = socket->socket_id();
  socket->SetClient(this);
  sockets_.emplace(id, SocketWithHandler{std::move(socket), error_handler});
}

void VirtualConnectionRouter::CloseSocket(int id) {
  auto it = sockets_.find(id);
  if (it != sockets_.end()) {
    reassembler_.DropMessagesForSocket(id);
    RemoveConnectionsBySocketId(id);
    std::unique_ptr<CastSocket> socket = std::move(it->second.socket);
    SocketErrorHandler* error_handler = it->second.error_handler.get();
    sockets_.erase(it);
    error_handler->OnClose(socket.get());
  }
}

Error VirtualConnectionRouter::Send(VirtualConnection virtual_conn,
                                    CastMessage message) {
  if (virtual_conn.peer_id == kBroadcastId) {
    return BroadcastFromLocalPeer(std::move(virtual_conn.local_id),
                                  std::move(message));
  }

  const VirtualConnection::AssociatedData* data =
      GetConnectionData(virtual_conn).value_or(nullptr);
  if (!IsTransportNamespace(message.namespace_()) && !data) {
    return Error::Code::kNoActiveConnection;
  }
  auto it = sockets_.find(virtual_conn.socket_id);
  if (it == sockets_.end()) {
    return Error::Code::kItemNotFound;
  }

  proto::CastMessage_ProtocolVersion protocol_version =
      kDefaultOutgoingMessageVersion;
  if (data) {
    protocol_version = static_cast<proto::CastMessage_ProtocolVersion>(
        data->max_protocol_version);
  }

  message.set_source_id(std::move(virtual_conn.local_id));
  message.set_destination_id(std::move(virtual_conn.peer_id));

  const ErrorOr<std::vector<proto::CastMessage>> chunks_or_error =
      ChunkMessage(message, protocol_version);
  if (chunks_or_error.is_error()) {
    return chunks_or_error.error();
  }

  const std::vector<proto::CastMessage>& chunks = chunks_or_error.value();
  for (const auto& chunk : chunks) {
    const Error result = it->second.socket->Send(chunk);
    if (!result.ok()) {
      return result;
    }
  }
  return Error::None();
}

void VirtualConnectionRouter::SendEnsuringConnected(
    VirtualConnection virtual_conn,
    proto::CastMessage message,
    std::function<void(Error)> callback) {
  if (GetConnectionData(virtual_conn)) {
    callback(Send(std::move(virtual_conn), std::move(message)));
    return;
  }
  OpenRemoteConnection(
      virtual_conn, [this, virtual_conn, message = std::move(message),
                     callback = std::move(callback)](bool success) mutable {
        if (!success) {
          callback(Error(Error::Code::kNoActiveConnection,
                         "Failed to establish connection"));
          return;
        }
        callback(Send(std::move(virtual_conn), std::move(message)));
      });
}

Error VirtualConnectionRouter::BroadcastFromLocalPeer(
    std::string local_id,
    proto::CastMessage message) {
  message.set_source_id(std::move(local_id));
  message.set_destination_id(kBroadcastId);

  // Broadcast to local endpoints.
  for (const auto& entry : endpoints_) {
    if (entry.first != message.source_id()) {
      entry.second->OnMessage(this, nullptr, message);
    }
  }

  // Broadcast to remote endpoints. If an Error occurs, continue broadcasting,
  // and later return the first Error that occurred.
  const ErrorOr<std::vector<proto::CastMessage>> chunks_or_error =
      ChunkMessage(message, kDefaultOutgoingMessageVersion);
  if (chunks_or_error.is_error()) {
    return chunks_or_error.error();
  }

  const std::vector<proto::CastMessage>& chunks = chunks_or_error.value();
  std::optional<Error> out;
  for (const auto& entry : sockets_) {
    for (const auto& chunk : chunks) {
      const Error result = entry.second.socket->Send(chunk);
      if (!result.ok()) {
        if (!out.has_value()) {
          out = result;
        }
        break;
      }
    }
  }
  return out.value_or(Error::None());
}

void VirtualConnectionRouter::OnError(CastSocket* socket, const Error& error) {
  const int id = socket->socket_id();
  auto it = sockets_.find(id);
  if (it != sockets_.end()) {
    reassembler_.DropMessagesForSocket(id);
    RemoveConnectionsBySocketId(id);
    std::unique_ptr<CastSocket> socket_owned = std::move(it->second.socket);
    SocketErrorHandler* error_handler = it->second.error_handler.get();
    sockets_.erase(it);
    error_handler->OnError(socket, error);
  }
}

void VirtualConnectionRouter::OnMessage(CastSocket* socket,
                                        CastMessage message) {
  OSP_CHECK(socket);

  ErrorOr<std::optional<CastMessage>> reassembled_or_error =
      reassembler_.ProcessMessage(socket->socket_id(), std::move(message));
  if (reassembled_or_error.is_error()) {
    OSP_DLOG_ERROR << "Failed to reassemble message: "
                   << reassembled_or_error.error();
    return;
  }
  if (!reassembled_or_error.value().has_value()) {
    return;
  }

  CastMessage reassembled = std::move(reassembled_or_error.value().value());
  const std::string& local_id = reassembled.destination_id();
  if (local_id == kBroadcastId) {
    for (const auto& entry : endpoints_) {
      entry.second->OnMessage(this, socket, reassembled);
    }
  } else {
    // Connection namespace messages are weird: The message.source_id() and
    // message.destination_id() are NOT treated as "envelope routing
    // information," like for all other namespaces. Instead, they are considered
    // part of the payload data for CONNECT/CLOSE requests. Thus, they require
    // special-case handling here.
    if (reassembled.namespace_() == kConnectionNamespace) {
      if (connection_handler_) {
        connection_handler_->OnMessage(this, socket, std::move(reassembled));
      }
      return;
    }

    // Drop all messages for virtual connections that do not yet exist.
    // Exception: All transport namespace messages (e.g., device auth,
    // heartbeats, etc.); because these are always assumed to have a route.
    if (!IsTransportNamespace(reassembled.namespace_()) &&
        !GetConnectionData(VirtualConnection{local_id, reassembled.source_id(),
                                             socket->socket_id()})) {
      return;
    }
    auto it = endpoints_.find(local_id);
    if (it != endpoints_.end()) {
      it->second->OnMessage(this, socket, std::move(reassembled));
    }
  }
}

}  // namespace openscreen::cast
