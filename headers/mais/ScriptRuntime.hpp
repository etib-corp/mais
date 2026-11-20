/*
 Copyright (c) 2026 ETIB Corporation

 Permission is hereby granted, free of charge, to any person obtaining a copy of
 this software and associated documentation files (the "Software"), to deal in
 the Software without restriction, including without limitation the rights to
 use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
 of the Software, and to permit persons to whom the Software is furnished to do
 so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in all
 copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 SOFTWARE.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "mais/BindingRegistry.hpp"
#include "mais/Error.hpp"
#include "mais/Result.hpp"

namespace mais
{
	/// \brief A single argument passed to a script function.
	///
	/// Scalars are built through the named factories so the Python type is
	/// explicit at the call site instead of being guessed by overload
	/// resolution:
	///
	/// ```cpp
	/// runtime.call("game", "on_update",
	/// {mais::ScriptArgument::number(delta)});
	/// ```
	///
	/// A host-owned native object is passed with native(), which pairs the
	/// object's address with a converter the host writes in a pybind11
	/// translation unit. maïs never dereferences the address and never takes
	/// ownership of the object.
	class ScriptArgument
	{
		public:
		/// \brief Produces the Python object representing a native argument.
		///
		/// `value` is the address handed to ScriptArgument::native(). The
		/// converter must return a *new reference* to a Python object that
		/// wraps that address without taking ownership of the C++ object, or
		/// `nullptr` when the value cannot be represented. maïs takes
		/// ownership of a non-null result.
		///
		/// The converter runs on the thread that called
		/// ScriptRuntime::initialize(), with the GIL held, so it may use
		/// pybind11 freely:
		///
		/// ```cpp
		/// mais::ScriptArgument::NativeConverter converter =
		///     +[](const void *value) -> void * {
		///         return pybind11::cast(
		///                    static_cast<const HostSettings *>(value),
		///                    pybind11::return_value_policy::reference)
		///             .release()
		///             .ptr();
		///     };
		/// ```
		using NativeConverter = std::function<void *(const void *)>;

		/// \brief A host-owned object lent to one call.
		struct NativeObject {
			/// Address of the host object; maïs never dereferences it.
			const void *value = nullptr;

			/// Produces the Python object for `value`; see NativeConverter.
			NativeConverter converter;
		};

		/// A Python `int`.
		[[nodiscard]] static ScriptArgument integer(std::int64_t value);

		/// A Python `float`.
		[[nodiscard]] static ScriptArgument number(double value);

		/// A Python `bool`.
		[[nodiscard]] static ScriptArgument boolean(bool value);

		/// A Python `str`.
		[[nodiscard]] static ScriptArgument string(std::string value);

		/// \brief A host-owned object, converted by `converter` at call time.
		///
		/// Lifetime: maïs never takes ownership. The converter runs only for
		/// the duration of the call, but the Python wrapper it returns may
		/// outlive the call, so `value` must outlive the runtime — the object
		/// must be destroyed after ScriptRuntime::shutdown(). Its class must
		/// already be registered by a binding callback, otherwise the call
		/// fails with ErrorCode::TypeMismatch.
		[[nodiscard]] static ScriptArgument native(const void *value,
												   NativeConverter converter);

		/// Payload type, one of `std::int64_t`, `double`, `bool`,
		/// `std::string`, or NativeObject.
		using Value =
			std::variant<std::int64_t, double, bool, std::string, NativeObject>;

		/// The stored payload.
		[[nodiscard]] const Value &value() const noexcept;

		/// Python type name of the payload, for diagnostics. A native argument
		/// reports "native".
		[[nodiscard]] const char *typeName() const noexcept;

		/// The native object when this argument is native, else nullptr.
		[[nodiscard]] const NativeObject *nativeObject() const noexcept;

		private:
		explicit ScriptArgument(Value value);

		Value _value;
	};

	/// \brief What a policy-bearing call does with a failure.
	///
	/// A policy never stops the interpreter and never throws. The runtime
	/// reports and the host, which owns its loop, decides when to leave it.
	///
	/// A host applies a policy per call, which also makes it per hook: pair
	/// the policy it wants with the name it is about to invoke. listFunctions()
	/// supplies the names when the schedule is built dynamically.
	enum class FailurePolicy {
		/// Report every failure and leave the decision to the host. This is the
		/// behaviour of the Error-returning call() overloads.
		Continue,

		/// Report the failure and mark the report with stopRequested, so a host
		/// can leave its loop without inspecting the error code. The runtime
		/// itself keeps running.
		Stop,

		/// Report the first failure of a hook once per run, then suppress the
		/// identical repeats that follow it. A successful call ends the streak,
		/// so a hook that recovers and breaks again is reported again.
		ReportOnce,
	};

	/// \brief The outcome of one call: its error, how long it took, and the
	/// failure accounting the policy used.
	///
	/// A suppressed repeat of an identical failure has an Ok `error` and
	/// `suppressed` set, so the common loop needs no per-call branching beyond
	/// the one it already writes:
	///
	/// ```cpp
	/// mais::CallReport report = runtime.callOptional(
	///     mais::FailurePolicy::ReportOnce, "game", "on_update",
	///     {mais::ScriptArgument::number(deltaSeconds)});
	/// if (report.stopRequested) {
	///     break;                      // only under FailurePolicy::Stop
	/// }
	/// if (report) {
	///     console.log(report.error);  // once per failure streak
	/// }
	/// console.timing("on_update", report.duration);
	/// ```
	struct CallReport {
		/// The failure, or an Ok error when the call succeeded or the failure
		/// was suppressed as an identical repeat.
		Error error;

		/// \brief How long the call took, measured by the runtime.
		///
		/// The clock spans validation, argument conversion, the Python call,
		/// and the capture of its failure, so the number is the cost the host
		/// pays for that hook rather than a benchmark estimate. It is only
		/// measured by the policy-bearing overloads.
		std::chrono::nanoseconds duration {};

		/// \brief Consecutive failures of this hook in the current run.
		///
		/// Zero after a success, and zero for a call rejected before the hook
		/// ran (an empty name, a runtime that is not initialized).
		std::uint64_t consecutiveFailures = 0;

		/// True when this failure repeats the previous one and the policy
		/// suppressed it; `error` is then Ok.
		bool suppressed = false;

		/// True when the policy is Stop and the call failed.
		bool stopRequested = false;

		/// True when the report carries no failure.
		[[nodiscard]] bool isOk() const noexcept
		{
			return error.isOk();
		}

		/// True when an error occurred; mirrors Error's operator bool.
		explicit operator bool() const noexcept
		{
			return !error.isOk();
		}
	};

	/// \brief Where one reload attempt stopped.
	///
	/// The phase, not the error code, distinguishes a compile failure from an
	/// execution failure: a syntax error and an exception in the module body
	/// can both report InvocationFailed.
	enum class ReloadPhase {
		/// Runtime state, module name, module kind and options.
		Validate,

		/// State export hook on the previous generation.
		Export,

		/// Reading and compiling the captured source.
		Compile,

		/// Creating and executing the new module body.
		Execute,

		/// State import hook on the new generation.
		Restore,

		/// The new generation is current.
		Commit,
	};

	/// \brief Opt-in state migration for one reload.
	///
	/// Both fields are either set or empty. The export function runs on the
	/// previous generation and returns the state as a `str`; the import
	/// function runs on the new generation and accepts that `str`. The payload
	/// is opaque text: the script validates its own schema and raises to
	/// refuse. See docs/HOT_RELOAD.md.
	struct ReloadOptions {
		/// Function on the previous generation returning the state as `str`.
		std::string exportFunction;

		/// Function on the new generation accepting that `str`.
		std::string importFunction;
	};

	/// \brief The outcome of one reload attempt.
	///
	/// The previous generation stays current after every failure, so a host
	/// can keep calling its hooks; `error` describes what went wrong and
	/// `phase` says where. A successful attempt reports Ok and ReloadPhase::
	/// Commit.
	struct ReloadReport {
		/// `Ok` when the new generation is current; the failure otherwise.
		Error error;

		/// Where the attempt stopped.
		ReloadPhase phase = ReloadPhase::Commit;

		/// Total wall time of the attempt.
		std::chrono::nanoseconds duration {};

		/// Reading and compiling the captured source.
		std::chrono::nanoseconds compileDuration {};

		/// Executing the new module body.
		std::chrono::nanoseconds executeDuration {};

		/// Export plus restore, or zero when migration was not requested.
		std::chrono::nanoseconds stateDuration {};

		/// Generation that is current when the call returns; unchanged on
		/// failure, and 0 when no generation is current.
		std::uint64_t generation = 0;

		/// True when the export hook ran and returned text.
		bool stateExported = false;

		/// True when the import hook accepted that text.
		bool stateRestored = false;

		/// True when the new generation is current.
		[[nodiscard]] bool isOk() const noexcept
		{
			return error.isOk();
		}

		/// True when the attempt failed; mirrors Error's operator bool.
		explicit operator bool() const noexcept
		{
			return !error.isOk();
		}
	};

	/// \brief Owns an embedded Python interpreter and runs host-defined
	/// scripts.
	///
	/// Lifecycle
	/// ---------
	/// One ScriptRuntime owns exactly one interpreter between initialize() and
	/// shutdown(). maïs never attaches to an interpreter it did not start:
	/// initialize() fails with InterpreterFailure when Python is already
	/// running, so a process keeps a single owner.
	///
	/// Binding ordering
	/// ----------------
	/// registerModule() queues a module definition before the interpreter
	/// starts; initialize() creates each module and runs its callback, after
	/// which the module is importable by name. This is a deliberate choice
	/// over pybind11's statically registered `PYBIND11_EMBEDDED_MODULE`
	/// macro: callbacks run with a live interpreter, so they may freely create
	/// classes and attach host objects. Modules are built into the executable
	/// and need no import path or packaging.
	///
	/// Lifetime contract
	/// -----------------
	/// The interpreter outlives a script module only until shutdown(). Native
	/// objects attached to Python must therefore outlive the runtime, and
	/// shutdown() must run before those objects are destroyed. Expose a native
	/// object with `pybind11::return_value_policy::reference` (or
	/// `reference_internal`) so Python never takes ownership of a host object.
	/// The same rule covers objects passed as arguments with
	/// ScriptArgument::native(): the runtime borrows the address for one call,
	/// but the wrapper it hands Python may keep referring to that address, so
	/// the object must still outlive the runtime.
	///
	/// Threading
	/// ---------
	/// The runtime is thread-affine. The thread that calls initialize() owns
	/// the interpreter until shutdown() returns: maïs keeps the GIL on that
	/// thread and never releases it between calls, so host bindings are
	/// written for that thread and normally run while a hook is executing.
	///
	/// Every method that touches Python must therefore be called from that
	/// thread — initialize(), shutdown(), loadModule(), reloadModule(),
	/// unloadModule(), addSearchPath() while the runtime is running,
	/// hasFunction(), listFunctions(), and every call()/callOptional()
	/// overload, including the typed and the policy-bearing ones. A call from
	/// any other thread is refused with
	/// ErrorCode::WrongThread instead of blocking on the GIL. The runtime is
	/// not internally synchronized, so the accessors that only read C++ state
	/// (isRunning(), searchPaths(), bindings(), isModuleLoaded()) are safe only
	/// when the host itself guarantees that no state-changing call runs
	/// concurrently.
	///
	/// That guarantee covers the mutators that run without the interpreter:
	/// registerModule() and addSearchPath() before initialize() edit the same
	/// shared state, so a host that queues bindings or paths from a second
	/// thread must serialize those calls against the owner's setup itself.
	///
	/// The one violation maïs cannot report is destroying a running runtime on
	/// another thread: ~ScriptRuntime() stops the interpreter and has no way
	/// to fail, so it must run on the owning thread too. A runtime whose owner
	/// thread has exited without calling shutdown() cannot be stopped by
	/// anyone else either.
	///
	/// Script errors never propagate as C++ exceptions: they are reported
	/// through Error so a failed hook cannot silently unwind the host's loop.
	/// See docs/THREADING.md for the decision and the alternatives that were
	/// rejected.
	///
	/// Call policy and timing
	/// ----------------------
	/// The Error-returning call() and callOptional() overloads report every
	/// failure, as they always have. The overloads that take a FailurePolicy
	/// first report a CallReport instead: the same error, the duration the
	/// runtime measured, and the per-hook failure count the policy used. A
	/// repeatedly failing hook therefore cannot end the host loop unless the
	/// host asks for FailurePolicy::Stop and leaves the loop itself.
	///
	/// The failure counters belong to one run of one runtime. initialize() and
	/// shutdown() clear them, so a restart never inherits a suppression, and
	/// two runtimes never share state. Only the policy-bearing overloads decide
	/// what to report; every call updates the counters, so a success through
	/// any overload ends the hook's streak.
	class ScriptRuntime
	{
		public:
		/// Creates a runtime that has not started Python yet.
		ScriptRuntime();

		/// Stops the interpreter when it is still running.
		~ScriptRuntime();

		// A runtime owns a process-wide interpreter, so it is neither copyable
		// nor movable: one owner, created where it is used.
		ScriptRuntime(const ScriptRuntime &)			= delete;
		ScriptRuntime &operator=(const ScriptRuntime &) = delete;
		ScriptRuntime(ScriptRuntime &&)					= delete;
		ScriptRuntime &operator=(ScriptRuntime &&)		= delete;

		// -- Bindings --------------------------------------------------------

		/// Queues `callback` as a module importable under `name`.
		/// \returns AlreadyInitialized once the runtime is running;
		/// InvalidArgument when `name` or `callback` is empty.
		Error registerModule(std::string name, BindingCallback callback);

		/// Bindings queued for the next initialize(), then still registered.
		[[nodiscard]] const BindingRegistry &bindings() const noexcept;

		// -- Lifecycle -------------------------------------------------------

		/// Starts Python and applies every queued binding.
		/// \returns AlreadyInitialized when already running, InterpreterFailure
		/// when Python cannot start or an interpreter already exists.
		Error initialize();

		/// Releases script references and finalizes the interpreter.
		/// \returns NotInitialized when the runtime is not running;
		/// WrongThread when called from a thread that does not own the
		/// interpreter, in which case the runtime keeps running.
		Error shutdown();

		/// True between a successful initialize() and shutdown().
		[[nodiscard]] bool isRunning() const noexcept;

		// -- Script loading --------------------------------------------------

		/// \brief Adds a directory that loadModule() searches for imports.
		///
		/// Search order
		/// ------------
		/// Paths are searched in the order they were added: when two of them
		/// contain a module with the same name, the first path added wins.
		/// The configured paths form one leading block in `sys.path`, ahead of
		/// the interpreter's own entries, so a script directory can shadow a
		/// standard-library module. A path added while the runtime is running
		/// is inserted after the paths already configured, never in front of
		/// them. See docs/SCRIPT_PACKAGING.md for the full contract.
		///
		/// The string is compared literally against the paths already
		/// recorded, so repeating one is a no-op that returns Ok, while two
		/// spellings of the same directory are recorded twice. Pass an
		/// absolute path: a relative one resolves against the process's
		/// working directory at import time.
		///
		/// \returns ScriptNotFound when `path` is not an existing directory,
		/// in which case nothing is recorded; WrongThread when the path would
		/// touch Python from a thread that does not own the interpreter.
		Error addSearchPath(const std::string &path);

		/// \brief Search paths configured so far, in search order.
		///
		/// The first entry wins when two of them contain the same module name.
		[[nodiscard]] std::vector<std::string> searchPaths() const;

		/// \brief Imports `moduleName` using the configured search paths.
		///
		/// The first configured path that provides the name wins, but only
		/// once per run: an already imported module, including one registered
		/// by registerModule(), is returned from `sys.modules` whatever the
		/// search paths say.
		/// \returns NotInitialized, InvalidArgument, ModuleNotFound,
		/// WrongThread, or InvocationFailed when the module raises while
		/// importing.
		Error loadModule(const std::string &moduleName);

		/// True when `moduleName` was imported by loadModule() or registered as
		/// a host binding.
		[[nodiscard]] bool isModuleLoaded(const std::string &moduleName) const;

		/// \brief Re-executes a loaded, source-backed module as a new
		/// generation.
		///
		/// The new generation executes in a fresh module object while the
		/// previous one is retained privately. Nothing becomes visible to the
		/// runtime or to later `import` statements until the new body, and any
		/// requested state migration, have succeeded; every failure therefore
		/// leaves the previous generation current and callable.
		///
		/// Generation
		/// ----------
		/// A first load records generation 1, each successful reload increments
		/// it, and an unload followed by a load starts again at 1. The
		/// generation is also written to the module as `__mais_generation__`
		/// after the module body succeeds; module-level code must not read it
		/// during execution, but functions may read it at call time.
		///
		/// Refused modules
		/// ---------------
		/// Only modules loaded by loadModule() from a `.py` source are
		/// reloadable. Host bindings and modules without a source file are
		/// refused with InvalidArgument, and dependencies a script imported on
		/// its own are not reloadable until the host loads them explicitly.
		///
		/// \returns A report whose `phase` names the step that stopped. The
		/// call never throws and never stops the interpreter for an expected
		/// failure; `error` carries the Python traceback when Python raised.
		[[nodiscard]] ReloadReport
			reloadModule(const std::string &moduleName,
						 const ReloadOptions &options = {});

		/// \brief Releases a loaded script module.
		///
		/// `isModuleLoaded()` and later calls report ModuleNotFound, exactly as
		/// for a module that was never loaded. The runtime removes the
		/// `sys.modules` entry only while it still points at the runtime's own
		/// module object. A host binding is refused with InvalidArgument, and
		/// an unknown module with ModuleNotFound.
		///
		/// \returns NotInitialized, InvalidArgument, ModuleNotFound, or
		/// WrongThread when called from a thread that does not own the
		/// interpreter. A refusal changes nothing.
		Error unloadModule(const std::string &moduleName);

		/// \brief The generation of a loaded script module.
		///
		/// This is a read-only C++ accessor: it acquires no lock and touches no
		/// Python, so it follows isModuleLoaded() and is safe only while the
		/// host does not race a call that changes runtime state.
		/// \returns NotInitialized, InvalidArgument when `moduleName` is empty,
		/// names a host binding, or names a module without a source file
		/// (neither has a generation), or ModuleNotFound when the module is
		/// not loaded.
		[[nodiscard]] Result<std::uint64_t>
			moduleGeneration(std::string_view moduleName) const;

		// -- Introspection ---------------------------------------------------

		/// \brief True when `moduleName` exposes a callable named
		/// `functionName`.
		///
		/// Reads the module namespace directly, so it never invokes the value,
		/// never triggers a module-level `__getattr__` or `__dir__`, and never
		/// imports anything. An absent name is Ok with value false. A name
		/// bound to a non-callable value reports TypeMismatch, like call(), so
		/// a host can distinguish "not implemented" from "implemented
		/// incorrectly". A callable supplied dynamically by a module-level
		/// `__getattr__` is reported absent; introspection never runs script
		/// code.
		/// \returns NotInitialized, InvalidArgument, ModuleNotFound when the
		/// module was never loaded, TypeMismatch when the name is not
		/// callable, or WrongThread when called from a thread that does not
		/// own the interpreter.
		[[nodiscard]] Result<bool>
			hasFunction(std::string_view moduleName,
						std::string_view functionName) const;

		/// \brief Names of the public callables in a loaded module, sorted.
		///
		/// "Public" means the name does not start with `_`, so dunder
		/// attributes and private helpers stay out of a console listing.
		/// Values that are not callable are omitted. The result is a namespace
		/// view: a public callable imported into the module appears too.
		/// Ordering is byte-wise ascending, independent of the platform locale.
		/// \returns NotInitialized, InvalidArgument, ModuleNotFound when the
		/// module was never loaded, or WrongThread when called from a thread
		/// that does not own the interpreter.
		[[nodiscard]] Result<std::vector<std::string>>
			listFunctions(std::string_view moduleName) const;

		// -- Invocation ------------------------------------------------------

		/// \brief Calls `functionName` in `moduleName` with no arguments.
		///
		/// Every call() and callOptional() overload, typed, policy-bearing, or
		/// neither, reports WrongThread when invoked from a thread other than
		/// the one that called initialize(). A refused policy-bearing call is
		/// not counted as a hook failure.
		Error call(std::string_view moduleName, std::string_view functionName);

		/// Calls `functionName` in `moduleName` with the given arguments.
		Error call(std::string_view moduleName, std::string_view functionName,
				   std::initializer_list<ScriptArgument> arguments);

		/// Like call(), but a function that does not exist is not an error.
		/// Absent optional hooks such as `on_start` therefore succeed silently.
		Error callOptional(std::string_view moduleName,
						   std::string_view functionName);

		/// Like call(), but a function that does not exist is not an error.
		Error callOptional(std::string_view moduleName,
						   std::string_view functionName,
						   std::initializer_list<ScriptArgument> arguments);

		/// \brief Calls `functionName`, measures it, and applies `policy` to a
		/// failure.
		///
		/// This is the overload a host uses for a hook it invokes every frame.
		/// The call is timed with a monotonic clock and the per-hook failure
		/// counters are updated; the Error-returning overloads below measure
		/// and suppress nothing, and pay for the counters only on a failure.
		///
		/// There is deliberately no `call<T>(FailurePolicy, ...)`: a value
		/// returning hook is not the per-frame failure path this policy
		/// targets, and timing one is a follow-up if a consumer needs it.
		///
		/// Repeated failure
		/// ----------------
		/// A failure is "repeated" when the same hook (module and function)
		/// fails consecutively with the same ErrorCode, Python exception type,
		/// and message. The traceback is not part of that identity: the same
		/// statement can move between lines while the failure stays the same.
		///
		/// FailurePolicy::ReportOnce returns the first failure of a streak as a
		/// failure and the identical repeats as Ok with `suppressed` set. The
		/// counters live in this runtime for the current run: initialize() and
		/// shutdown() clear them, and a successful call ends the streak, so a
		/// suppression never survives a restart or a recovery.
		///
		/// Every overload updates the counters, so a failure that an
		/// Error-returning call() already reported is suppressed by the first
		/// ReportOnce call that repeats it: a host adopting a policy mid-run
		/// sees `suppressed` before it has seen the error.
		///
		/// A call rejected before the hook runs (empty name, runtime not
		/// initialized, a thread that does not own the interpreter) is reported
		/// but not counted: consecutiveFailures stays zero. Such a call still
		/// sets `stopRequested` under FailurePolicy::Stop.
		///
		/// \returns The error, the measured `duration`, and the failure
		/// accounting. No report is ever fatal for a script failure, so the
		/// host loop only stops when the host honours stopRequested.
		[[nodiscard]] CallReport call(FailurePolicy policy,
									  std::string_view moduleName,
									  std::string_view functionName);

		/// Like the policy-bearing call(), passing `arguments` to the hook.
		[[nodiscard]] CallReport
			call(FailurePolicy policy, std::string_view moduleName,
				 std::string_view functionName,
				 std::initializer_list<ScriptArgument> arguments);

		/// \brief Like the policy-bearing call(), for an optional hook.
		///
		/// An absent function is a success, exactly like callOptional()'s, so
		/// it also ends any failure streak for that name.
		[[nodiscard]] CallReport callOptional(FailurePolicy policy,
											  std::string_view moduleName,
											  std::string_view functionName);

		/// Like the policy-bearing callOptional(), passing `arguments`.
		[[nodiscard]] CallReport
			callOptional(FailurePolicy policy, std::string_view moduleName,
						 std::string_view functionName,
						 std::initializer_list<ScriptArgument> arguments);

		/// \brief Calls `functionName` and reads its return value back as `T`.
		///
		/// `T` is one of bool, std::int64_t, double, or std::string. The
		/// conversion is strict: a result of another Python type reports
		/// TypeMismatch instead of converting silently, and a function that
		/// returns None yields a result that isOk() without hasValue().
		/// \returns FunctionNotFound when the function does not exist.
		template<ScriptReturnType T> [[nodiscard]] Result<T>
			call(std::string_view moduleName, std::string_view functionName);

		/// Like call<T>(), passing `arguments` to the function.
		template<ScriptReturnType T> [[nodiscard]] Result<T>
			call(std::string_view moduleName, std::string_view functionName,
				 std::initializer_list<ScriptArgument> arguments);

		/// \brief Like call<T>(), but a function that does not exist is not an
		/// error: the result isOk() without a value, just like a function that
		/// returned None.
		template<ScriptReturnType T>
		[[nodiscard]] Result<T> callOptional(std::string_view moduleName,
											 std::string_view functionName);

		/// Like callOptional<T>(), passing `arguments` to the function.
		template<ScriptReturnType T> [[nodiscard]] Result<T>
			callOptional(std::string_view moduleName,
						 std::string_view functionName,
						 std::initializer_list<ScriptArgument> arguments);

		private:
		struct Impl;

		std::unique_ptr<Impl> _impl;
	};
}	 // namespace mais
