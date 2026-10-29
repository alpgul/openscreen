// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/common/channel/message_chunker.h"

#include <string>
#include <utility>
#include <vector>

#include "cast/common/channel/message_util.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace openscreen::cast {

using proto::CastMessage;
using proto::CastMessage_PayloadType;

TEST(MessageChunkerTest, GetPayloadViewViewsBinaryAndStringWithoutCopy) {
  CastMessage string_msg;
  string_msg.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  string_msg.set_payload_utf8("hello world");
  ByteView str_view = GetPayloadView(string_msg);
  EXPECT_EQ(ByteViewToString(str_view), "hello world");

  CastMessage bin_msg;
  bin_msg.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_BINARY);
  bin_msg.set_payload_binary("binary data");
  ByteView bin_view = GetPayloadView(bin_msg);
  EXPECT_EQ(ByteViewToString(bin_view), "binary data");

  CastMessage empty_msg;
  EXPECT_TRUE(GetPayloadView(empty_msg).empty());
}

TEST(MessageChunkerTest, NoChunkingForSmallMessage) {
  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  message.set_payload_utf8("small message");

  std::vector<CastMessage> chunks =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_3)
          .value();
  ASSERT_EQ(chunks.size(), 1u);
  EXPECT_EQ(chunks[0].payload_utf8(), "small message");
  EXPECT_FALSE(chunks[0].continued());
}

TEST(MessageChunkerTest, ChunkingFailsOnUnsupportedProtocolVersion) {
  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_0);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  message.set_payload_utf8(std::string(kMaxChunkPayloadSize + 100, 'x'));

  auto result =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_0);
  EXPECT_TRUE(result.is_error());
  EXPECT_EQ(result.error().code(), Error::Code::kCastV2InvalidMessage);
}

TEST(MessageChunkerTest, ChunkingForLargeBinaryMessage) {
  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_BINARY);

  std::vector<uint8_t> large_payload(kMaxChunkPayloadSize * 2 + 100, 'a');
  message.set_payload_binary(large_payload.data(), large_payload.size());

  std::vector<CastMessage> chunks =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_3)
          .value();
  ASSERT_EQ(chunks.size(), 3u);

  EXPECT_TRUE(chunks[0].continued());
  EXPECT_EQ(chunks[0].remaining_length(), kMaxChunkPayloadSize + 100u);
  EXPECT_EQ(chunks[0].payload_binary().size(), kMaxChunkPayloadSize);

  EXPECT_TRUE(chunks[1].continued());
  EXPECT_EQ(chunks[1].remaining_length(), 100u);
  EXPECT_EQ(chunks[1].payload_binary().size(), kMaxChunkPayloadSize);

  EXPECT_FALSE(chunks[2].continued());
  EXPECT_EQ(chunks[2].remaining_length(), 0u);
  EXPECT_EQ(chunks[2].payload_binary().size(), 100u);
}

TEST(MessageChunkerTest, ChunkingForLargeStringMessage) {
  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);

  std::string large_payload(kMaxChunkPayloadSize + 100, 's');
  message.set_payload_utf8(large_payload);

  std::vector<CastMessage> chunks =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_3)
          .value();
  ASSERT_EQ(chunks.size(), 2u);

  // Design doc says chunked strings use payload_binary.
  EXPECT_TRUE(chunks[0].has_payload_binary());
  EXPECT_FALSE(chunks[0].has_payload_utf8());
  EXPECT_EQ(chunks[0].payload_type(),
            CastMessage_PayloadType::CastMessage_PayloadType_STRING);

  EXPECT_TRUE(chunks[1].has_payload_binary());
  EXPECT_FALSE(chunks[1].has_payload_utf8());
  EXPECT_EQ(chunks[1].payload_type(),
            CastMessage_PayloadType::CastMessage_PayloadType_STRING);
}

TEST(MessageChunkerTest, ReassembleChunks) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_BINARY);

  std::vector<uint8_t> large_payload(kMaxChunkPayloadSize + 500, 'x');
  message.set_payload_binary(large_payload.data(), large_payload.size());

  std::vector<CastMessage> chunks =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_3)
          .value();
  ASSERT_EQ(chunks.size(), 2u);

  auto result1 = reassembler.ProcessMessage(socket_id, chunks[0]);
  ASSERT_TRUE(result1.is_value());
  EXPECT_FALSE(result1.value().has_value());

  auto result2 = reassembler.ProcessMessage(socket_id, chunks[1]);
  ASSERT_TRUE(result2.is_value());
  ASSERT_TRUE(result2.value().has_value());
  CastMessage assembled = std::move(result2.value().value());

  EXPECT_EQ(assembled.source_id(), "source");
  EXPECT_EQ(assembled.payload_binary().size(), large_payload.size());
  EXPECT_EQ(std::vector<uint8_t>(assembled.payload_binary().begin(),
                                 assembled.payload_binary().end()),
            large_payload);
}

