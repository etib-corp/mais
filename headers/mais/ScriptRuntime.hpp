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
	/// The runtime keeps the GIL for the thread that called initialize(). Every
	/// method and every script callback must run on that thread. Script errors
	/// never propagate as C++ exceptions: they are reported through Error so a
	/// failed hook cannot silently unwind the host's loop.
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
		/// \returns NotInitialized when the runtime is not running.
		Error shutdown();

		/// True between a successful initialize() and shutdown().
		[[nodiscard]] bool isRunning() const noexcept;

		// -- Script loading --------------------------------------------------

		/// \brief Adds a directory that loadModule() searches for imports.
		/// \returns ScriptNotFound when `path` is not an existing directory.
		Error addSearchPath(const std::string &path);

		/// Search paths configured so far, in insertion order.
		[[nodiscard]] std::vector<std::string> searchPaths() const;

		/// Imports `moduleName` using the configured search paths.
		/// \returns NotInitialized, InvalidArgument, ModuleNotFound, or
		/// InvocationFailed when the module raises while importing.
		Error loadModule(const std::string &moduleName);

		/// True when `moduleName` was imported by loadModule() or registered as
		/// a host binding.
		[[nodiscard]] bool isModuleLoaded(const std::string &moduleName) const;

		// -- Invocation ------------------------------------------------------

		/// Calls `functionName` in `moduleName` with no arguments.
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

		private:
		struct Impl;

		std::unique_ptr<Impl> _impl;
	};
}	 // namespace mais
