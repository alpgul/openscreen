// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Cast standalone sender and receiver end-to-end integration test suite.

mod runner;

use runner::{find_sanitizer_report, CastProcessRunner, ProcessOutput};
use rust_gtest_interop::prelude::*;
use std::time::Duration;

/// Generous timeout for playing out a 10-second test video stream.
const TEST_TIMEOUT: Duration = Duration::from_secs(60);

/// Logged by the receiver once it has set up playback for a negotiated codec.
/// With the external media libraries, this happens when the first frame of
/// that stream is decoded.
const FOUND_OPUS: &str = "Found codec: opus (";
const FOUND_VP8: &str = "Found codec: vp8 (";
const FOUND_VP9: &str = "Found codec: vp9 (";

/// Expects that neither of a process's output streams contains a sanitizer
/// error report or a fatal log message.
fn expect_no_errors(name: &str, output: &ProcessOutput) {
    for (stream_name, log) in [("stderr", &output.stderr), ("stdout", &output.stdout)] {
        let sanitizer_report = find_sanitizer_report(log);
        expect_true!(
            sanitizer_report.is_none(),
            "{name} {stream_name} contains a sanitizer report ({:?}):\n{log}",
            sanitizer_report
        );
        expect_false!(
            log.contains("[FATAL:"),
            "{name} {stream_name} contains a FATAL error:\n{log}"
        );
    }
}

/// Executes a complete sender-receiver session, and asserts on session
/// negotiation, the `expected_receiver_logs`, and clean teardown of both
/// processes.
fn run_e2e_session(
    extra_sender_args: &[&str],
    expected_receiver_logs: &[&str],
) -> Result<(), Box<dyn std::error::Error>> {
    let mut runner = CastProcessRunner::new()?;
    runner.start_receiver()?;
    let sender_output = runner.run_sender(extra_sender_args, TEST_TIMEOUT)?;
    let receiver_output = runner.stop_receiver()?;

    expect_no_errors("Sender", &sender_output);
    expect_no_errors("Receiver", &receiver_output);

    expect_true!(
        sender_output.success(),
        "Sender exited with status {:?}.\nStderr:\n{}\nStdout:\n{}",
        sender_output.exit_code,
        sender_output.stderr,
        sender_output.stdout
    );
    expect_true!(
        receiver_output.success(),
        "Receiver exited with status {:?} after SIGTERM.\nStderr:\n{}\nStdout:\n{}",
        receiver_output.exit_code,
        receiver_output.stderr,
        receiver_output.stdout
    );

    expect_true!(
        receiver_output.stderr.contains("Successfully negotiated a session"),
        "Receiver log missing 'Successfully negotiated a session'. Full log:\n{}",
        receiver_output.stderr
    );
    for expected in expected_receiver_logs {
        expect_true!(
            receiver_output.stderr.contains(expected),
            "Receiver log missing '{expected}'. Full log:\n{}",
            receiver_output.stderr
        );
    }

    // Assert that the sender reached the end of both media streams.
    for track in ["audio", "video"] {
        let marker = format!("The {track} capturer has reached the end of the media stream.");
        expect_true!(
            sender_output.stderr.contains(&marker),
            "Sender log missing '{marker}'. Full log:\n{}",
            sender_output.stderr
        );
    }

    Ok(())
}

/// Validates baseline streaming with the default codecs (VP8 + Opus).
#[gtest(StandaloneE2ETest, GoldenCase)]
fn test_golden_case() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&[], &[FOUND_OPUS, FOUND_VP8])
}

/// Validates explicit VP8 video codec flag.
#[gtest(StandaloneE2ETest, Vp8Codec)]
fn test_vp8_codec() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-c", "vp8"], &[FOUND_OPUS, FOUND_VP8])
}

/// Validates explicit VP9 video codec flag.
#[gtest(StandaloneE2ETest, Vp9Codec)]
fn test_vp9_codec() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-c", "vp9"], &[FOUND_OPUS, FOUND_VP9])
}

/// Validates remoting mode protocol handshake and streaming. In remoting mode,
/// the negotiated stream configs only say REMOTE_AUDIO and REMOTE_VIDEO; the
/// receiver learns the actual codecs from the remoting RPC initialization
/// handshake, so finding them proves that the handshake completed.
#[gtest(StandaloneE2ETest, RemotingMode)]
fn test_remoting_mode() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-r"], &[FOUND_OPUS, FOUND_VP8])
}

/// Validates Android RTP compatibility mode, in which the sender uses
/// non-standard RTP payload types.
#[gtest(StandaloneE2ETest, AndroidHack)]
fn test_android_hack() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-a"], &[FOUND_OPUS, FOUND_VP8])
}
