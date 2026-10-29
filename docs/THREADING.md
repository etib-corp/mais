# Threading and the GIL

**Status:** accepted (M1, MAIS-1.6), before 1.0.0. **Applies to:** `mais::ScriptRuntime`.

This is the decision record behind the threading contract in
[`ScriptRuntime.hpp`](../headers/mais/ScriptRuntime.hpp). It exists because
`TECHNICAL_CHOICES.md` requires interpreter ownership to be an explicit choice,
and because `evan` may eventually want scripts callable from a render or worker
thread (`ROADMAP.md`, Q5).

## The question

Which thread may invoke scripts, who holds the GIL, and what happens when a host
gets it wrong?

## The decision

**One thread owns the interpreter: the thread that calls `initialize()`.** maïs
does not release the GIL for that thread while the runtime is running, does not
provide cross-thread entry, and does not attach to an interpreter it did not
start.

Concretely:

- `initialize()` starts Python on the calling thread. pybind11's
  `scoped_interpreter` leaves the GIL with that thread and maïs never calls
  `PyEval_SaveThread()`, so the interpreter stays bound to it from
  `initialize()` to `shutdown()`.
- Every Python-touching operation must be made from that thread:
  `initialize()`, `shutdown()`, `loadModule()`, `addSearchPath()` while the
  runtime is running, `call()`, and `callOptional()`.
- Host bindings are written for that thread, and a hook runs there. A script
  that hands a bound function to its own `threading.Thread` moves that call off
  the owning thread; a binding that then tries to re-enter `ScriptRuntime` is
  refused with `WrongThread`.
- A violation is refused with `ErrorCode::WrongThread` before Python is
  touched. That check is not a synchronization mechanism; it turns a silent
  hang into a diagnosable error.
- The runtime is not internally synchronized. The accessors that only read C++
  state (`isRunning()`, `searchPaths()`, `bindings()`, `isModuleLoaded()`) are
  safe only when the host itself guarantees that no state-changing call runs
  concurrently.

## What the host must guarantee

1. **One thread does all scripting.** Typically the application's main thread.
   If `guillaume` wants scripting on a dedicated thread, that thread calls
   `initialize()`, `loadModule()`, every hook, and `shutdown()` — nothing else
   calls into maïs.
2. **That thread also destroys the runtime.** `~ScriptRuntime()` stops the
   interpreter, so it must run where `shutdown()` would have been allowed.
3. **Native objects exposed to Python are only mutated from that thread** while
   Python can reach them. maïs cannot enforce this; it follows from the host
   owning its objects (`ARCHITECTURE.md`, "Ownership boundaries").
4. **No background Python threads.** A `threading.Thread` started by a script
   can only make progress while the owning thread is executing Python bytecode,
   because that is the only time the GIL is handed over. Scripts must not rely
   on it. maïs does not pump Python between hooks.

## What happens on violation

| Operation | Owning thread | Any other thread |
| --- | --- | --- |
| `initialize()` | starts Python | `AlreadyInitialized`; it never touches the running interpreter, so it is safe |
| `call()` / `callOptional()` | runs the hook | `WrongThread`, the hook does not run |
| `loadModule()` | imports the module | `WrongThread`, nothing is imported |
| `addSearchPath()` while running | prepends to `sys.path` | `WrongThread`, the path is not recorded |
| `shutdown()` | finalizes Python | `WrongThread`, the runtime keeps running |
| `registerModule()` | fails with `AlreadyInitialized` | same |
| read-only accessors | reads C++ state | unsynchronized; the host must not race them |
| `~ScriptRuntime()` while running | finalizes Python | **not detectable** — no return path to report through, and the interpreter cannot be finalized from a thread that does not own it |

The last row is the one gap. There is no error to return from a destructor, so
the contract is documentation only: a runtime whose owner thread exits without
calling `shutdown()` cannot be stopped by anyone else, and destroying it
elsewhere is undefined behaviour. Hosts must keep the runtime's lifetime inside
the owning thread's scope.

## Why this model

- **It is what embedding Python already gives you.** Declaring it is cheaper
  than fighting it, and it keeps the invariant obvious: every `py::object` in
  the runtime is created, used, and destroyed on one thread.
- **It preserves the lifetime story that bindings depend on.** Native facades are
  borrowed for a call but the Python wrapper may outlive it, so the object must
  outlive the runtime (`ScriptRuntime.hpp`, "Lifetime contract"). Serializing
  every access to one thread is what makes that rule sufficient.
- **It costs nothing today.** The GIL serializes Python anyway, so a second
  thread calling `call()` would not run any faster; it would only add ways to
  get the ordering and shutdown logic wrong.
