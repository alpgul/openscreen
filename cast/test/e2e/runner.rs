// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Subprocess orchestration harness for Cast end-to-end integration tests.

use std::fs;
use std::io::Read;
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

static RUNNER_ID: AtomicU64 = AtomicU64::new(1);
static CERT_GEN_LOCK: Mutex<()> = Mutex::new(());

const RECEIVER_BIN_NAME: &str = "cast_receiver";
const SENDER_BIN_NAME: &str = "cast_sender";

/// Returns the path to the executable `name` in `dir`, appending the
/// platform-specific executable suffix (e.g. ".exe" on Windows).
fn executable_path(dir: &Path, name: &str) -> PathBuf {
    dir.join(format!("{name}{}", std::env::consts::EXE_SUFFIX))
}

/// Returns the name of the loopback network interface for the current
/// platform.
// TODO(issuetracker.google.com/236163082): add Windows support once the
// standalone sender and receiver are supported there.
fn loopback_interface_name() -> &'static str {
    if cfg!(target_os = "macos") {
        "lo0"
    } else {
        "lo"
    }
}

/// Output result captured from a spawned child process.
pub struct ProcessOutput {
    /// Process exit code, or `None` if terminated by an OS signal.
    pub exit_code: Option<i32>,
    /// Accumulated standard output stream.
    pub stdout: String,
    /// Accumulated standard error stream.
    pub stderr: String,
}

impl ProcessOutput {
    /// Returns `true` if the process completed with an exit code of 0.
    pub fn success(&self) -> bool {
        self.exit_code == Some(0)
    }
}

/// A wrapper around a spawned child process that continuously drains standard
/// output and standard error on background threads to prevent pipe buffer
/// saturation and deadlocks.
struct ManagedChild {
    child: Child,
    stdout_buf: Arc<Mutex<Vec<u8>>>,
    stderr_buf: Arc<Mutex<Vec<u8>>>,
    stdout_thread: Option<thread::JoinHandle<()>>,
    stderr_thread: Option<thread::JoinHandle<()>>,
}

impl ManagedChild {
    /// Spawns a new child process and initiates background streaming of its
    /// pipes.
    fn spawn(mut cmd: Command) -> Result<Self, String> {
        let mut child = cmd
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .spawn()
            .map_err(|e| format!("Failed to spawn process: {e}"))?;

        let stdout_buf = Arc::new(Mutex::new(Vec::new()));
        let stderr_buf = Arc::new(Mutex::new(Vec::new()));

        fn pump_pipe(
            mut pipe: impl Read + Send + 'static,
            buf: Arc<Mutex<Vec<u8>>>,
        ) -> thread::JoinHandle<()> {
            thread::spawn(move || {
                let mut chunk = [0u8; 4096];
                while let Ok(n) = pipe.read(&mut chunk) {
                    if n == 0 {
                        break;
                    }
                    if let Ok(mut lock) = buf.lock() {
                        lock.extend_from_slice(&chunk[..n]);
                    }
                }
            })
        }

        let stdout_thread = child.stdout.take().map(|stdout| pump_pipe(stdout, stdout_buf.clone()));
        let stderr_thread = child.stderr.take().map(|stderr| pump_pipe(stderr, stderr_buf.clone()));

        Ok(Self { child, stdout_buf, stderr_buf, stdout_thread, stderr_thread })
    }

    /// Returns the captured stdout as a UTF-8 string with sanitized null bytes.
    fn stdout_to_string(&self) -> String {
        self.stdout_buf
            .lock()
            .map(|buf| String::from_utf8_lossy(&buf).replace('\0', "\\0"))
            .unwrap_or_default()
    }

    /// Returns the captured stderr as a UTF-8 string with sanitized null bytes.
    fn stderr_to_string(&self) -> String {
        self.stderr_buf
            .lock()
            .map(|buf| String::from_utf8_lossy(&buf).replace('\0', "\\0"))
            .unwrap_or_default()
    }

