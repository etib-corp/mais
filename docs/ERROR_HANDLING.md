# Error handling

Every fallible operation returns `mais::Error`, defined in
`headers/mais/Error.hpp`. Nothing in the scripting runtime throws for an
expected failure, so a script that raises an exception cannot unwind the host's
frame loop, and the host always receives enough context to log or display the
problem.

## The taxonomy

| `mais::ErrorCode`    | Meaning                                                     | Handling                       |
| -------------------- | ----------------------------------------------------------- | ------------------------------ |
| `Ok`                 | Success                                                     | Continue                       |
| `NotInitialized`     | Used before `initialize()` or after `shutdown()`            | Fix the call order             |
| `AlreadyInitialized` | `initialize()` or `registerModule()` after startup          | Register bindings first        |
| `InterpreterFailure` | Python could not start/stop, or another interpreter exists   | Fatal: stop scripting          |
| `InvalidArgument`    | Empty name, empty binding callback, empty search path        | Fix the call                   |
| `ScriptNotFound`     | A search path is not an existing directory                   | Fix the path                   |
| `ModuleNotFound`     | A module could not be imported, or was never loaded          | Load it first                  |
| `FunctionNotFound`   | The module has no such function                              | Use `callOptional()` for hooks, or `hasFunction()` to check without invoking |
| `TypeMismatch`       | A value could not be converted (including a native argument), the attribute is not callable, or a returned value has the wrong type | Fix the argument or the script |
| `InvocationFailed`   | The script raised a Python exception                          | Log `traceback()`/`frames()` and continue |

Helpers: `isOk`, `isRecoverable`, `isFatal`, and `errorCodeName`.

## Consumer contract

Every fallible method returns `mais::Error`:

- `ScriptRuntime::initialize()` / `shutdown()`
- `ScriptRuntime::registerModule()`
- `ScriptRuntime::addSearchPath()` / `loadModule()`
- `ScriptRuntime::hasFunction()` / `listFunctions()`
- `ScriptRuntime::call()` / `callOptional()`, and their typed
  `call<T>()` / `callOptional<T>()` overloads

