# maïs

maïs is a reusable C++ scripting runtime for applications that embed Python through pybind11. It manages the generic scripting lifecycle while leaving engine integration, application scripts, and UI to the host. `guillaume` is the first intended consumer; `evan` is an engine it may connect to through a separate adapter.

## What it provides

- Ownership and lifecycle management for an embedded Python interpreter.
- Script discovery, loading, and invocation of application-defined hooks.
- A host binding-registration mechanism for exposing selected C++ services to Python.
- Exception reporting with useful Python traceback information.
- A standalone test/example host with no dependency on `guillaume` or `evan`.

maïs does **not** create an engine, own the frame loop, render UI, or package a consumer's Python scripts.

## Key concepts

- **Runtime** — starts and stops Python, manages paths, and invokes script functions.
- **Host** — the application that owns the runtime, its native objects, and the execution loop.
- **Binding adapter** — optional integration code that exposes a narrow host API to Python. An `evan` adapter belongs in `guillaume` or a separate integration package, not in maïs.
- **Script hooks** — callbacks chosen by the host, such as `configure`, `on_start`, `on_update`, and `on_shutdown`; they are not mandatory engine features.
- **Ownership boundary** — host-owned C++ objects must outlive Python references to them; Python objects must be released before interpreter shutdown.

## Architecture

```text
guillaume (host) ──► maïs ──► Python / pybind11
      │
      ├────────────► evan
      └────────────► optional evan adapter ──► evan + maïs

maïs does not depend on guillaume, evan, or a UI toolkit.
```

The host creates the engine and stays in control of its main loop. If Python configures engine startup, the host passes a configuration facade into a script, validates the result, and applies it before engine initialization. Python should not construct a duplicate engine implicitly.

## Usage

A host typically:

1. Creates its native services and configuration.
2. Queues bindings for the services it chooses to expose, **before** Python starts.
3. Starts maïs and adds the application's script directory.
4. Loads its script module and invokes hooks at the points it chooses in its loop.
5. Stops Python before destroying the native services those scripts referenced.

```cpp
mais::ScriptRuntime runtime;

runtime.registerModule("host", [&services](mais::PythonModule &module) {
    pybind11::module_ &host = module.as<pybind11::module_>();
    pybind11::class_<WindowSettings>(host, "WindowSettings")
        .def_readwrite("title", &WindowSettings::title);
    // The host keeps ownership; Python only borrows the object.
    host.attr("settings") =
        pybind11::cast(&services.settings, pybind11::return_value_policy::reference);
});

runtime.initialize();
runtime.addSearchPath("scripts");
runtime.loadModule("game");
runtime.callOptional("game", "configure");
runtime.callOptional("game", "on_start");
// ... the host's own loop ...
runtime.callOptional("game", "on_update", {mais::ScriptArgument::number(deltaSeconds)});
runtime.callOptional("game", "on_shutdown");
runtime.shutdown();
```

The script reaches the host through `import host`:

```python
import host


def configure():
    host.settings.title = "Guillaume"


def on_update(delta_seconds):
    pass
```

`configure`, `on_start`, `on_update` and `on_shutdown` are the hook names the
standalone example chooses to call. They are host-defined names, not features
maïs enforces: any function in any loaded module can be invoked.

`callOptional` reports a missing function as success, which is what an optional
hook needs. `call` reports `FunctionNotFound` instead, so a misspelled hook is
not silently ignored.

## Building

Requirements are CMake 3.20 or newer, a C++20 compiler, and a Python
installation with development files — headers and the embeddable library, which
is `python3-dev` on Debian and Ubuntu. pybind11 is fetched automatically when it
is not already installed. No Vulkan SDK, OpenXR, GLFW, or engine-specific
platform selection is required.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPython_EXECUTABLE="$(command -v python3)"
cmake --build build
ctest --test-dir build --output-on-failure
```

Optional extras:

```sh
cmake -S . -B build -DBUILD_EXAMPLES=ON -DBUILD_BENCHMARKS=ON
cmake --build build
./build/examples/standalone_host/standalone_host examples/standalone_host/scripts
```

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Build and register the GoogleTest suite. |
| `BUILD_EXAMPLES` | `OFF` | Build the standalone host samples. |
| `BUILD_BENCHMARKS` | `OFF` | Build the Google Benchmark harness. |
| `BUILD_DOCS` | `OFF` | Build the Doxygen API reference. |
| `DEV_MODE` | `OFF` | Compile with `-Wall -Wextra -Wpedantic -Werror`. |
| `MAIS_FETCH_PYBIND11` | `ON` | Fetch pybind11 when it is not installed. |

Python discovery is delegated to pybind11 so that the interpreter and the
linked library cannot come from different installations. `ScriptRuntime` holds
the GIL for the thread that called `initialize()`, so every script call happens
on that thread.

## Consuming as a dependency

The library installs an exported CMake package, so another project can link it:

```cmake
find_package(mais CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE mais::mais)
```

The exported target requires pybind11, because maïs embeds Python through it;
the generated package config re-discovers it with `find_dependency`. A host that
defines its own bindings also links `pybind11::embed` directly, as the tests and
the standalone example do. Engine-specific bindings stay in the application or
in an optional adapter target and are never pulled in by the generic package.

pybind11 is not ABI-compatible across major versions, so the installed package
records the major it was built against and refuses to configure against a
different one. Pin the same major as the maïs package you consume; this
repository currently builds with pybind11 2.13.6, which `pip install
pybind11==2.13.6` also provides. Note that `pip install pybind11` now installs
3.x, which the package config rejects with an explanatory message.

## Integration with evan and guillaume

- `evan`: retains native ownership of engine configuration, windowing, rendering, and frame updates; it does not depend on Python.
- Optional adapter: binds a small, host-owned facade for configuration and runtime actions. It depends on maïs and `evan`, never the reverse.
- `guillaume`: owns its Python scripts, engine instance, UI, adapter selection, and callback scheduling.

UI components such as a script console and engine settings panel belong to the application UI. maïs only supplies generic scripting diagnostics and invocation facilities.

## Documentation and contributions

See [AGENTS.md](AGENTS.md) for repository development guidance and [TECHNICAL_CHOICES.md](docs/TECHNICAL_CHOICES.md) for recorded architectural decisions. Additional reading:

- [docs/GETTING_STARTED.md](docs/GETTING_STARTED.md) — build a host step by step.
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — ownership and dependency rules.
- [docs/ERROR_HANDLING.md](docs/ERROR_HANDLING.md) — error taxonomy and tracebacks.
- [examples/standalone_host](examples/standalone_host) — a complete host.
- [CONTRIBUTING.md](CONTRIBUTING.md) — how to take part, and [LICENSE](LICENSE) for terms.
