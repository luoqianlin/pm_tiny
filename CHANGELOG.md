# Changelog

All notable changes to PM_Tiny are documented in this file.

## [4.0.0] - 2026-08-15

### Added

- SDK 4 with shared C/C++ interfaces, persistent transport workers, reconnect backoff, and explicit `flush` support.
- Structured daemon inspection, bounded control sessions, stronger IPC path validation, and lifecycle/resource cleanup.
- Cross-user Linux/Android dynamic starts with peer-credential identity resolution and controlled launch environments.

### Changed

- Daemon shutdown, process generations, and Windows named-pipe handling are hardened against stale events and resource leaks.
- Dynamic `pm start` no longer relies on interactive `sudo`; root `pm_tiny` performs explicit identity switching.
- Cross-user launches require a path-qualified executable and remove inherited `PATH`, `SUDO_*`, and `LD_*` values before applying explicit `--env` overrides.

[4.0.0]: https://github.com/luoqianlin/pm_tiny/compare/v3.2.0...v4.0.0

## [3.2.0] - 2026-08-14

### Added

- Protocol v3 with structured runtime `start`, daemon inspection, and stable JSON schemas.
- `pm info` / Android `pm2 info` for effective configuration sources, runtime mode, logging state,
  process-tree backend, platform capabilities, PID, and uptime.
- Cross-platform daemon configuration precedence and source tracking for command-line, environment,
  YAML, default, and derived values.
- Transactional runtime configuration persistence with environment sidecars and interrupted-save recovery.
- Shared daemon and managed-program log rotation with observable fallback and retry state.
- Windows foreground and SCM configuration parity, named-pipe security settings, and Job Object reporting.

### Changed

- Dynamic program creation now uses `pm start <name> [options] -- <executable> [args...]` on all platforms.
- Program configuration uses structured `executable` and `args` fields and rejects removed legacy formats.
- Missing, zero-byte, whitespace-only, and comment-only `prog.yaml` files start as an empty configuration;
  `pm save` creates the file when persistence is requested.
- Linux, Android, and Windows share protocol, configuration, inspection, persistence, logging, and renderer contracts.
- Android continues to build the `pm` target while installing the client as `pm2`.

### Removed

- Protocol v2 and its legacy command/configuration compatibility paths.
- Internal development instructions and environment-specific validation paths from the public tree.

## [2.0.0] - 2026-08-13

First public cross-platform release.

### Added

- Shared protocol v2, control commands, process-list schema, dependency DAG, and restart policy.
- Stable dependency ordering with waiting, starting, online, failed, and blocked states.
- Exponential restart backoff, bounded restart windows, ready/tick heartbeats, and timeouts.
- Linux and Android process-tree management through cgroup v2 with process-group fallback.
- Windows VS 2022/MSVC support using named pipes, process groups, Job Objects, and SCM integration.
- Adaptive process tables, JSON status, dependency graph text/JSON/DOT output, inspection, and log streaming.
- Android `pm2` client filename to avoid conflicting with the system package manager.
- Android executables statically link libc++ and do not require `libc++_shared.so` on the device.
- Offline builds with pinned bundled dependencies and Linux, Android, and Windows regression coverage.

### Changed

- Reorganized shared, daemon, CLI, and platform-specific code into explicit subsystem directories.
- Unified CLI diagnostics when the daemon is unavailable or the IPC environment is misconfigured.

### Removed

- Experimental Boost graph test and the Boost development dependency.
- Android installation of the ambiguous `pm` client alias.

[2.0.0]: https://github.com/luoqianlin/pm_tiny/releases/tag/v2.0.0
[3.2.0]: https://github.com/luoqianlin/pm_tiny/compare/v2.0.0...main
