// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "gtest/gtest.h"
#include "osp/msgs/osp_messages.h"

using openscreen::msgs::CborEncodeBuffer;
using openscreen::msgs::HttpHeader;
using openscreen::msgs::PresentationConnectionCloseEvent;
using openscreen::msgs::PresentationConnectionMessage;
using openscreen::msgs::PresentationStartRequest;
using openscreen::msgs::PresentationUrlAvailabilityRequest;

namespace openscreen::osp {

TEST(PresentationMessagesTest, EncodeRequestOneUrl) {
  uint8_t buffer[256];
  std::vector<std::string> urls = {"https://example.com/receiver.html"};
  int64_t bytes_out = EncodePresentationUrlAvailabilityRequest(
      PresentationUrlAvailabilityRequest{7, urls}, buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationUrlAvailabilityRequest decoded_request;
  int64_t bytes_read = DecodePresentationUrlAvailabilityRequest(
      buffer, bytes_out, decoded_request);
  ASSERT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(7u, decoded_request.request_id);
  EXPECT_EQ(urls, decoded_request.urls);
}

TEST(PresentationMessagesTest, EncodeRequestMultipleUrls) {
  uint8_t buffer[256];
  std::vector<std::string> urls{"https://example.com/receiver.html",
                                "https://openscreen.org/demo_receiver.html",
                                "https://turt.le/asdfXCV"};
  int64_t bytes_out = EncodePresentationUrlAvailabilityRequest(
      PresentationUrlAvailabilityRequest{7, urls}, buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationUrlAvailabilityRequest decoded_request;
  int64_t bytes_read = DecodePresentationUrlAvailabilityRequest(
      buffer, bytes_out, decoded_request);
  ASSERT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(7u, decoded_request.request_id);
  EXPECT_EQ(urls, decoded_request.urls);
}

TEST(PresentationMessagesTest, EncodeWouldOverflow) {
  uint8_t buffer[40];
  std::vector<std::string> urls = {"https://example.com/receiver.html"};
  int64_t bytes_out = EncodePresentationUrlAvailabilityRequest(
      PresentationUrlAvailabilityRequest{7, urls}, buffer, sizeof(buffer));
  ASSERT_GT(bytes_out, static_cast<int64_t>(sizeof(buffer)));
}

// TODO(btolsch): Expand invalid utf8 testing to good/bad files and fuzzing.
TEST(PresentationMessagesTest, EncodeInvalidUtf8) {
  uint8_t buffer[256];
  std::vector<std::string> urls = {"\xc0"};
  int64_t bytes_out = EncodePresentationUrlAvailabilityRequest(
      PresentationUrlAvailabilityRequest{7, urls}, buffer, sizeof(buffer));
  ASSERT_GT(0, bytes_out);
}

TEST(PresentationMessagesTest, DecodeInvalidUtf8) {
  uint8_t buffer[256];
  std::vector<std::string> urls = {"https://example.com/receiver.html"};
  int64_t bytes_out = EncodePresentationUrlAvailabilityRequest(
      PresentationUrlAvailabilityRequest{7, urls}, buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);
  // Manually change a character in the url string to be non-utf8.
  buffer[30] = 0xc0;

  PresentationUrlAvailabilityRequest decoded_request;
  int64_t bytes_read = DecodePresentationUrlAvailabilityRequest(
      buffer, bytes_out, decoded_request);
  ASSERT_GT(0, bytes_read);
}

TEST(PresentationMessagesTest, InitiationRequest) {
  uint8_t buffer[256];
  const std::string kPresentationId = "lksdjfloiqwerlkjasdlfq";
  const std::string kPresentationUrl = "https://example.com/receiver.html";
  std::vector<HttpHeader> headers;
  int64_t bytes_out = EncodePresentationStartRequest(
      PresentationStartRequest{13, kPresentationId, kPresentationUrl, headers},
      buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationStartRequest decoded_request;
  int64_t bytes_read =
      DecodePresentationStartRequest(buffer, bytes_out, decoded_request);
  ASSERT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(13u, decoded_request.request_id);
  EXPECT_EQ(kPresentationId, decoded_request.presentation_id);
  EXPECT_EQ(kPresentationUrl, decoded_request.url);
  EXPECT_EQ(0, (int)decoded_request.headers.size());
}

TEST(PresentationMessagesTest, InitiationRequestWithoutOptional) {
  uint8_t buffer[256];
  const std::string kPresentationId = "lksdjfloiqwerlkjasdlfq";
  const std::string kPresentationUrl = "https://example.com/receiver.html";
  std::vector<HttpHeader> headers;
  int64_t bytes_out = EncodePresentationStartRequest(
      PresentationStartRequest{13, kPresentationId, kPresentationUrl, headers},
      buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationStartRequest decoded_request;
  int64_t bytes_read =
      DecodePresentationStartRequest(buffer, bytes_out, decoded_request);
  ASSERT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(13u, decoded_request.request_id);
  EXPECT_EQ(kPresentationId, decoded_request.presentation_id);
  EXPECT_EQ(kPresentationUrl, decoded_request.url);
  EXPECT_EQ(0, (int)decoded_request.headers.size());
}

TEST(PresentationMessagesTest, EncodeConnectionMessageString) {
  uint8_t buffer[256];
  PresentationConnectionMessage message;
  message.connection_id = 1234;
  message.message.which =
      PresentationConnectionMessage::Message::Which::kString;
  new (&message.message.str) std::string("test message as a string");
  int64_t bytes_out =
      EncodePresentationConnectionMessage(message, buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationConnectionMessage decoded_message;
  int64_t bytes_read =
      DecodePresentationConnectionMessage(buffer, bytes_out, decoded_message);
  ASSERT_GT(bytes_read, 0);
  EXPECT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(message.connection_id, decoded_message.connection_id);
  ASSERT_EQ(message.message.which, decoded_message.message.which);
  EXPECT_EQ(message.message.str, decoded_message.message.str);
}

TEST(PresentationMessagesTest, EncodeConnectionMessageBytes) {
  uint8_t buffer[256];
  PresentationConnectionMessage message;
  message.connection_id = 1234;
  message.message.which = PresentationConnectionMessage::Message::Which::kBytes;
  new (&message.message.bytes)
      std::vector<uint8_t>{0, 1, 2, 3, 255, 254, 253, 86, 71, 0, 0, 1, 0, 2};
  int64_t bytes_out =
      EncodePresentationConnectionMessage(message, buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationConnectionMessage decoded_message;
  int64_t bytes_read =
      DecodePresentationConnectionMessage(buffer, bytes_out, decoded_message);
  ASSERT_GT(bytes_read, 0);
  EXPECT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(message.connection_id, decoded_message.connection_id);
  ASSERT_EQ(message.message.which, decoded_message.message.which);
  EXPECT_EQ(message.message.bytes, decoded_message.message.bytes);
}

TEST(PresentationMessagesTest, CborEncodeBufferSmall) {
  std::vector<std::string> urls = {"https://example.com/receiver.html"};
  PresentationUrlAvailabilityRequest request{7, urls};
  CborEncodeBuffer buffer;
  ASSERT_TRUE(EncodePresentationUrlAvailabilityRequest(request, &buffer));
  EXPECT_LT(buffer.size(), CborEncodeBuffer::kDefaultInitialEncodeBufferSize);

  PresentationUrlAvailabilityRequest decoded_request;
  int64_t bytes_read = DecodePresentationUrlAvailabilityRequest(
      buffer.data() + 1, buffer.size() - 1, decoded_request);
  EXPECT_EQ(static_cast<size_t>(bytes_read), buffer.size() - 1);
  EXPECT_EQ(request.request_id, decoded_request.request_id);
  EXPECT_EQ(request.urls, decoded_request.urls);
}

TEST(PresentationMessagesTest, CborEncodeBufferMedium) {
  std::string url = "https://example.com/receiver.html";
  std::vector<std::string> urls{};
  for (int i = 0; i < 100; ++i) {
    urls.push_back(url);
  }
  PresentationUrlAvailabilityRequest request{7, urls};
  CborEncodeBuffer buffer;
  ASSERT_TRUE(EncodePresentationUrlAvailabilityRequest(request, &buffer));
  EXPECT_GT(buffer.size(), CborEncodeBuffer::kDefaultInitialEncodeBufferSize);

  PresentationUrlAvailabilityRequest decoded_request;
  int64_t bytes_read = DecodePresentationUrlAvailabilityRequest(
      buffer.data() + 1, buffer.size() - 1, decoded_request);
  ASSERT_GT(bytes_read, 0);
  EXPECT_EQ(static_cast<size_t>(bytes_read), buffer.size() - 1);
  EXPECT_EQ(request.request_id, decoded_request.request_id);
  EXPECT_EQ(request.urls, decoded_request.urls);
}

TEST(PresentationMessagesTest, CborEncodeBufferTooLarge) {
  std::vector<std::string> urls = {"https://example.com/receiver.html"};
  PresentationUrlAvailabilityRequest request{7, urls};
  CborEncodeBuffer buffer{10, 30};
  ASSERT_FALSE(EncodePresentationUrlAvailabilityRequest(request, &buffer));
}

TEST(PresentationMessagesTest, EncodePresentationConnectionCloseEvent) {
  uint8_t buffer[256];
  PresentationConnectionCloseEvent event = {
      .connection_id = 1,
      .reason =
          msgs::PresentationConnectionCloseEvent_reason::kCloseMethodCalled,
      .connection_count = 1,
      .has_error_message = false};
  int64_t bytes_out =
      EncodePresentationConnectionCloseEvent(event, buffer, sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationConnectionCloseEvent decoded_event;
  int64_t bytes_read =
      DecodePresentationConnectionCloseEvent(buffer, bytes_out, decoded_event);
  ASSERT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(1u, decoded_event.connection_id);
  EXPECT_EQ(1u, decoded_event.connection_count);

  PresentationConnectionCloseEvent event_with_message = {
      .connection_id = 2,
      .reason =
          msgs::PresentationConnectionCloseEvent_reason::kCloseMethodCalled,
      .connection_count = 2,
      .has_error_message = true,
      .error_message = "test message"};
  bytes_out = EncodePresentationConnectionCloseEvent(event_with_message, buffer,
                                                     sizeof(buffer));
  ASSERT_LE(bytes_out, static_cast<int64_t>(sizeof(buffer)));
  ASSERT_GT(bytes_out, 0);

  PresentationConnectionCloseEvent decoded_event_with_message;
  bytes_read = DecodePresentationConnectionCloseEvent(
      buffer, bytes_out, decoded_event_with_message);
  ASSERT_EQ(bytes_read, bytes_out);
  EXPECT_EQ(2u, decoded_event_with_message.connection_id);
  EXPECT_EQ(2u, decoded_event_with_message.connection_count);
  EXPECT_EQ("test message", decoded_event_with_message.error_message);
}

}  // namespace openscreen::osp