    /// Waits for the process to exit up to `timeout`, draining its pipe
    /// streams. On timeout, the process is killed and the error includes
    /// everything it logged.
    fn wait_timeout(&mut self, timeout: Duration) -> Result<ProcessOutput, String> {
        let start = Instant::now();
        loop {
            match self.child.try_wait() {
                Ok(Some(status)) => {
                    self.join_pipe_threads();
                    return Ok(ProcessOutput {
                        exit_code: status.code(),
                        stdout: self.stdout_to_string(),
                        stderr: self.stderr_to_string(),
                    });
                }
                Ok(None) => {
                    if start.elapsed() > timeout {
                        self.kill();
                        return Err(format!(
                            "Process timed out after {timeout:?}.\nStderr:\n{}\nStdout:\n{}",
                            self.stderr_to_string(),
                            self.stdout_to_string()
                        ));
                    }
                    thread::sleep(Duration::from_millis(100));
                }
                Err(e) => return Err(format!("Error waiting for process: {e}")),
            }
        }
    }

    /// Joins the pipe-draining threads. They exit once the process has exited
    /// and its pipes are closed.
    fn join_pipe_threads(&mut self) {
        if let Some(thread) = self.stdout_thread.take() {
            let _ = thread.join();
        }
        if let Some(thread) = self.stderr_thread.take() {
            let _ = thread.join();
        }
    }

    /// Requests process termination via SIGTERM on POSIX (falling back to
    /// SIGKILL on timeout) and collects all captured stream outputs.
    fn terminate_and_collect(mut self, timeout: Duration) -> ProcessOutput {
        // On POSIX systems, send SIGTERM first to allow the child process to
        // perform a graceful shutdown (e.g. flushing streams, running
        // destructors). On non-POSIX platforms (or as a fallback if the
        // process doesn't exit within timeout), use child.kill() which
        // terminates the process immediately.
        #[cfg(unix)]
        // SAFETY:
        // 1. `self.child.id()` returns the positive OS process ID allocated to this active child
        //    process.
        // 2. Sending `SIGTERM` requests a cooperative shutdown and does not cause memory corruption
        //    or violate any Rust safety invariants.
        // 3. Casting `u32` PID to `i32` (`pid_t` in POSIX) is sound for all valid non-negative
        //    process identifiers.
        unsafe {
            libc::kill(self.child.id() as i32, libc::SIGTERM);
        }
        #[cfg(not(unix))]
        {
            let _ = self.child.kill();
        }

        let start = Instant::now();
        let mut exit_code = None;
        loop {
            match self.child.try_wait() {
                Ok(Some(status)) => {
                    exit_code = status.code();
                    break;
                }
                Ok(None) => {
                    if start.elapsed() > timeout {
                        let _ = self.child.kill();
                        let _ = self.child.wait();
                        break;
                    }
                    thread::sleep(Duration::from_millis(50));
                }
                Err(_) => {
                    let _ = self.child.kill();
                    break;
                }
            }
        }

        self.join_pipe_threads();

        ProcessOutput {
            exit_code,
            stdout: self.stdout_to_string(),
            stderr: self.stderr_to_string(),
        }
    }

    /// Forcefully terminates the child process.
    fn kill(&mut self) {
        if let Ok(None) = self.child.try_wait() {
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
        self.join_pipe_threads();
    }
}

impl Drop for ManagedChild {
    fn drop(&mut self) {
        self.kill();
    }
}

/// Markers that sanitizer runtimes print when they detect an error.
const SANITIZER_MARKERS: &[&str] = &[
    "ERROR: AddressSanitizer",
    "ERROR: LeakSanitizer",
    "WARNING: MemorySanitizer",
    "WARNING: ThreadSanitizer",
    "SUMMARY: UndefinedBehaviorSanitizer",
];

/// Returns the first sanitizer error marker found in `log`, if any.
pub fn find_sanitizer_report(log: &str) -> Option<&'static str> {
    SANITIZER_MARKERS.iter().copied().find(|marker| log.contains(marker))
}

/// A uniquely-named temporary directory that is deleted when dropped.
struct TempDir {
    path: PathBuf,
}

impl TempDir {
    fn new() -> Result<Self, String> {
        let id = RUNNER_ID.fetch_add(1, Ordering::SeqCst);
        let path =
            std::env::temp_dir().join(format!("openscreen_e2e_{}_{}", std::process::id(), id));
        fs::create_dir_all(&path).map_err(|e| format!("Failed to create temp dir: {e}"))?;
        Ok(Self { path })
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.path);
    }
}

/// Helper struct for spawning and managing `cast_receiver` and `cast_sender`
/// child processes with RAII cleanup, ephemeral port allocation, and isolated
/// certificates.
pub struct CastProcessRunner {
    build_dir: PathBuf,
    cert_path: PathBuf,
    key_path: PathBuf,
    video_path: PathBuf,
    port: u16,
    receiver: Option<ManagedChild>,
    // Declared last so that it is deleted only after the receiver process,
    // which uses files in it, has been killed by Drop.
    _temp_dir: TempDir,
}

