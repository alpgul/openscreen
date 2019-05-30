// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef OSP_BASE_BORINGSSL_UTIL_H_
#define OSP_BASE_BORINGSSL_UTIL_H_

namespace openscreen {

void LogAndClearBoringSslErrors();

// Multiple sequential calls to InitOpenSSL or CleanupOpenSSL are ignored
// by OpenSSL itself.
void InitOpenSSL();
void CleanupOpenSSL();

}  // namespace openscreen

#endif  // OSP_BASE_BORINGSSL_UTIL_H_
