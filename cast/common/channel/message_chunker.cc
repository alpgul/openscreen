// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/common/channel/message_chunker.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

#include "cast/common/channel/message_util.h"
#include "util/osp_logging.h"

namespace openscreen::cast {

ByteView GetPayloadView(const proto::CastMessage& message) {
  if (message.has_payload_binary()) {
    return ByteViewFromString(message.payload_binary());
  }
  if (message.has_payload_utf8()) {
    return ByteViewFromString(message.payload_utf8());
  }
  return {};
}

ErrorOr<std::vector<proto::CastMessage>> ChunkMessage(
    const proto::CastMessage& message,
    proto::CastMessage_ProtocolVersion protocol_version) {
  const ByteView full_payload = GetPayloadView(message);

  if (full_payload.size() <= kMaxChunkPayloadSize) {
    return std::vector<proto::CastMessage>{message};
  }

  if (protocol_version < proto::CastMessage_ProtocolVersion_CASTV2_1_1) {
    return Error::Code::kCastV2InvalidMessage;
  }

  std::vector<proto::CastMessage> chunks;
  size_t offset = 0;
  while (offset < full_payload.size()) {
    proto::CastMessage chunk;
    chunk.set_protocol_version(protocol_version);
    chunk.set_source_id(message.source_id());
    chunk.set_destination_id(message.destination_id());
    chunk.set_namespace_(message.namespace_());
    chunk.set_payload_type(message.payload_type());

    const size_t chunk_size =
        std::min(kMaxChunkPayloadSize, full_payload.size() - offset);
    // Per the Cast V2 Message Chunking specification, chunk data is always
    // stored in the payload_binary field to avoid splitting multibyte UTF-8
    // characters across chunk boundaries. The payload_type field preserves
    // whether the reconstituted message is UTF-8 string or binary.
    chunk.set_payload_binary(full_payload.data() + offset, chunk_size);
    offset += chunk_size;

    if (offset < full_payload.size()) {
      chunk.set_continued(true);
      chunk.set_remaining_length(
          static_cast<uint32_t>(full_payload.size() - offset));
    } else {
      chunk.set_continued(false);
      chunk.set_remaining_length(0);
    }
    chunks.push_back(std::move(chunk));
  }

  return chunks;
}

bool MessageReassembler::MessageKey::operator<(const MessageKey& other) const {
  return std::tie(socket_id, source_id, destination_id, namespace_) <
         std::tie(other.socket_id, other.source_id, other.destination_id,
                  other.namespace_);
}

MessageReassembler::MessageReassembler() = default;
MessageReassembler::~MessageReassembler() = default;
MessageReassembler::MessageReassembler(MessageReassembler&&) noexcept = default;
MessageReassembler& MessageReassembler::operator=(
    MessageReassembler&&) noexcept = default;

ErrorOr<std::optional<proto::CastMessage>> MessageReassembler::ProcessMessage(
    int socket_id,
    proto::CastMessage message) {
  const MessageKey key = {socket_id, message.source_id(),
                          message.destination_id(), message.namespace_()};

  auto it = incomplete_messages_.find(key);
  if (it == incomplete_messages_.end()) {
    if (!message.continued()) {
      // Per the Cast V2 Message Chunking specification, chunk data is always
      // transmitted in the payload_binary field. If a chunked message consists
      // of only a single segment, its string payload must be moved to
      // payload_utf8 for delivery.
      if (message.payload_type() == proto::CastMessage_PayloadType_STRING &&
          message.has_payload_binary()) {
        message.set_payload_utf8(std::move(*message.mutable_payload_binary()));
        message.clear_payload_binary();
      }
      return std::make_optional(std::move(message));
    }

    const ByteView chunk_payload = GetPayloadView(message);
    IncompleteMessage incomplete;
    incomplete.payload_type = message.payload_type();
    incomplete.source_id = message.source_id();
    incomplete.destination_id = message.destination_id();
    incomplete.namespace_ = message.namespace_();
    incomplete.data.assign(chunk_payload.begin(), chunk_payload.end());
    incomplete.expected_remaining_length =
        message.has_remaining_length() ? message.remaining_length() : 0;

    incomplete_messages_.emplace(key, std::move(incomplete));
    return std::optional<proto::CastMessage>{};
  }

  // Continuing an existing message.
  IncompleteMessage& incomplete = it->second;

  // Validate remaining_length if present.
  const ByteView chunk_payload = GetPayloadView(message);
  if (incomplete.expected_remaining_length > 0 &&
      incomplete.expected_remaining_length !=
          message.remaining_length() + chunk_payload.size()) {
    incomplete_messages_.erase(it);
    return Error::Code::kCastV2InvalidMessage;
  }

  incomplete.data.insert(incomplete.data.end(), chunk_payload.begin(),
                         chunk_payload.end());
  incomplete.expected_remaining_length =
      message.has_remaining_length() ? message.remaining_length() : 0;

  if (message.continued()) {
    return std::optional<proto::CastMessage>{};
  }

  // Last chunk received. Assemble and return.
  proto::CastMessage assembled;
  assembled.set_protocol_version(message.protocol_version());
  assembled.set_source_id(std::move(incomplete.source_id));
  assembled.set_destination_id(std::move(incomplete.destination_id));
  assembled.set_namespace_(std::move(incomplete.namespace_));
  assembled.set_payload_type(incomplete.payload_type);

  if (incomplete.payload_type == proto::CastMessage_PayloadType_STRING) {
    assembled.set_payload_utf8(ByteViewToString(incomplete.data));
  } else {
    assembled.set_payload_binary(incomplete.data.data(),
                                 incomplete.data.size());
  }

  incomplete_messages_.erase(it);
  return std::make_optional(std::move(assembled));
}

void MessageReassembler::DropMessagesForSocket(int socket_id) {
  std::erase_if(incomplete_messages_, [socket_id](const auto& item) {
    return item.first.socket_id == socket_id;
  });
}

}  // namespace openscreen::cast
