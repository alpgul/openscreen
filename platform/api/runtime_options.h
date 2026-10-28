// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PLATFORM_API_RUNTIME_OPTIONS_H_
#define PLATFORM_API_RUNTIME_OPTIONS_H_

namespace openscreen {

// Options that let the embedder choose between implementations at runtime, for
// example to roll out a new implementation behind a feature flag.
//
// The default value of each field is the library's recommended setting.
// Embedders that are happy with the defaults do not need to do anything. To
// change some of them, start from the defaults and set only the fields you
// want to control:
//
//   RuntimeOptions options;
//   options.use_rust_json = false;
//   SetRuntimeOptions(options);
//
// When a new field is added, embedders get its default until they choose to
// set it.
struct RuntimeOptions {
  // Use the Rust serde_json_lenient backend for json::Parse() and
  // json::Stringify(), instead of jsoncpp. Has no effect if Open Screen was
  // built with `use_json_serde = false`.
  bool use_rust_json = true;

  // Use the Rust wire parser for Cast Streaming RTP and RTCP packets, instead
  // of the C++ parser. Has no effect if Open Screen was built with
  // `use_rust_rtp_parser = false`.
  bool use_rust_rtp_parser = true;
};

// Returns the options in effect: the defaults, or the last value passed to
// SetRuntimeOptions(). Can be called on any thread.
RuntimeOptions GetRuntimeOptions();

// Replaces the options in effect. Can be called on any thread.
//
// Open Screen reads the options when it creates the objects that depend on
// them. For example, a Cast Streaming sender or receiver picks its packet
// parsers when it is created, and keeps them for its whole life. So call this
// before creating those objects, normally once at startup.
void SetRuntimeOptions(const RuntimeOptions& options);

}  // namespace openscreen

#endif  // PLATFORM_API_RUNTIME_OPTIONS_H_
