// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Cast standalone sender and receiver end-to-end integration test suite.

mod runner;

use runner::CastProcessRunner;
use rust_gtest_interop::prelude::*;
use std::time::Duration;

/// Generous timeout for playing out a 10-second test video stream.
const TEST_TIMEOUT: Duration = Duration::from_secs(60);

/// Helper function to execute a complete sender-receiver session and assert
/// on protocol initialization, session negotiation, and clean teardown.
fn run_e2e_session(
    extra_sender_args: &[&str],
    expected_codec_str: Option<&str>,
) -> Result<(), Box<dyn std::error::Error>> {
    let mut runner = CastProcessRunner::new()?;
    runner.start_receiver()?;
    let sender_output = runner.run_sender(extra_sender_args, TEST_TIMEOUT)?;

    expect_true!(
        sender_output.success(),
        "Sender exited with non-zero status: {:?}.\nStderr:\n{}\nStdout:\n{}",
        sender_output.exit_code,
        sender_output.stderr,
        sender_output.stdout
    );

    let receiver_output = runner.stop_receiver()?;

    // Assert that the receiver successfully initialized and negotiated a
    // session.
    expect_true!(
        receiver_output.stderr.contains("CastService is running."),
        "Receiver log missing 'CastService is running.'. Full log:\n{}",
        receiver_output.stderr
    );
    expect_true!(
        receiver_output.stderr.contains("Successfully negotiated a session"),
        "Receiver log missing 'Successfully negotiated a session'. Full log:\n{}",
        receiver_output.stderr
    );

    if let Some(codec) = expected_codec_str {
        expect_true!(
            receiver_output.stderr.contains(codec),
            "Receiver log missing codec match for '{codec}'. Full log:\n{}",
            receiver_output.stderr
        );
    }

    // Assert that the sender reached the end of stream and exited gracefully.
    expect_true!(
        sender_output.stderr.contains("Video complete. Exiting...")
            || sender_output
                .stderr
                .contains("The video capturer has reached the end of the media stream."),
        "Sender log missing completion marker. Full log:\n{}",
        sender_output.stderr
    );

    // Ensure neither process logged fatal errors.
    expect_false!(
        receiver_output.stderr.contains("[FATAL:"),
        "Receiver output contains FATAL error:\n{}",
        receiver_output.stderr
    );
    expect_false!(
        sender_output.stderr.contains("[FATAL:"),
        "Sender output contains FATAL error:\n{}",
        sender_output.stderr
    );

    Ok(())
}

/// Validates baseline golden case streaming (VP8 + Opus).
#[gtest(StandaloneE2e, GoldenCase)]
fn test_golden_case() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&[], Some("opus"))
}

/// Validates explicit VP8 video codec flag.
#[gtest(StandaloneE2e, Vp8Codec)]
fn test_vp8_codec() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-c", "vp8"], Some("vp8"))
}

/// Validates explicit VP9 video codec flag.
#[gtest(StandaloneE2e, Vp9Codec)]
fn test_vp9_codec() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-c", "vp9"], Some("vp9"))
}

/// Validates remoting mode protocol handshake and streaming.
#[gtest(StandaloneE2e, RemotingMode)]
fn test_remoting_mode() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-r"], None)
}

/// Validates Android RTP compatibility mode.
#[gtest(StandaloneE2e, AndroidHack)]
fn test_android_hack() -> Result<(), Box<dyn std::error::Error>> {
    run_e2e_session(&["-a"], None)
}