TEST(MessageChunkerTest, ReassembleInterleavedChunks) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  auto make_large_msg = [](const std::string& ns, char c) {
    CastMessage message;
    message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
    message.set_source_id("s");
    message.set_destination_id("d");
    message.set_namespace_(ns);
    message.set_payload_type(
        CastMessage_PayloadType::CastMessage_PayloadType_STRING);
    message.set_payload_utf8(std::string(kMaxChunkPayloadSize + 100, c));
    return message;
  };

  CastMessage msg1 = make_large_msg("ns1", '1');
  CastMessage msg2 = make_large_msg("ns2", '2');

  std::vector<CastMessage> chunks1 =
      ChunkMessage(msg1, proto::CastMessage_ProtocolVersion_CASTV2_1_3).value();
  std::vector<CastMessage> chunks2 =
      ChunkMessage(msg2, proto::CastMessage_ProtocolVersion_CASTV2_1_3).value();

  // Interleave
  auto intermediate1 = reassembler.ProcessMessage(socket_id, chunks1[0]);
  ASSERT_TRUE(intermediate1.is_value());
  EXPECT_FALSE(intermediate1.value().has_value());

  auto intermediate2 = reassembler.ProcessMessage(socket_id, chunks2[0]);
  ASSERT_TRUE(intermediate2.is_value());
  EXPECT_FALSE(intermediate2.value().has_value());

  auto res1 = reassembler.ProcessMessage(socket_id, chunks1[1]);
  ASSERT_TRUE(res1.is_value());
  ASSERT_TRUE(res1.value().has_value());
  EXPECT_EQ(res1.value()->payload_utf8(),
            std::string(kMaxChunkPayloadSize + 100, '1'));

  auto res2 = reassembler.ProcessMessage(socket_id, chunks2[1]);
  ASSERT_TRUE(res2.is_value());
  ASSERT_TRUE(res2.value().has_value());
  EXPECT_EQ(res2.value()->payload_utf8(),
            std::string(kMaxChunkPayloadSize + 100, '2'));
}

TEST(MessageChunkerTest, InvalidRemainingLengthRejectsChunk) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  message.set_payload_utf8(std::string(kMaxChunkPayloadSize + 100, 'y'));

  std::vector<CastMessage> chunks =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_3)
          .value();
  ASSERT_EQ(chunks.size(), 2u);

  auto intermediate = reassembler.ProcessMessage(socket_id, chunks[0]);
  ASSERT_TRUE(intermediate.is_value());
  EXPECT_FALSE(intermediate.value().has_value());

  // Corrupt the remaining length on the second chunk.
  chunks[1].set_remaining_length(999);
  auto result = reassembler.ProcessMessage(socket_id, chunks[1]);
  ASSERT_TRUE(result.is_error());
  EXPECT_EQ(result.error().code(), Error::Code::kCastV2InvalidMessage);
}

TEST(MessageChunkerTest, DropMessagesForSocketClearsIncomplete) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  message.set_payload_utf8(std::string(kMaxChunkPayloadSize + 100, 'a'));

  std::vector<CastMessage> chunks =
      ChunkMessage(message, proto::CastMessage_ProtocolVersion_CASTV2_1_3)
          .value();
  ASSERT_EQ(chunks.size(), 2u);

  auto intermediate1 = reassembler.ProcessMessage(socket_id, chunks[0]);
  ASSERT_TRUE(intermediate1.is_value());
  EXPECT_FALSE(intermediate1.value().has_value());

  reassembler.DropMessagesForSocket(socket_id);

  // Second chunk should now be rejected as an invalid continuation (or
  // treated as a lone message which is invalid because continued=false but it
  // was supposed to be continuation).
  auto result = reassembler.ProcessMessage(socket_id, chunks[1]);
  // Since chunk 1 has continued=false and remaining_length=0, as a fresh
  // message it is returned as is with only 100 bytes (not the full
  // message), and the payload is moved to payload_utf8 matching payload_type.
  ASSERT_TRUE(result.is_value());
  ASSERT_TRUE(result.value().has_value());
  EXPECT_EQ(result.value()->payload_utf8().size(), 100u);
  EXPECT_FALSE(result.value()->has_payload_binary());
}

TEST(MessageChunkerTest,
     ProcessMessage_SingleSegmentChunkedStringMessage_MovesPayloadToUtf8) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  message.set_payload_binary("single segment string payload");
  message.set_continued(false);
  message.set_remaining_length(0);

  auto result = reassembler.ProcessMessage(socket_id, message);
  ASSERT_TRUE(result.is_value());
  ASSERT_TRUE(result.value().has_value());
  const CastMessage& processed = result.value().value();
  EXPECT_EQ(processed.payload_type(),
            CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  EXPECT_EQ(processed.payload_utf8(), "single segment string payload");
  EXPECT_FALSE(processed.has_payload_binary());
}

TEST(MessageChunkerTest,
     ProcessMessage_SingleSegmentChunkedBinaryMessage_KeepsPayloadInBinary) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_3);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_BINARY);
  message.set_payload_binary("single segment binary payload");
  message.set_continued(false);
  message.set_remaining_length(0);

  auto result = reassembler.ProcessMessage(socket_id, message);
  ASSERT_TRUE(result.is_value());
  ASSERT_TRUE(result.value().has_value());
  const CastMessage& processed = result.value().value();
  EXPECT_EQ(processed.payload_type(),
            CastMessage_PayloadType::CastMessage_PayloadType_BINARY);
  EXPECT_EQ(processed.payload_binary(), "single segment binary payload");
  EXPECT_FALSE(processed.has_payload_utf8());
}

TEST(MessageChunkerTest,
     ProcessMessage_NormalStringMessage_KeepsPayloadInUtf8) {
  MessageReassembler reassembler;
  const int socket_id = 1;

  CastMessage message;
  message.set_protocol_version(proto::CastMessage_ProtocolVersion_CASTV2_1_0);
  message.set_source_id("source");
  message.set_destination_id("dest");
  message.set_namespace_("namespace");
  message.set_payload_type(
      CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  message.set_payload_utf8("normal string message");

  auto result = reassembler.ProcessMessage(socket_id, message);
  ASSERT_TRUE(result.is_value());
  ASSERT_TRUE(result.value().has_value());
  const CastMessage& processed = result.value().value();
  EXPECT_EQ(processed.payload_type(),
            CastMessage_PayloadType::CastMessage_PayloadType_STRING);
  EXPECT_EQ(processed.payload_utf8(), "normal string message");
  EXPECT_FALSE(processed.has_payload_binary());
}

}  // namespace openscreen::cast
