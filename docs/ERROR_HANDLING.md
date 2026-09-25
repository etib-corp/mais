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
| `FunctionNotFound`   | The module has no such function                              | Use `callOptional()` for hooks |
| `TypeMismatch`       | A value could not be converted, or the attribute is not callable | Fix the argument or the script |
| `InvocationFailed`   | The script raised a Python exception                          | Log `traceback()` and continue |

Helpers: `isOk`, `isRecoverable`, `isFatal`, and `errorCodeName`.

## Consumer contract

Every fallible method returns `mais::Error`:

- `ScriptRuntime::initialize()` / `shutdown()`
- `ScriptRuntime::registerModule()`
- `ScriptRuntime::addSearchPath()` / `loadModule()`
- `ScriptRuntime::call()` / `callOptional()`

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
fills both halves of the error:

- `Error::message()` — the Python message, for example
  `ValueError: boom from Python`.
- `Error::traceback()` — the traceback formatted by
  `traceback.format_exception()`, including the script file and line.

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
