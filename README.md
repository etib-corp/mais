# maïs

maïs is a reusable C++ scripting runtime for applications that embed Python through pybind11. It manages the generic scripting lifecycle while leaving engine integration, application scripts, and UI to the host. `guillaume` is the first intended consumer; `evan` is an engine it may connect to through a separate adapter.

> This README describes the target design, not a verified inventory of the current repository. Check the implementation and CMake targets before treating the examples below as working commands.

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

## Intended usage

A host typically:

1. Creates its native services and configuration.
2. Registers bindings for the services it chooses to expose **before** initializing Python when using embedded pybind11 modules.
3. Starts maïs, adds the application's script directory, and invokes an optional configuration hook.
4. Initializes its engine, then invokes script hooks at documented points in its loop.
5. Releases script references and stops Python before destroying referenced host services.

For example, a consumer-owned Python script could define:

```python
def configure(config):
    config.window_title = "Guillaume"
    config.width = 1280
    config.height = 720


def on_update(context, dt):
    pass
```

These hook names and object fields illustrate a proposed integration contract; they are not claims about an existing maïs API.

## Building

The intended build dependencies are a compatible C++ compiler, CMake, Python development files, and pybind11. Exact minimum versions and dependency acquisition should be documented after they have been verified in the repository. No Vulkan SDK, OpenXR, GLFW, or engine-specific platform selection should be required for maïs itself.

For a conventional CMake layout, the expected workflow is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The `BUILD_TESTING` option and commands above are targets for the implementation; inspect the checked-in CMake files for the actual supported options. Use a Python interpreter and development library from the same compatible installation.

## Consuming as a dependency

The intended deliverable is an exported CMake target that can be installed and linked from another project. If the package name and target are implemented as `mais` and `mais::mais`, usage would look like:

```cmake
find_package(mais CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE mais::mais)
```

Do not assume `find_package(mais)` works until installation/export rules exist and have been tested with a separate consumer. Engine-specific bindings should be linked by the application or an optional adapter target, not pulled in automatically by the generic package.

## Integration with evan and guillaume

- `evan`: retains native ownership of engine configuration, windowing, rendering, and frame updates; it does not depend on Python.
- Optional adapter: binds a small, host-owned facade for configuration and runtime actions. It depends on maïs and `evan`, never the reverse.
- `guillaume`: owns its Python scripts, engine instance, UI, adapter selection, and callback scheduling.

UI components such as a script console and engine settings panel belong to the application UI. maïs only supplies generic scripting diagnostics and invocation facilities.

## Documentation and contributions

See [AGENTS.md](AGENTS.md) for repository development guidance and [TECHNICAL_CHOICES.md](TECHNICAL_CHOICES.md) for architectural decisions. Add links to build guides, contribution rules, and license files when those files are present; this README does not assume the old renderer project's documentation or license applies.
