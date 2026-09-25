# maïs examples

Runnable samples that show how a host embeds Python through maïs. None of them
depend on `guillaume` or `evan`.

## standalone_host

The reference host. It owns two native services (`HostSettings` and
`HostMetrics`), exposes them to Python as the importable `host` module, and
drives the script hooks from its own loop. It doubles as an integration test,
so CTest runs it whenever tests are enabled.

It walks through the host's responsibilities in order:

1. Queue the `host` binding, then start the interpreter.
2. Add the script directory and import `game`.
3. Call the optional hooks: `configure`, `on_start`, `on_update`, `on_shutdown`.
4. Validate the settings the script proposed before using them.
5. Stop the runtime before the native services are destroyed.

### Building

```sh
cmake -S . -B build -DBUILD_EXAMPLES=ON
cmake --build build --target standalone_host
```

### Running

```sh
./build/examples/standalone_host/standalone_host examples/standalone_host/scripts
```

Scripts reach host objects with `import host` instead of receiving them as
call arguments; see the lifetime and argument notes in
[GETTING_STARTED.md](../docs/GETTING_STARTED.md).

