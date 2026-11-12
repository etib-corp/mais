# Script packaging and resolution order

maïs owns no script location of its own. It resolves the directories a host
hands it through `ScriptRuntime::addSearchPath()` and nothing else: there is no
implicit working-directory entry, no environment-variable search, and no
"scripts next to the executable" convention baked into the library. This
document defines what the runtime does with the paths it is given, and
recommends a layout for the host that gives them.

The order below is a contract, not an implementation note: the shadowing tests
in `tests/sources/test_script_runtime.cpp` fail if the implementation and this
document disagree.

## Resolution order

Paths are searched in the order they were added. When two search paths contain a
module with the same name, **the first path added wins**.

```cpp
runtime.addSearchPath("/opt/my_app/scripts");       // searched first
runtime.addSearchPath("/opt/my_app/scripts/extra"); // searched second
runtime.loadModule("game");  // /opt/my_app/scripts/game.py wins
```

Reversing the two `addSearchPath()` calls reverses the winner. The directory
names themselves mean nothing.

### How the paths reach `sys.path`

`addSearchPath()` does not append to the end of `sys.path`. The configured
directories form one contiguous **leading** block, in insertion order:

```text
sys.path[0..n-1]   the paths this runtime configured, in the order added
sys.path[n..]      the interpreter's own entries: PYTHONPATH, the standard
                   library, lib-dynload, site-packages
```

Two consequences follow:

- A directory added first is the very first place Python looks for a
  file-backed module, ahead of the standard library and of site-packages.
- A path added *after* `initialize()` is inserted immediately after the paths
  already configured, not in front of them. A host can therefore add a path to a
  running runtime without reordering the ones it already had.

