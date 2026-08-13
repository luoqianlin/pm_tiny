# Changelog

All notable changes to PM_Tiny are documented in this file.

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
