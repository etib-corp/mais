# maïs Technical Choices

This document records the intended choices for a generic C++ scripting library. Verify toolchain versions and existing code before treating a proposal here as implemented behavior.

## Language and runtime

- Use C++ for the host-facing runtime and Python as the first embedded scripting language, via pybind11.
- Align the C++ standard with the actual repository; C++20 was specified in the earlier source document, but must be confirmed for maïs.
- Keep the language-specific implementation behind a small host-facing API where practical. Supporting another scripting language is a possible future design goal, not a requirement for the first version.
- Own interpreter initialization/finalization explicitly. Define whether a runtime exclusively owns Python or can attach to an existing interpreter; do not silently mix both modes.

## Dependency boundaries

maïs depends on Python/pybind11, not on a consumer, engine, renderer, or UI toolkit. `guillaume` composes maïs and `evan`. If multiple applications need the same engine bindings, place them in a separate `evan-scripting` adapter depending on both packages. `evan` stays independent of maïs.

Do not require an `IEngine` interface in the generic scripting API for a single integration. An application-side adapter or host-owned facade can expose only the engine operations scripts actually need.

## Binding model

- Start with a minimal vertical slice: expose one host-owned fake service to a script and invoke it from Python.
- Choose embedded-module registration versus a separately built Python extension deliberately. Embedded pybind11 modules must be registered before Python initializes; separately built extensions need import-path and packaging rules.
- Bind narrow, stable configuration and callback APIs rather than internal engine classes or a Python-side `Engine()` constructor that duplicates the host's engine.
- Define explicit return values, exception translation, and diagnostics for script calls. Preserve Python tracebacks.
- Avoid exposing pybind11 types in public headers unless consumers truly need them; this reduces coupling and makes the package easier to evolve.

## Ownership and threading

- The host owns native engine objects; Python receives references or handles only with a documented lifetime policy.
- A single process-wide interpreter owner is the initial default. Additional interpreter and subinterpreter behavior requires separate design and tests.
- Access Python objects while holding the GIL. Run script callbacks on a known thread unless a deliberate cross-thread dispatch model is implemented.
- Drop Python callbacks and wrappers before interpreter finalization, and finalize the interpreter before destroying native objects they can reference.
- C++ owns the application's main loop. Scripts execute at host-defined points; avoid giving Python an uncontrolled blocking `run()` entry point.

## Configuration strategy

`evan` should expose native C++ configuration at the correct point in its lifecycle. The consumer can offer Python a restricted configuration object, validate script-supplied values, then apply them before engine initialization. Runtime settings, if supported, should use a separate API with clear thread and lifecycle rules. maïs only executes hooks and transports errors; it does not interpret engine settings.

## UI strategy

maïs contains no UI. A consumer can implement `Panel`, `Button`, `Text`, `Input`, `Console`, and `EngineSettings` around an application controller. Defer `SceneViewport` until the real rendering backend and window ownership are known. Do not select Qt/QML, ImGui, or another framework on behalf of the scripting library.

## Build model

- Build maïs as its own CMake project and export a reusable, target-scoped CMake package if distribution to other projects is required.
- Locate a compatible Python interpreter and development installation; decide how pybind11 is acquired according to the repository's verified dependency policy.
- Keep tests and examples optional. Test against a fake host without `evan` and with no Vulkan, OpenXR, or GLFW dependencies.
- Avoid reusing the old rendering project's one-platform/one-backend flags: they are unrelated to the scripting runtime.
- Specify supported Python/C++/CMake versions, static/shared-library behavior, installation layout, and packaging once validated by CI and an external consumer test.

## Deferred decisions

- Hot reloading and state migration.
- Python sandboxing and untrusted-script execution.
- Multi-interpreter or multi-threaded callback execution.
- Engine-specific binding API breadth.
- Whether a second scripting language merits a shared language-neutral interface.

Each deferred item should receive explicit requirements, tests, and ownership rules before implementation.