- **Nobody has asked for it yet.** Q5 ("must scripts ever run off the main
  thread?") is unanswered by `evan` and `guillaume`. Guessing now would bake a
  queue and an ownership model into the public API before there is a caller to
  validate them.

## Rejected alternative: release the GIL between calls

The proposal: leave the GIL released between calls so any thread can enter
Python and `gil_scoped_acquire` succeeds.

Rejected for three reasons:

- **It buys no parallelism.** CPython still runs one thread at a time. It buys
  cross-thread entry, which is what a dispatch queue gives with explicit
  ordering; here the ordering would be whatever the OS scheduler picks.
- **It moves the hard problem rather than solving it.** Finalization must still
  happen on the initializing thread, so the runtime would have to track how many
  threads are inside Python, block `shutdown()` until that count drains, and
  decide what happens to a call that arrives while shutting down. That is the
  same bookkeeping a queue needs, minus the ordering guarantees.
- **It pushes data races onto every binding author.** A hook could run on a
  worker thread while the host mutates the native object the hook is reading.
  Today that is impossible by construction.

If it is ever revisited, the minimum it requires is: `PyEval_SaveThread()` after
initialization, `PyEval_RestoreThread()` around finalization, a counter of
threads inside Python, a documented shutdown drain rule, and a statement of what
a hook may assume about host state.

## Deferred alternative: a dispatch queue (MAIS-4.1)

Marshal work onto the interpreter thread from any thread and let the owning
thread drain the queue at host-chosen points. Not rejected — deferred, because it
changes the host's contract (the host must pump) and should be designed with the
consumer that needs it in the room.

The design MAIS-4.1 starts from:

- A work item carries a callable, a completion signal, and an error slot, so the
  submitting thread can observe the `Error` (message and Python traceback).
- `call()` from the owning thread stays inline. Re-entrancy — a hook calling back
  into maïs on the same thread — must not enqueue, or it would wait on a queue
  only it can drain.
- Submission from another thread either blocks with a timeout or returns a
  future, and never holds a lock while Python runs.
- `shutdown()` stops accepting items, drains or fails the pending ones with a
  documented error, and then finalizes Python on the owning thread.
- A bounded queue with a stated frame budget, so a slow script cannot grow the
  queue without limit or stall the frame indefinitely.
- The GIL is still only held while a work item executes, on the owning thread.

No part of that is implemented. Nothing in the current API promises it.

## If the answer changes

1. Write the change as its own decision record, update this file's status, and
   update `MILESTONES.md` first.
2. Keep the synchronous `call()` semantics for the owning thread, so existing
   hosts keep working unchanged.
3. Add the new entry point (`post()`/`submit()` plus a pump point) rather than
   redefining `call()`, so the contract change is opt-in.
4. Relax the `WrongThread` refusal only for the operations the new model
   genuinely supports; the rest stays refused.
5. Update `ScriptRuntime.hpp`, `ARCHITECTURE.md`, `TECHNICAL_CHOICES.md`,
   `GETTING_STARTED.md`, and `CHANGELOG.md` in the same change, and add tests
   that a cross-thread call is delivered on the owning thread in order.
6. Revisit Q5 in `ROADMAP.md`.

## Related decision: one owner per process

Interpreter *ownership* is separate from thread affinity. `initialize()` fails
with `InterpreterFailure` when any interpreter is already running, so maïs never
shares or attaches to one. Attaching to a host-owned interpreter remains
undecided and unimplemented; if it is ever added it needs its own record.

## What is tested, and what is not

Tested in [`tests/sources/test_thread_affinity.cpp`](../tests/sources/test_thread_affinity.cpp):

- `call()`, `loadModule()`, `addSearchPath()`, and `shutdown()` from a second
  thread all return `WrongThread`, and none of them changes the runtime: no hook
  ran, the path list is unchanged, the interpreter is still running.
- The owning thread keeps working after those refusals — a real call succeeds and
  the counter increments.
- The owner is the thread that called `initialize()`, not the thread that
  constructed the runtime, so a dedicated script thread works.
- Ownership follows each `initialize()`, including after a restart on a different
  thread.

Not tested, deliberately:

- **An actual deadlock.** With the refusal in place there is no reachable
  deadlock to assert, and a test for the old behaviour would hang the suite
  instead of failing it.
- **The destructor from the wrong thread.** The violation cannot be reported and
  would have to be observed as undefined behaviour, so it is documented rather
  than tested.
- **The absence of background Python threads.** A test could start one and
  observe that it does not progress, but that asserts a scheduling accident
  rather than a contract.
