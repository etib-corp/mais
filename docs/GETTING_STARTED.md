# Getting Started

This tutorial walks a host application from an empty project to a running
script. It assumes you have already [built the library](../README.md#building).

## 1. Link maïs

maïs embeds Python, so the host links the *embeddable* interpreter rather than
the extension-module shim. pybind11 provides both through `pybind11::embed`:

```cmake
find_package(mais CONFIG REQUIRED)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE mais::mais pybind11::embed)
```

The host links `pybind11::embed` itself because it writes the binding callbacks
that maïs runs. maïs's own public headers never include pybind11, so a host that
only *starts* scripts and never exposes native objects does not need it.

## 2. Register the services Python may touch

The host keeps ownership of its native objects and decides which ones scripts
can see. Bindings are queued **before** the interpreter starts:

```cpp
#include "mais/ScriptRuntime.hpp"

#include <pybind11/embed.h>

struct HostClock
{
    double deltaSeconds = 0.0;
};

mais::ScriptRuntime runtime;
HostClock clock;

runtime.registerModule("host", [&clock](mais::PythonModule &module) {
    pybind11::module_ &host = module.as<pybind11::module_>();
    pybind11::class_<HostClock>(host, "HostClock")
        .def_readwrite("delta_seconds", &HostClock::deltaSeconds);
    // Python borrows the object; the host still owns it.
    host.attr("clock") =
        pybind11::cast(&clock, pybind11::return_value_policy::reference);
});
```

`registerModule` is maïs's answer to pybind11's statically registered
`PYBIND11_EMBEDDED_MODULE`: the callback runs once the interpreter is live, so
it can create classes and attach host objects without packaging an extension
module. Scripts then reach the host with a plain `import host`.

## 3. Start Python and load a script

```cpp
if (mais::Error error = runtime.initialize(); error) {
    std::cerr << error.toString() << '\n';
    return 1;
}
if (mais::Error error = runtime.addSearchPath("scripts"); error) {
    std::cerr << error.toString() << '\n';
    return 1;
}
if (mais::Error error = runtime.loadModule("game"); error) {
    std::cerr << error.toString() << '\n';
    runtime.shutdown();
    return 1;
}
```

`addSearchPath` accepts only an existing directory, so a typo fails immediately
instead of turning into a confusing import error later.

## 4. Call hooks from the host's loop

The host owns the loop and chooses where scripts run. `callOptional` treats a
missing function as success, which is exactly what an optional hook needs:

```cpp
if (mais::Error error = runtime.callOptional("game", "configure"); error)
    std::cerr << error.toString() << '\n';

for (int frame = 0; frame < frameCount; ++frame) {
    clock.deltaSeconds = advanceFrame();
    runtime.callOptional("game", "on_update",
                         {mais::ScriptArgument::number(clock.deltaSeconds)});
}

runtime.callOptional("game", "on_shutdown");
```

The matching script:

```python
import host


def configure():
    host.settings.window_title = "my_app"


def on_start():
    ...


def on_update(delta_seconds):
    host.clock.delta_seconds = delta_seconds
```

Arguments are scalars only (`int`, `float`, `bool`, `str`) in this version, so a
hook that needs a native object reads it from the host module instead of
receiving it as a parameter.

## 5. Report failures with the traceback

Every fallible call returns a `mais::Error`, and a script error never unwinds
the host's loop as a C++ exception:

```cpp
if (mais::Error error = runtime.call("game", "on_update"); error) {
    if (mais::isFatal(error.code())) {
        break;                                // the interpreter is unusable
    }
    std::cerr << error.toString() << '\n';    // message + Python traceback
}
```

`callOptional` hides only the "function does not exist" case; a hook that exists
and raises still reports its failure.

## 6. Stop before destroying host objects

The interpreter must outlive every Python reference to a native object, so shut
the runtime down before those objects are destroyed:

```cpp
runtime.shutdown();
```

## Next steps

- [ERROR_HANDLING.md](ERROR_HANDLING.md) for the error taxonomy and tracebacks.
- [ARCHITECTURE.md](ARCHITECTURE.md) for the ownership and dependency rules.
- `examples/standalone_host/` for the complete runnable host.
- Browse the [API reference](https://etib-corp.github.io/mais).
