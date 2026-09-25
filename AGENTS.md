# AGENTS.md — maïs

## Repository purpose

maïs is a reusable C++ scripting runtime, initially embedding Python through pybind11. It must be usable by any host application, independently of `guillaume` (the first consumer) and `evan` (the engine currently being integrated). The library owns generic interpreter lifecycle, script loading and invocation, binding registration, and error reporting. It does not own an engine, an application main loop, or a UI.

The earlier MAIS rendering/XR description and its Vulkan, GLFW, and OpenXR build matrix do **not** describe this repository's intended responsibility. Do not carry engine-specific platform options or renderer dependencies into maïs merely because they appeared in an earlier AGENTS.md.

## Architecture and dependency rules

- maïs may depend on Python/pybind11 and other explicitly approved generic dependencies; it must not include or link `guillaume` or `evan`.
- `evan` must remain usable without maïs or Python.
- `guillaume` is the composition root: it creates the engine, configures the runtime, supplies game scripts, and decides when script hooks execute.
- Engine-specific bindings belong in `guillaume` initially or in an optional, separate `evan-scripting` adapter if reuse justifies it. Such an adapter may depend on both maïs and evan; neither of those projects should depend on the adapter.
- Do not add a mandatory `IEngine` interface to maïs to support one consumer. Prefer generic host binding registration and a narrow consumer-side facade.
- Keep Python-facing APIs deliberate and small; do not expose engine internals by default.

```text
guillaume ──► maïs ──► Python / pybind11
    │
    ├────────► evan
    └────────► optional evan-scripting adapter ──► maïs + evan

maïs -X-> guillaume / evan / application UI
 evan -X-> maïs
```

## Implementation status and assumptions

This file describes the target architecture, not verified repository state. Before making edits, inspect the actual source tree, CMake targets, C++ standard, formatting rules, tests, supported platforms, and existing APIs. The original document stated C++20, CMake 3.10+, FetchContent, GoogleTest, and specific paths/scripts; keep those conventions only where confirmed by this repository. Do not assume the old Vulkan SDK setup, Android configuration, or platform/backend validation applies to the scripting library.

## Suggested project layout

Follow the real repository layout if it already exists. For a new repository, a reasonable starting point is:

```text
CMakeLists.txt
headers/mais/        # Public runtime API; avoid consumer-specific types
sources/             # Python runtime and generic infrastructure
tests/sources/       # Runtime and fake-host integration tests
examples/            # Minimal standalone host with no evan/guillaume
cmake/               # Packaging/configuration helpers, if needed
docs/                # Lifecycle, embedding, and binding documentation
```

Consumer scripts, engine bindings, editor components, and game configuration belong outside maïs. Do not add `guillaume/python/` or `evan` headers to this repository.

## Development priorities

1. Inspect the repository and define a standalone acceptance test: a host invokes a Python function, injects a fake C++ service, and reports a Python exception.
2. Implement interpreter ownership and lifecycle with clear single-owner semantics per process. Prevent invalid reinitialization and document shutdown ordering.
3. Implement script/module loading, configurable search paths, function invocation, and useful Python exception reporting.
4. Provide a host binding registration mechanism. Choose its concrete form only after validating pybind11's embedded-module initialization requirements and the desired packaging model.
5. Test C++-owned object injection, object lifetimes, error handling, repeated calls, and thread/GIL behavior without `evan`.
6. Add installation/exported CMake targets and a standalone example; verify a consumer can link maïs without inheriting engine dependencies.
7. In a separate integration layer, expose minimal `evan` configuration and callbacks to Python. Integrate it into `guillaume` only after the generic runtime passes its own tests.

## Runtime and binding safety