impl CastProcessRunner {
    /// Creates a new `CastProcessRunner`, initializing an isolated temp dir,
    /// dynamic port, and generated TLS certificates.
    pub fn new() -> Result<Self, String> {
        let build_dir = Self::find_build_dir()?;
        let video_path = Self::find_test_video(&build_dir)?;
        let receiver_bin = executable_path(&build_dir, RECEIVER_BIN_NAME);
        if !receiver_bin.exists() {
            return Err(format!("cast_receiver not found at {}", receiver_bin.display()));
        }

        // Create the temp dir first, so it is cleaned up on any later error.
        let temp_dir = TempDir::new()?;
        let port = Self::allocate_free_port()?;
        let cert_path = temp_dir.path.join("generated_root_cast_receiver.crt");
        let key_path = temp_dir.path.join("generated_root_cast_receiver.key");

        // Generate self-signed test credentials in the current working
        // directory, then move them to the isolated temporary
        // directory. We avoid setting current_dir to temp_dir because
        // MSAN binaries on Linux have a relative PT_INTERP (dynamic
        // linker) that fails to resolve outside the repo root.
        let _lock = CERT_GEN_LOCK.lock().unwrap();
        let status = Command::new(&receiver_bin)
            .arg("-g")
            .arg("-v")
            .stdout(Stdio::null())
            .stderr(Stdio::piped())
            .status()
            .map_err(|e| {
                format!(
                    "Failed to run cast_receiver -g with path '{}': {e}",
                    receiver_bin.display()
                )
            })?;

        let cwd_cert = PathBuf::from("generated_root_cast_receiver.crt");
        let cwd_key = PathBuf::from("generated_root_cast_receiver.key");
        if cwd_cert.exists() {
            let _ = fs::copy(&cwd_cert, &cert_path);
            let _ = fs::remove_file(&cwd_cert);
        }
        if cwd_key.exists() {
            let _ = fs::copy(&cwd_key, &key_path);
            let _ = fs::remove_file(&cwd_key);
        }
        drop(_lock);

        if !status.success() || !cert_path.exists() || !key_path.exists() {
            return Err("Failed to generate test certificates with cast_receiver -g".to_string());
        }

        Ok(Self {
            build_dir,
            cert_path,
            key_path,
            video_path,
            port,
            receiver: None,
            _temp_dir: temp_dir,
        })
    }

    /// Spawns the `cast_receiver` process.
    pub fn start_receiver(&mut self) -> Result<(), String> {
        let receiver_bin = executable_path(&self.build_dir, RECEIVER_BIN_NAME);

        let mut cmd = Command::new(receiver_bin);
        cmd.arg("-d")
            .arg(&self.cert_path)
            .arg("-p")
            .arg(&self.key_path)
            .arg("-x")
            .arg("-v")
            .arg("-r")
            .arg(self.port.to_string())
            .arg(loopback_interface_name())
            .env("SDL_VIDEODRIVER", "dummy")
            .env("SDL_AUDIODRIVER", "dummy");

        let mut managed = ManagedChild::spawn(cmd)?;

        // "CastService is running." is logged before the CastService is
        // created, so after seeing it, wait until the receiver is actually
        // accepting connections on its port.
        let start = Instant::now();
        let timeout = Duration::from_secs(15);
        let address = SocketAddr::from(([127, 0, 0, 1], self.port));
        let mut saw_running_log = false;
        loop {
            if let Ok(Some(status)) = managed.child.try_wait() {
                managed.join_pipe_threads();
                return Err(format!(
                    "Receiver exited prematurely with status {status:?}.\nStderr:\n{}\nStdout:\n{}",
                    managed.stderr_to_string(),
                    managed.stdout_to_string()
                ));
            }
            if !saw_running_log {
                saw_running_log = managed.stderr_to_string().contains("CastService is running.");
            }
            if saw_running_log
                && TcpStream::connect_timeout(&address, Duration::from_millis(100)).is_ok()
            {
                break;
            }
            if start.elapsed() > timeout {
                // Kill the process and join its pipe threads so that the
                // captured output is complete.
                managed.kill();
                return Err(format!(
                    "Receiver failed to start listening within {timeout:?}.\nStderr:\n{}\nStdout:\n{}",
                    managed.stderr_to_string(),
                    managed.stdout_to_string()
                ));
            }
            thread::sleep(Duration::from_millis(50));
        }

        self.receiver = Some(managed);
        Ok(())
    }