Because the block sits in front of the interpreter's entries, a script
directory can shadow a standard-library module with the same name (a
`json.py` next to the host's scripts wins over `json`). That is ordinary
Python behaviour for a script directory, and it is the host's responsibility:
keep the script root free of names the library or the application relies on.

A name that is not backed by a file can never be shadowed. `import` consults
`sys.modules` first, then the built-in and frozen importers, and only then the
path finders:

1. a module already imported in this interpreter (`sys.modules`), which
   includes every module registered by `registerModule()`;
2. a built-in or frozen module (`sys`, `_io`, `importlib._bootstrap`, …);
3. the configured search paths, in insertion order;
4. the remaining `sys.path` entries.

### Duplicates and path spelling

`addSearchPath()` compares the string it was given with the strings already
recorded:

- the same string twice is a no-op that returns `Ok`; the directory is recorded
  once and reaches `sys.path` once;
- an empty string is refused with `InvalidArgument`;
- a path that is not an existing directory is refused with `ScriptNotFound` and
  is not recorded;
- a call from a thread that does not own the interpreter, once the runtime is
  running, is refused with `WrongThread` and is not recorded.

The comparison is literal. `scripts`, `scripts/`, `./scripts`, and
`/opt/my_app/scripts` are four different strings, so passing two spellings of
one directory records it twice and puts it in `sys.path` twice. The spelling
does not change the winner — both entries point at the same directory — but
`searchPaths()` no longer matches the host's intent. Pass absolute, canonical
paths (for example `std::filesystem::weakly_canonical()`), and the recorded
list stays one entry per directory.

Relative paths are passed to Python unchanged, so they resolve against the
process's working directory *at import time*. The host does not control that
directory — a Finder launch on macOS starts at `/`, and a service manager may
pick its own — which is why the layouts below resolve an absolute path first.

### One import per run

`import` caches the module it found in `sys.modules`. A module keeps the origin
it had when it was first imported: adding a search path later, or calling
`loadModule()` again, does not re-resolve the name to another directory. A new
`initialize()` starts a fresh interpreter, so a restart re-resolves every name
against the paths configured for that run.

## Failure modes

What a host sees, by cause. Only `InterpreterFailure` means scripting cannot
continue; every code below leaves a usable interpreter behind
(see [ERROR_HANDLING.md](ERROR_HANDLING.md)).

| Cause                                                  | Call                             | `ErrorCode`                                                  | Runtime afterwards                         |
| ------------------------------------------------------ | -------------------------------- | ------------------------------------------------------------ | ------------------------------------------ |
| Empty search path                                      | `addSearchPath("")`              | `InvalidArgument`                                            | Running; nothing recorded                  |
| Search path is not an existing directory               | `addSearchPath("scripts/games")` | `ScriptNotFound`                                             | Running; nothing recorded                  |
| Same search path added twice                           | `addSearchPath(same)`            | `Ok`                                                         | Running; one entry                         |
| `addSearchPath()` from a foreign thread                | `addSearchPath(dir)`             | `WrongThread`                                                | Running; nothing recorded                  |
| No configured path contains the module                 | `loadModule("game")`             | `ModuleNotFound`                                             | Running; `isModuleLoaded("game")` is false |
| Module found, and its import raised                    | `loadModule("game")`             | `InvocationFailed`, with `traceback()` and `frames()`        | Running; the module is not marked loaded   |
| Module found, and it imports a module no path provides | `loadModule("game")`             | `ModuleNotFound`, with the traceback of the failing `import` | Running; the module is not marked loaded   |
| Module name is empty                                   | `loadModule("")`                 | `InvalidArgument`                                            | Running                                    |
| Runtime not started or already stopped                 | `loadModule("game")`             | `NotInitialized`                                             | Unchanged                                  |
| Hook called on a module that was never loaded          | `call("game", "on_update")`      | `ModuleNotFound`                                             | Running                                    |

A missing directory is therefore reported where the host made the mistake — at
`addSearchPath()` — rather than as a puzzling `ModuleNotFound` at load time. It
also means the runtime never silently ignores a path that the host misspelled.

`ScriptNotFound` and `ModuleNotFound` are different codes on purpose: the first
is a configuration error the host can fix before starting anything, the second
can also be caused by a genuinely absent optional script.

## Recommended layout

### Development

Keep the scripts in the host's source tree, reached through the build:

```text
my_app/
├── CMakeLists.txt
├── sources/
│   └── main.cpp
├── scripts/                 # the script root the host registers
│   ├── game.py
│   └── settings.py
└── third_party/
    └── mais/
```

Point the host target at that directory at configure time. The definition
belongs to the host target, so nothing about script layout leaks into maïs:

```cmake
target_compile_definitions(my_app PRIVATE
    MYAPP_SOURCE_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts")
```

Nothing stops a developer from editing `game.py` in the source tree while the
host runs from the build tree; the path is absolute and does not depend on the
working directory. Two cautions:

- the compiler embeds the path of the machine that configured the build, so use
  the definition only when the host is meant to run there (a `Debug` build, or a
  dedicated option);
- prefer an environment-variable override, shown below, for the case where a
  developer wants to run the shipped binary against a working copy.

### Packaged

Ship the scripts as application resources: read-only, installed next to or
beside the executable by whichever mechanism the project already uses (CPack,
an installer, a `.tar.gz`, a platform store). The layouts below are conventions,
not requirements — the requirement is that the host knows one absolute
directory and passes it to `addSearchPath()`.

| Platform              | Executable                          | Scripts                                 |
| --------------------- | ----------------------------------- | --------------------------------------- |
| macOS                 | `MyApp.app/Contents/MacOS/my_app`   | `MyApp.app/Contents/Resources/scripts/` |
| Linux, prefix install | `/usr/local/bin/my_app`             | `/usr/local/share/my_app/scripts/`      |
| Linux, self-contained | `my_app/bin/my_app`                 | `my_app/scripts/`                       |
| Windows               | `C:\Program Files\MyApp\my_app.exe` | `C:\Program Files\MyApp\scripts\`       |

A read-only script directory is fine: CPython skips writing `__pycache__` when
it cannot, without reporting an error. A writable one gains bytecode caches that
an installer should probably exclude rather than ship.

### Locating the directory at runtime

Resolve the directory from the executable, never from the working directory, and
let a candidate list absorb the differences between the layouts above. First
candidate that exists wins, so one binary handles a prefix install and a
self-contained tarball.

```cpp
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace {

/// Directory holding the running executable; empty when it cannot be read.
std::filesystem::path executableDirectory()
{
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    // A too-small buffer is the documented way to query the length.
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    // The path the OS reports may run through a symlink; prefer the real one.
    const std::filesystem::path self(buffer.data());
    std::error_code ignored;
    const std::filesystem::path resolved =
        std::filesystem::canonical(self, ignored);
    return (ignored ? self : resolved).parent_path();
#else
    std::error_code ignored;
    const std::filesystem::path self =
        std::filesystem::read_symlink("/proc/self/exe", ignored);
    return ignored ? std::filesystem::path {} : self.parent_path();
#endif
}

}  // namespace

/// Absolute script root for this run; empty when nothing was found.
std::filesystem::path findScriptDirectory()
{
    // 1. An explicit override, for development or a relocated install.
    if (const char *override = std::getenv("MYAPP_SCRIPT_DIR");
        override != nullptr && *override != '\0') {
        return override;
    }

    const std::filesystem::path executable = executableDirectory();
    if (executable.empty()) {
        return {};
    }

    const std::vector<std::filesystem::path> candidates = {
#if defined(__APPLE__)
        // MyApp.app/Contents/MacOS -> MyApp.app/Contents/Resources
        executable / ".." / "Resources" / "scripts",
#endif
        executable / "scripts",                              // portable, Windows
        executable / ".." / "share" / "my_app" / "scripts",  // Linux prefix
    };

    for (const std::filesystem::path &candidate: candidates) {
        std::error_code ignored;
        if (std::filesystem::is_directory(candidate, ignored)) {
            return std::filesystem::weakly_canonical(candidate);
        }
    }
    return {};
}
```

Then register it, and treat "not found" as the configuration error it is
instead of letting it surface later as a `ModuleNotFound`:

```cpp
const std::filesystem::path scripts = findScriptDirectory();
if (scripts.empty()) {
    std::cerr << "no script directory found; set MYAPP_SCRIPT_DIR\n";
    return 1;
}

// pybind11 hands the string to Python as UTF-8, so take the path's UTF-8 form:
// path::string() would narrow a Windows path through the ANSI code page, and
// in C++20 u8string() is a std::u8string rather than a std::string.
const std::u8string utf8 = scripts.u8string();
if (mais::Error error =
        runtime.addSearchPath(std::string(utf8.begin(), utf8.end()));
    error) {
    std::cerr << error.toString() << '\n';  // ScriptNotFound if it vanished
    return 1;
}
```

Two habits keep this predictable:

- **One root, one call.** Give the host a single script root and organise it
  with packages (`scripts/game/__init__.py`, `scripts/game/hooks.py`). One
  `addSearchPath()` call means no ordering question at all; if two roots are
  genuinely needed, the order of the calls is what decides a collision.
- **Add paths before `initialize()` when the layout allows it.**
  `initialize()` is atomic, so a path that fails there leaves Python stopped
  instead of a half-configured runtime. Adding a path later is supported and
  keeps its place after the paths already configured.

## What is asserted

`tests/sources/test_script_runtime.cpp` holds the contract down:

- `SearchPathOrderTest.ResolvesAShadowedModuleToTheFirstAddedPath` — two
  directories each provide a module named `shadowed`; the first one added
  provides the imported module, in both orders.
- `SearchPathOrderTest.PutsTheSearchPathsAheadOfTheInterpreterEntries` — the
  configured paths are the leading `sys.path` entries, in insertion order.
- `SearchPathOrderTest.IgnoresADuplicateSearchPath` — a repeated path is
  recorded once, reaches `sys.path` once, and never overtakes a path added
  before it.
- `SearchPathOrderTest.TreatsTwoSpellingsOfOneDirectoryAsTwoPaths` — the
  duplicate check is literal, so a trailing slash is a different path.
- `ScriptLoadingTest.ValidatesSearchPaths` — `InvalidArgument` for an empty
  path and `ScriptNotFound` for a directory that does not exist.
- `ScriptLoadingTest.ReportsMissingModules` — `ModuleNotFound` for a module no
  path provides.
