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
- Pass a host-owned object to a hook with `ScriptArgument::native()`: the argument carries the object's address and a host-written converter, so pybind11 stays out of `headers/mais/`. The runtime never takes ownership, the object must outlive the runtime, and its class must be registered by a binding callback before the call. Conversion failures return `InvalidArgument` or `TypeMismatch`, never a crash.
- Define explicit return values, exception translation, and diagnostics for script calls. Preserve Python tracebacks.
- Avoid exposing pybind11 types in public headers unless consumers truly need them; this reduces coupling and makes the package easier to evolve.

## Ownership and threading

- The host owns native engine objects; Python receives references or handles only with a documented lifetime policy.
- A single process-wide interpreter owner is the initial default. Additional interpreter and subinterpreter behavior requires separate design and tests.
- **Decided:** the thread that calls `initialize()` owns the interpreter, and maïs keeps the GIL on it. Every Python-touching method must be called from that thread, host bindings are written for it, and a call from another thread is refused with `WrongThread` rather than blocking. The runtime is not internally synchronized, so its state accessors are single-threaded too.
- **Rejected:** releasing the GIL between calls. It allows cross-thread entry without parallelism and moves interpreter-finalization ordering onto every caller.
- **Deferred:** an explicit dispatch queue onto the interpreter thread. It requires a host pump point, ordered delivery, result hand-off, and a shutdown drain rule; it is tracked as MAIS-4.1 and gated on an answered Q5 in `ROADMAP.md`. The full rationale is in [THREADING.md](THREADING.md).
- Drop Python callbacks and wrappers before interpreter finalization, and finalize the interpreter before destroying native objects they can reference.
- C++ owns the application's main loop. Scripts execute at host-defined points; avoid giving Python an uncontrolled blocking `run()` entry point.
- Failure policy and timing belong to the runtime, not to each consumer: the per-call
  `FailurePolicy` and the measured `CallReport` keep the same continue/stop/report-once
  decision out of every host. The host still owns the loop and the shutdown order, so
  `Stop` only asks it to leave the loop; maïs never stops the interpreter on its own.

## Call policy and timing surface

- **Decided:** a per-call `FailurePolicy` is the final shape for the untyped
  `call()`/`callOptional()` calls. The policy belongs at the call site because
  only the host knows a hook's identity, how often it runs, and whether its
  failure may end the loop; defaults leave the pre-1.0 signatures unchanged, so
  a host that never asks for a policy adds no reporting decision to its loop.
- **Accepted cost:** four policy-bearing overloads next to the existing eight.
  A `CallOptions` struct was rejected for `1.0.0` because it would either change
  every signature or duplicate them. Consolidating into `CallOptions` is the
  fallback if the surface must grow again, and it belongs to the API-freeze
  review (MAIS-1.9) rather than to a feature branch.
- **Decided:** there is no `call<T>(FailurePolicy, ...)`. A value-returning call
  is a query, not the per-frame failure path the policy exists for: `Stop` has
  nothing to mean there, and a suppressed value has no defensible meaning.
  Timing a value-returning call inside the runtime is a follow-up, taken only if
  a consumer needs it; a caller can already time the typed call itself.
- Every overload updates the per-hook counters, so the accounting describes the
  hook and not the overload a host happened to use. `ReportOnce` can therefore
  suppress a failure that an `Error`-returning call already reported; see
  [ERROR_HANDLING.md](ERROR_HANDLING.md#adopting-a-policy-mid-session).

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
- Multi-interpreter or multi-threaded callback execution: the threading model is decided in [THREADING.md](THREADING.md), and cross-thread dispatch needs its own design before any code (MAIS-4.1).
- Engine-specific binding API breadth.
- Whether a second scripting language merits a shared language-neutral interface.

Each deferred item should receive explicit requirements, tests, and ownership rules before implementation.
