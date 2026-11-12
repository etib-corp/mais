# maïs examples

Runnable samples that show how a host embeds Python through maïs. None of them
depend on `guillaume` or `evan`.

## standalone_host

The reference host. It owns two native services (`HostSettings` and
`HostMetrics`), registers their classes in the importable `host` module, and
passes the host-owned objects to the hooks as call arguments. It drives the
script hooks from its own loop and doubles as an integration test, so CTest
runs it whenever tests are enabled.

It walks through the host's responsibilities in order:

1. Queue the `host` binding, then start the interpreter.
2. Add the script directory and import `game`.
3. Call `configure` and validate the settings the script proposed before using
   them, then call `on_start`.
4. Call `on_update` every frame with a `ReportOnce` policy, and print the
   duration the runtime measured for it: a failing hook is reported once
   instead of every frame, and its cost is attributable to that hook.
5. Call `on_shutdown`, then stop the runtime before the native services are
   destroyed.

### Building

```sh
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --target standalone_host
```

### Running

```sh
./build/examples/standalone_host/standalone_host examples/standalone_host/scripts
```

Scripts receive host objects as call arguments; the script never imports the
`host` module. See the lifetime and argument notes in
[GETTING_STARTED.md](../docs/GETTING_STARTED.md).

## frame_loop_host

The frame-loop host models the shape a real XR/UI consumer uses, with a local
engine stub instead of a real engine and no renderer, window, or UI toolkit. It
is the worked example for the M2 integration pattern:

1. Queue the `host` binding, start the interpreter, and import the module.
2. Call `configure` with a **settings facade** the script writes to. The host
   validates the proposal before starting its engine stub, so an invalid
   proposal is reported and the previous settings are kept instead. It then
   calls `on_start`.
3. Run a bounded fixed-timestep loop. The host advances its engine stub and
   then calls `on_update` with a native `FrameContext` — frame index, fixed
   delta, elapsed time, and a script-written step count — not a scalar.
4. Give each hook the policy that fits it: `Stop` for `configure` and
   `on_start`, `ReportOnce` for the per-frame `on_update`, and `Continue` for
   `on_shutdown`.
5. Call `on_shutdown` with the end-of-session context, stop the engine and the
   runtime, then print a shutdown summary: frames run, simulated time, script
   steps, reported and suppressed `on_update` failures, mean call time, and the
   settings the engine was started with.

The example script deliberately raises in `on_update` on two consecutive
frames. The first failure is reported with its traceback, the second is
suppressed by `ReportOnce`, and the loop still runs every planned frame and
exits cleanly. That is the per-hook failure behaviour a long-running consumer
needs, and it is why `ReportOnce` is used rather than an `if` around every
call.

### Building

```sh
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --target frame_loop_host
```

If CMake selects an interpreter without development files, add
`-DPython_EXECUTABLE=<path>` as described in the top-level README.

### Running

```sh
# The normal scenario: valid settings, one reported update failure.
./build/examples/frame_loop_host/frame_loop_host \
  examples/frame_loop_host/scripts game

# The rejection scenario: width = 0 is refused and never applied.
./build/examples/frame_loop_host/frame_loop_host \
  examples/frame_loop_host/scripts invalid_settings_game
```

The first run ends with a summary like:

```text
host: summary frames=120/120 engine_frames=120 simulated=1.333s script_steps=118 update_reports=1 suppressed=1 ... settings=1600x900
```

The second run rejects the proposal, starts with the host defaults
(`1280x720`), and exits cleanly:

```text
host: rejected settings proposal: width must be between 64 and 7680 pixels (got 0)
host: starting with the previous settings: 1280x720 "mais frame loop host"
```

CTest runs both scenarios when tests are enabled:

```sh
ctest --test-dir build --output-on-failure -R frame_loop_host
```

Neither example depends on `guillaume` or `evan`. The engine stub only records
the validated settings; a real consumer replaces it with its own engine and
keeps the same host-owned loop, context, and policy decisions.