- Keep engine ownership in the host. A Python `Engine()` constructor must not silently create a second instance when the application already owns one.
- Register embedded Python modules before interpreter initialization; if choosing extension modules instead, document how they are built, installed, and imported.
- Define whether the runtime owns the interpreter exclusively or can attach to a host-owned interpreter; do not support both implicitly.
- Hold the GIL when manipulating Python objects; do not invoke Python from arbitrary render/worker threads without an explicit threading design.
- Ensure host-owned C++ objects outlive Python wrappers and callbacks that reference them. Release Python references before finalizing the interpreter.
- Propagate script errors with traceback/context; do not swallow exceptions or turn every failure into an uninformative Boolean.
- Keep C++ in charge of the application loop. Python may provide `configure`, `onStart`, `onUpdate`, and `onShutdown` hooks when the consumer chooses to call them.
- If Python may configure engine startup, validate settings and apply them before engine initialization. Engine configuration is an adapter/consumer concern, not a maïs core dependency.
- Treat hot reload, sandboxing, and multiple subinterpreters as future features requiring explicit design rather than default capabilities.

## UI boundary

maïs does not implement UI widgets. An application UI may expose scripting status, logs, and engine configuration through its own controller/model, independently of the Python runtime implementation. A useful first component set for `guillaume` is `Panel`, `Button`, `Text`, `Input`, `Console`, and `EngineSettings`. Add `SceneViewport` only once the actual renderer/window ownership and UI framework are known. Do not introduce Qt/QML or ImGui into maïs to satisfy UI needs in a consumer.

## C++ conventions

- Follow the repository's verified style. If the supplied conventions still apply: public `.hpp` headers under `headers/mais/`, `.cpp` implementations under `sources/`, tests under `tests/sources/`, `PascalCase` types, `camelCase` methods/variables, `_`-prefixed members, and `mais` namespaces.
- Prefer RAII, explicit ownership, `const` correctness, `enum class`, and exceptions with actionable diagnostics. Avoid raw owning pointers.
- Keep public headers minimal. Use implementation hiding where appropriate to avoid forcing every consumer to include `pybind11/embed.h` and to limit ABI exposure.
- Add tests for public behavior and lifecycle edge cases whenever changing the API.
- Run the repository's formatter if present; verify the configuration rather than assuming the previous 80-column/tab rules still apply.

## CMake conventions

- Use target-scoped includes, compile features/definitions, and link dependencies. Avoid global `include_directories()` and `add_definitions()`.
- Keep Python/pybind11 dependencies private where the public API allows; exported targets must carry any genuinely required transitive dependencies.
- Resolve the supported Python version and interpreter/development-package discovery before writing the production build configuration. Keep the Python executable and linked development library compatible.
- Tests and examples should be optional CMake targets. Do not require GLFW, OpenXR, Vulkan, or renderer-specific platform flags to build the generic library.
- If existing repository conventions use FetchContent, GoogleTest, or `gtest_discover_tests()`, follow them after verifying availability. Do not change dependency versions casually.

## Build and test

Use commands corresponding to the actual CMake options. For a conventional single-config build, start with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

These commands are a baseline, not a claim that the current repository already configures successfully with these flags. If the project retains a formatting helper such as `./scripts/run-clang-format.sh`, use it; otherwise follow the checked-in formatting configuration.

## Editing boundaries

- Never edit generated `build/` output or fetched third-party sources.
- Do not modify `guillaume` or `evan` as a side effect of a maïs-only change unless the task explicitly includes those repositories.
- Treat public API changes, Python ABI/version changes, C++ standard changes, and new dependencies as compatibility decisions: explain their impact and obtain maintainer approval where the project requires it.
- Do not modify CI, platform/toolchain configuration, or release packaging unless the task calls for it; preserve any repository-specific approval rules that actually exist.
- If current code conflicts with this target architecture, report the mismatch and propose the smallest migration rather than assuming a large rewrite is authorized.

## Change checklist

- [ ] Builds and tests independently of `guillaume` and `evan`.
- [ ] A fake-host test covers binding registration, script invocation, and Python traceback reporting.
- [ ] Interpreter, GIL, object ownership, and shutdown behavior are documented and tested.
- [ ] No consumer/engine/UI includes or link dependencies were introduced in maïs.
- [ ] CMake usage remains target-scoped and installed/exported targets work for an external consumer.
- [ ] Formatting and relevant tests pass; documentation reflects public behavior changes.

## Safe example tasks

1. Add a test that injects a fake C++ counter into Python and invokes it from a script.
2. Improve error reporting so a failed Python callback returns a traceback to the host.
3. Add an optional standalone example that uses maïs without `evan` or `guillaume`.
4. Document the binding registration timing and object-lifetime contract.
