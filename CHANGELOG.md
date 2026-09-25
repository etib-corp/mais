# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

The renderer API described in earlier revisions of this file was never released
as part of this scripting library. 1.0.0 is the first release of this runtime.

### Added

- `mais::ScriptRuntime`, the host-facing entry point: interpreter lifecycle
  (`initialize()`/`shutdown()`), validated script search paths, module loading,
  and hook invocation through `call()` and `callOptional()`.
- `mais::BindingRegistry` and `mais::PythonModule`: hosts queue embedded Python
  modules before the interpreter starts, and their callbacks run once Python is
  live. This replaces `PYBIND11_EMBEDDED_MODULE` for embedded hosts, so no
  extension module has to be built, installed, or placed on an import path.
- `mais::Error`, `mais::ErrorCode`, `isOk`/`isRecoverable`/`isFatal`, and
  `mais::ScriptArgument`: every fallible operation reports a message plus a
  Python traceback instead of throwing or returning a bare Boolean.
- A standalone fake-host acceptance test: the host injects a C++ service,
  invokes a script that mutates it, and asserts on the reported traceback.
- Lifecycle tests covering API misuse, single-owner-per-process enforcement,
  three sequential restarts, and destruction without an explicit `shutdown()`.
- `examples/standalone_host`, a complete host with no engine, no UI, and no
  dependency on `guillaume` or `evan`. CTest runs it as an integration test.
- A benchmark harness (`BUILD_BENCHMARKS`) measuring per-call overhead,
  including a full round trip where Python calls back into a host binding.
- Install/export rules so an external project can `find_package(mais)` and link
  `mais::mais`, verified against a separate consumer.

### Changed

- The project is now the generic scripting runtime described in
  `docs/ARCHITECTURE.md`. The earlier Vulkan/OpenXR rendering layer and its
  platform/backend build matrix (`BUILD_FOR_GLFW`, `BUILD_FOR_OPENXR`,
  `BUILD_FOR_LINUX|MACOS|WINDOWS|ANDROID`, `Vulkan::Vulkan`, GLM, stb) are gone
  from the build, along with the `utility` dependency.
- Python discovery is delegated to pybind11 so the interpreter and the linked
  library always come from the same installation. Select one explicitly with
  `-DPython_EXECUTABLE=<path>`.
- `initialize()` is atomic: a failing host binding or search path leaves Python
  stopped rather than handing the host a half-configured runtime.
- Documentation rewritten around the scripting lifecycle: host-owned objects,
  binding ordering, GIL and thread affinity, and shutdown ordering.

### Removed

- The renderer-specific API (`Engine`, `Renderer`, `Scene`, `GPUMesh`,
  `GPUTexture`, `Platform`, `ADeviceBackend`), its examples, and its tests. UI
  components belong to a consumer, not to maïs.
- The Vulkan/NDK/Java prerequisites and the Vulkan SDK setup step in CI. The
  build needs only CMake, a C++20 compiler, and Python development files.