The overloads that take a `mais::FailurePolicy` first return a
`mais::CallReport` instead: the same error plus the measured duration and the
per-hook failure accounting. They are described in
[Call policy and timing](#call-policy-and-timing).

A default-constructed `Error` is `Ok`, and `explicit operator bool()` reports
failure, so the check reads the same way as `std::error_code`:

```cpp
while (host.isRunning()) {
    host.processFrame();

    if (mais::Error error = runtime.callOptional("game", "on_update"); error) {
        std::cerr << error.toString() << '\n';
        if (mais::isFatal(error.code())) {
            break;  // the interpreter is unusable; scripting must stop
        }
        // Recoverable: the host keeps running and logs the failure.
    }
}
```

Only `InterpreterFailure` is fatal. Every other code means the interpreter is
still usable, so the host decides whether to retry, ignore, or stop.

## Python exceptions

When a script raises, the runtime captures the exception pybind11 reports and
fills the diagnostic fields:

- `Error::message()` — the Python message, for example
  `ValueError: boom from Python`.
- `Error::traceback()` — the traceback formatted by
  `traceback.format_exception()`, including the script file and line.
- `Error::exceptionType()` — the Python type name, for example
  `ValueError`; empty when no Python exception was captured.
- `Error::frames()` — the traceback as structured data, described below.

```cpp
mais::Error error = runtime.call("game", "explode");
if (error) {
    std::cerr << error.toString();        // "[InvocationFailed] ValueError: ..."
    std::cerr << error.message() << '\n';
    std::cerr << error.traceback() << '\n';  // empty when none was captured
}
```

Tracebacks are best-effort. A failure with no Python frame (a failed import
argument check, for example) still yields a message, and `hasTraceback()`
reports which case you are in.

### Structured frames

`traceback()` is for logging. A console that needs to jump to source reads the
same exception as data:

| Accessor | Meaning |
| --- | --- |
| `Error::exceptionType()` | Python type name, for example `ValueError` |
| `Error::frames()` | Up to `Error::maxFrames` frames, outermost first |

Each `mais::Error::Frame` carries:

- `file` — the path Python reported, exactly as written in `traceback()`;
- `line` — the 1-based line, or `0` when Python reported none;
- `function` — the function name, or `<module>` for module-level code.

```cpp
if (error) {
    std::cerr << error.exceptionType() << ": " << error.message() << '\n';
    for (const mais::Error::Frame &frame : error.frames()) {
        std::cerr << frame.file << ':' << frame.line << " in "
                  << frame.function << '\n';
    }
}
```

Frames are copied from the traceback object pybind11 already captured, so
building them runs no Python code and reads no source file. `frames().back()`
is the raise site, because the order matches `traceback.format_exception()`.

The cap applies to the structured list only. When a traceback is deeper than
`Error::maxFrames` (32), the innermost frames are kept so the raise site is
never lost; `traceback()` is produced exactly as before, including CPython's
own formatting and any repeated-frame folding. A non-Python error, such as a
failed path check, has no frames and no exception type; `hasTraceback()`
continues to report whether formatted text was captured.

## Native arguments

`ScriptArgument::native()` hands a host-owned object to a call through a
converter the host writes in a pybind11 translation unit. Conversion failures
never crash and never leave a Python error set for the next call:

| Situation                                   | `ErrorCode` | Notes |
| ------------------------------------------- | --- | --- |
| The argument has no converter               | `InvalidArgument` | Rejected before any Python call |
| The converter returns no object             | `TypeMismatch` | The host refused that value |
| The class was never registered by a binding | `TypeMismatch` | pybind11 reports an unregistered type |
| The converter raises                        | `TypeMismatch`, or the classified Python error | The message and traceback are preserved |

Every case is recoverable, so the host loop keeps running. The lifetime rule
is the one in `ScriptRuntime`'s notes: maïs never takes ownership, but the
wrapper it hands Python may outlive the call, so the host object must outlive
the runtime.

## Typed return values

The typed overloads `call<T>()` and `callOptional<T>()` read what a script
returns; `T` is one of `bool`, `std::int64_t`, `double`, or `std::string`. The
result is a `mais::Result<T>` with three states, still reported through
`Error`:

| Script outcome | `Result<T>` state | `error().code()` |
| --- | --- | --- |
| returns a `T` | `hasValue()` | `Ok` |
| returns `None` | `isOk()` without a value | `Ok` |
| hook missing (`callOptional<T>`) | `isOk()` without a value | `Ok` |
| hook missing (`call<T>`) | no value | `FunctionNotFound` |
| returns another type | no value | `TypeMismatch` |
| raises | no value | `InvocationFailed` + traceback |

`None` is therefore never confused with a failure: it means "the script has no
opinion", and the host keeps its defaults. The conversion is strict — `True`
and `False` are the only `bool`s, an `int` is not accepted for `double`, and a
`float` is never truncated to an integer. A mismatch reports `TypeMismatch`
whose message names the function, the returned type, and the requested one:

```text
'game.configure' returned float, but bool was requested
```

## Call policy and timing

The `Error`-returning `call()` and `callOptional()` overloads report every
failure. A host that runs a session for hours usually wants one policy instead
of an `if` around each call, and a number it can attribute to the hook. The
policy-bearing overloads take a `mais::FailurePolicy` first and return a
`mais::CallReport`:

| `FailurePolicy` | On success | On failure |
| --- | --- | --- |
| `Continue` | `isOk()` | the error, every time |
| `Stop` | `isOk()` | the error, with `stopRequested` set |
| `ReportOnce` | `isOk()` | the error once per streak, then `isOk()` with `suppressed` and no error |

```cpp
mais::CallReport report = runtime.callOptional(
    mais::FailurePolicy::ReportOnce, "game", "on_update",
    {mais::ScriptArgument::number(deltaSeconds)});

if (report.stopRequested) {
    break;                       // only under FailurePolicy::Stop
}
if (report) {
    console.log(report.error);   // once per failure streak
}
console.timing("on_update", report.duration);
```

A suppressed repeat carries an `Ok` error, so `if (report)` logs the first
failure and nothing after it. The report keeps the accounting for a console
that wants to show how many were folded away: `consecutiveFailures` and
`suppressed`. The overloads with a policy are the only ones that measure and
suppress; the `Error`-returning overloads keep their previous behaviour and
their per-call cost. A host that pairs a policy with the names from
`listFunctions()` has a per-hook policy without a table of its own.

`FailurePolicy::Stop` never stops the interpreter and never throws: it only sets
`stopRequested`, so the host leaves its loop when it is ready and calls
`shutdown()` in its own order. A repeatedly failing hook therefore cannot end
the loop unless the host opts into `Stop` and honours that flag.

### What "repeated" means

A failure repeats when the same hook (module and function) fails consecutively
with the same `ErrorCode`, Python exception type, and message. The traceback is
not part of that identity: a statement can move between lines while the failure
stays the same failure, so the message distinguishes causes and the code
distinguishes categories.

- A **changed** message, exception type, or code is a new failure and is
  reported again, even though the hook is the same.
- A **successful** call of that hook ends its streak, so a hook that recovers
  and breaks again is reported again.
- Each hook has its own counters: a failure of `on_start` never suppresses one
  of `on_update`.

### Where the counters live

Failure counters belong to one run of one `ScriptRuntime`:

- `initialize()` and `shutdown()` clear them, so a restart never inherits a
  suppression from the previous run.
- A second runtime never sees them; nothing is process-wide or stored on disk.
- Every call updates them, whichever overload made it, but only the
  policy-bearing overloads decide what to report. A call rejected before the
  hook runs (empty name, runtime not initialized) is reported but not counted,
  so `consecutiveFailures` stays zero for it.
- They are not thread-safe, like every other part of the runtime: use the
  thread that called `initialize()`.

### Reading the duration

`CallReport::duration` is a `std::chrono::nanoseconds` measured with
`std::chrono::steady_clock`. The clock spans validation, argument conversion,
the Python call, and the capture of its failure, so the number is what the host
pays for that hook rather than a benchmark estimate. A console can chart it per
hook and a bench cannot disagree with it: `benchmarks/benchmark_main.cpp`
reports the same value as the `runtime_ns_per_call` counter.

## Inspecting a module without running it

`callOptional()` cannot answer "does this hook exist?" without running the
hook: when it is present the probe executes it, and when it is absent the `Ok`
result looks exactly like a hook that returned `None`. A host that needs the
answer before invoking must use `hasFunction()`, which only reads the module
namespace.

```cpp
mais::Result<bool> present = runtime.hasFunction("game", "on_start");
if (!present.isOk()) {
    log(present.error());          // ModuleNotFound, TypeMismatch, ...
} else if (present.value()) {
    mais::Error error = runtime.call("game", "on_start");  // exactly once
    if (error) {
        log(error);
    }
}
```

`hasFunction()` distinguishes the three namespace cases:

| Namespace entry | Result |
| --- | --- |
| callable | `isOk()`, `value() == true` |
| absent | `isOk()`, `value() == false` |
| non-callable | `TypeMismatch`, the same `call()` reports |
| module never loaded | `ModuleNotFound` |

On success `hasValue()` is true for both `true` and `false`: an absent name is
an answer, not a missing result.

`listFunctions()` returns the public callables of a loaded module, sorted
byte-wise so a console can render the list directly:

```cpp
mais::Result<std::vector<std::string>> hooks =
    runtime.listFunctions("game");
if (hooks.isOk()) {
    for (const std::string &name: hooks.value()) {
        console.add(name);
    }
}
```

The listing is a namespace view: non-callable attributes and names beginning
with `_` are omitted, while a public callable imported into the module (for
example `from math import sqrt`) appears. Introspection never invokes a value,
never triggers a module-level `__getattr__` or `__dir__`, and never imports a
module. Consequently, a callable supplied dynamically by `__getattr__` is
reported absent even though `call()` may find it; that is the price of not
running script code. Answers are snapshots: a later script call may change the
namespace.

## Errors versus exceptions

`ScriptRuntime` methods report through `Error` rather than throwing.
`BindingRegistry::add()` throws `std::invalid_argument` for a programming error
such as an empty module name, and `ScriptRuntime::registerModule()` converts
that into `InvalidArgument` so hosts see one error style.

## Failure atomicity

`initialize()` is atomic: if a host binding throws, or a queued search path is
rejected, the interpreter is stopped again and the runtime is left
uninitialized. A host therefore never has to reason about a half-configured
runtime.
