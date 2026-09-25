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

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace mais
{
	/// \brief Opaque handle to a Python module created by the runtime.
	///
	/// maïs keeps pybind11 out of its public headers, so a binding callback
	/// receives this handle and reinterprets it in its own translation unit:
	///
	/// ```cpp
	/// runtime.registerModule("host", [&counter](mais::PythonModule &module) {
	///     pybind11::module_ &m = module.as<pybind11::module_>();
	///     pybind11::class_<Counter>(m, "Counter")
	///         .def("increment", &Counter::increment);
	///     m.attr("counter") = pybind11::cast(
	///         &counter, pybind11::return_value_policy::reference);
	/// });
	/// ```
	///
	/// The handle is valid only for the duration of the callback.
	class PythonModule
	{
		public:
		/// Wraps a module pointer; normally created by ScriptRuntime.
		explicit PythonModule(void *handle) noexcept;

		/// \brief Returns the runtime's module wrapper as `T`.
		///
		/// The runtime owns a `pybind11::module_` for the duration of the
		/// callback and this accessor returns a reference to that object. `T`
		/// is not checked at runtime, so it must be `pybind11::module_`:
		///
		/// ```cpp
		/// pybind11::module_ &m = module.as<pybind11::module_>();
		/// ```
		template<typename T> T &as() const noexcept
		{
			return *static_cast<T *>(_handle);
		}

		/// True when the handle refers to a live module.
		[[nodiscard]] bool valid() const noexcept;

		private:
		void *_handle = nullptr;
	};

	/// Callback that populates one embedded module.
	using BindingCallback = std::function<void(PythonModule &)>;

	/// A module the host exposes to scripts.
	struct ModuleBinding {
		/// Name the module is imported under, for example `host`.
		std::string name;

		/// Callback that populates the module before it becomes importable.
		BindingCallback callback;
	};

	/// \brief Collects the embedded modules a host wants to expose to scripts.
	///
	/// Bindings are queued before ScriptRuntime::initialize() and applied once
	/// the interpreter is running, so a callback may create pybind11 classes
	/// and attach host-owned objects. Because the interpreter outlives the
	/// callbacks but not the other way around, every attached native object
	/// must outlive the runtime; see ScriptRuntime's lifetime notes.
	class BindingRegistry
	{
		public:
		/// Queues `callback` as a module importable under `name`.
		/// \throws std::invalid_argument when `name` or `callback` is empty.
		void add(std::string name, BindingCallback callback);

		/// True when nothing has been registered.
		[[nodiscard]] bool empty() const noexcept;

		/// Number of registered modules.
		[[nodiscard]] std::size_t size() const noexcept;

		/// Registered modules, in registration order.
		[[nodiscard]] const std::vector<ModuleBinding> &
			modules() const noexcept;

		/// Removes every registered module.
		void clear() noexcept;

		private:
		std::vector<ModuleBinding> _modules;
	};
}	 // namespace mais
