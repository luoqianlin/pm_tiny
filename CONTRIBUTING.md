# Contributing to PM_Tiny

Thank you for helping improve PM_Tiny.

## Before opening a change

- Search existing issues and describe the target platform and failure mode.
- Keep cross-platform protocol, configuration, dependency, and lifecycle semantics consistent.
- Preserve C++14 compatibility and offline builds.
- Do not add a dependency without documenting its version, license, size, and platform support.

## Build and test

Linux:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPM_TINY_BUILD_TESTS=ON
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

Windows changes must also pass the VS 2022/MSVC x64 test suite. Android changes must build with the NDK; process lifecycle changes should run the isolated device regression scripts with an explicit ADB serial.

## Pull requests

- Keep each pull request focused and explain externally visible behavior changes.
- Add or update tests for bug fixes and new behavior.
- Update the protocol and platform documents when their contracts change.
- Run `git diff --check` before submitting.

By contributing, you agree that your contribution is licensed under the Apache License 2.0.
