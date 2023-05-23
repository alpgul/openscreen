// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "cast/common/public/testing/discovery_utils.h"

#include <sstream>
#include <string>
#include <vector>

#include "util/stringprintf.h"

namespace openscreen {
namespace cast {

const IPAddress kAddressV4 = {192, 168, 0, 0};
const IPAddress kAddressV6 = {1, 2, 3, 4, 5, 6, 7, 8};
const IPEndpoint kEndpointV4 = {kAddressV4, kPort};
const IPEndpoint kEndpointV6 = {kAddressV6, kPort};

discovery::DnsSdTxtRecord CreateValidTxt() {
  discovery::DnsSdTxtRecord txt;
  txt.SetValue(kUniqueIdKey, kTestUniqueId);
  txt.SetValue(kVersionKey, std::to_string(kTestVersion));
  txt.SetValue(kCapabilitiesKey, kCapabilitiesStringLong);
  txt.SetValue(kStatusKey, std::to_string(kStatus));
  txt.SetValue(kFriendlyNameKey, kFriendlyName);
  txt.SetValue(kModelNameKey, kModelName);
  return txt;
}

void CompareTxtString(const discovery::DnsSdTxtRecord& txt,
                      const std::string& key,
                      const std::string& expected) {
  ErrorOr<std::string> value = txt.GetStringValue(key);
  ASSERT_FALSE(value.is_error())
      << "expected value: '" << expected << "' for key: '" << key
      << "'; got error: " << value.error();
  EXPECT_EQ(value.value(), expected)
      << "expected value '" << expected << "' for key: '" << key << "'";
}

void CompareTxtInt(const discovery::DnsSdTxtRecord& txt,
                   const std::string& key,
                   int expected) {
  ErrorOr<std::string> value = txt.GetStringValue(key);
  ASSERT_FALSE(value.is_error())
      << "key: '" << key << "'' expected: '" << expected << "'";
  EXPECT_EQ(value.value(), std::to_string(expected))
      << "for key: '" << key << "'";
}

}  // namespace cast
}  // namespace openscreen