    /// Spawns the `cast_sender` process, waits for it to finish, and returns
    /// its collected output.
    pub fn run_sender(
        &self,
        extra_args: &[&str],
        timeout: Duration,
    ) -> Result<ProcessOutput, String> {
        let sender_bin = executable_path(&self.build_dir, SENDER_BIN_NAME);
        if !sender_bin.exists() {
            return Err(format!("cast_sender not found at {}", sender_bin.display()));
        }

        let address = format!("127.0.0.1:{}", self.port);
        let mut cmd = Command::new(sender_bin);
        cmd.arg(&address)
            .arg(&self.video_path)
            .arg("-d")
            .arg(&self.cert_path)
            .arg("-n")
            .arg("-v")
            .args(extra_args);

        // `wait_timeout` returns only once the process has exited or been
        // killed, so there is no need to keep `managed` around afterwards.
        ManagedChild::spawn(cmd)?.wait_timeout(timeout)
    }

    /// Stops the `cast_receiver` process gracefully and returns its captured
    /// output.
    pub fn stop_receiver(&mut self) -> Result<ProcessOutput, String> {
        if let Some(receiver) = self.receiver.take() {
            // Give receiver a moment to process final media frames.
            thread::sleep(Duration::from_millis(500));
            Ok(receiver.terminate_and_collect(Duration::from_secs(5)))
        } else {
            Err("Receiver is not running".to_string())
        }
    }

    fn check_build_dir(path: &Path) -> Option<PathBuf> {
        let canon = fs::canonicalize(path).ok()?;
        (executable_path(&canon, SENDER_BIN_NAME).exists()
            && executable_path(&canon, RECEIVER_BIN_NAME).exists())
        .then_some(canon)
    }

    fn find_build_dir() -> Result<PathBuf, String> {
        if let Ok(dir) = std::env::var("OPENSCREEN_BUILD_DIR")
            && let Some(canon) = Self::check_build_dir(Path::new(&dir))
        {
            return Ok(canon);
        }

        if let Ok(exe) = std::env::current_exe()
            && let Some(parent) = exe.parent()
            && let Some(canon) = Self::check_build_dir(parent)
        {
            return Ok(canon);
        }

        for candidate in
            ["out/Default", "out/Debug", "out/Release", "out/msan", "out/asan", "out/tsan", "."]
        {
            if let Some(canon) = Self::check_build_dir(Path::new(candidate)) {
                return Ok(canon);
            }
        }

        if let Ok(entries) = fs::read_dir("out") {
            for entry in entries.flatten() {
                if let Some(canon) = Self::check_build_dir(&entry.path()) {
                    return Ok(canon);
                }
            }
        }

        Err("Could not locate Open Screen build output directory containing cast_sender and cast_receiver".to_string())
    }

    fn find_test_video(build_dir: &Path) -> Result<PathBuf, String> {
        let video_name = "bbb_sunflower_2160p_60fps_normal.mp4";

        // Check build dir first
        let in_build = build_dir.join(video_name);
        if let Ok(canon) = fs::canonicalize(&in_build) {
            return Ok(canon);
        }

        // Check relative test data dir
        let in_repo = PathBuf::from("test/data/cast").join(video_name);
        if let Ok(canon) = fs::canonicalize(&in_repo) {
            return Ok(canon);
        }

        // Walk up from build dir to find repo root
        let mut curr = Some(build_dir);
        while let Some(dir) = curr {
            let candidate = dir.join("test/data/cast").join(video_name);
            if let Ok(canon) = fs::canonicalize(&candidate) {
                return Ok(canon);
            }
            curr = dir.parent();
        }

        Err(format!("Could not locate test video '{video_name}'"))
    }

    fn allocate_free_port() -> Result<u16, String> {
        let listener = TcpListener::bind("127.0.0.1:0")
            .map_err(|e| format!("Failed to bind to free port: {e}"))?;
        let port =
            listener.local_addr().map_err(|e| format!("Failed to get local port: {e}"))?.port();
        drop(listener);
        // Brief settling delay
        thread::sleep(Duration::from_millis(50));
        Ok(port)
    }
}

impl Drop for CastProcessRunner {
    fn drop(&mut self) {
        if let Some(mut child) = self.receiver.take() {
            child.kill();
        }
    }
}
