// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DISCOVERY_MDNS_TESTING_MDNS_TEST_UTIL_H_
#define DISCOVERY_MDNS_TESTING_MDNS_TEST_UTIL_H_

#include <initializer_list>
#include <string_view>
#include <vector>

#include "discovery/mdns/public/mdns_records.h"

namespace openscreen::discovery {

inline constexpr IPAddress kFakeARecordAddress = IPAddress(192, 168, 0, 0);
inline constexpr IPAddress kFakeAAAARecordAddress =
    IPAddress(1, 2, 3, 4, 5, 6, 7, 8);
inline constexpr uint16_t kFakeSrvRecordPort = 80;

TxtRecordRdata MakeTxtRecord(std::initializer_list<std::string_view> strings);
std::vector<TxtRecordRdata::Entry> MakeTxtEntries(
    std::initializer_list<std::string_view> strings);

// Methods to create fake MdnsRecord entities for use in UnitTests.
MdnsRecord GetFakePtrRecord(const DomainName& target,
                            std::chrono::seconds ttl = std::chrono::seconds(1));
MdnsRecord GetFakeSrvRecord(const DomainName& name,
                            std::chrono::seconds ttl = std::chrono::seconds(1));
MdnsRecord GetFakeSrvRecord(const DomainName& name,
                            const DomainName& target,
                            std::chrono::seconds ttl = std::chrono::seconds(1));
MdnsRecord GetFakeTxtRecord(const DomainName& name,
                            std::chrono::seconds ttl = std::chrono::seconds(1));
MdnsRecord GetFakeARecord(const DomainName& name,
                          std::chrono::seconds ttl = std::chrono::seconds(1));
MdnsRecord GetFakeAAAARecord(
    const DomainName& name,
    std::chrono::seconds ttl = std::chrono::seconds(1));

}  // namespace openscreen::discovery

#endif  // DISCOVERY_MDNS_TESTING_MDNS_TEST_UTIL_H_
