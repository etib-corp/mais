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

#include "mais/ScriptRuntime.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

namespace py = pybind11;

namespace mais
{
	namespace
	{
		/// Python type name of the raised exception, e.g. "ValueError".
		std::string pythonErrorTypeName(const py::error_already_set &error)
		{
			try {
				return error.type().attr("__name__").cast<std::string>();
			} catch (const std::exception &) {
				return "PythonError";
			}
		}

		/// Maps a Python exception to the closest maïs error code.
		ErrorCode classifyPythonError(const py::error_already_set &error)
		{
			if (error.matches(PyExc_ModuleNotFoundError)) {
				return ErrorCode::ModuleNotFound;
			}
			if (error.matches(PyExc_AttributeError)) {
				return ErrorCode::FunctionNotFound;
			}
			if (error.matches(PyExc_TypeError)) {
				return ErrorCode::TypeMismatch;
			}
			return ErrorCode::InvocationFailed;
		}

		/// Converts a script argument into the Python object to pass along.
		py::object toPythonObject(const ScriptArgument &argument)
		{
			return std::visit(
				[](const auto &value) -> py::object {
					return py::cast(value);
				},
				argument.value());
		}

		/// Result of converting one returned Python value to a C++ scalar.
		enum class ConversionResult {
			Converted,
			WrongType,
			OutOfRange,
		};

		/// Converts a returned Python bool; True and False only.
		ConversionResult tryConvertReturnValue(const py::handle &value,
											   bool &out)
		{
			if (!PyBool_Check(value.ptr())) {
				return ConversionResult::WrongType;
			}
			out = value.ptr() == Py_True;
			return ConversionResult::Converted;
		}

		/// Converts a returned Python int; bool is excluded, 64-bit range
		/// enforced.
		ConversionResult tryConvertReturnValue(const py::handle &value,
											   std::int64_t &out)
		{
			// True and False are Python ints as well, so they are excluded
			// explicitly: a boolean return never reads back as 1 or 0.
			if (!PyLong_Check(value.ptr()) || PyBool_Check(value.ptr())) {
				return ConversionResult::WrongType;
			}

			int overflow = 0;
			const long long number =
				PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
			if (number == -1 && PyErr_Occurred()) {
				PyErr_Clear();
				return ConversionResult::WrongType;
			}
			if (overflow != 0) {
				return ConversionResult::OutOfRange;
			}
			out = static_cast<std::int64_t>(number);
			return ConversionResult::Converted;
		}

		/// Converts a returned Python float; an int is not silently widened.
		ConversionResult tryConvertReturnValue(const py::handle &value,
											   double &out)
		{
			if (!PyFloat_Check(value.ptr())) {
				return ConversionResult::WrongType;
			}
			out = PyFloat_AS_DOUBLE(value.ptr());
			return ConversionResult::Converted;
		}

		/// Converts a returned Python str as UTF-8, embedded NULs included.
		ConversionResult tryConvertReturnValue(const py::handle &value,
											   std::string &out)
		{
			if (!PyUnicode_Check(value.ptr())) {
				return ConversionResult::WrongType;
			}

			Py_ssize_t size	 = 0;
			const char *text = PyUnicode_AsUTF8AndSize(value.ptr(), &size);
			if (text == nullptr) {
				PyErr_Clear();
				return ConversionResult::WrongType;
			}
			out.assign(text, static_cast<std::size_t>(size));
			return ConversionResult::Converted;
		}

		/// Python type name of `value`, for diagnostics: "float", "dict".
		const char *pythonTypeName(const py::handle &value)
		{
			return Py_TYPE(value.ptr())->tp_name;
		}

		/// Name of the requested type, the way a script author writes it.
		template<typename T> const char *expectedTypeName()
		{
			if constexpr (std::same_as<T, bool>) {
				return "bool";
			} else if constexpr (std::same_as<T, std::int64_t>) {
				return "int";
			} else if constexpr (std::same_as<T, double>) {
				return "float";
			} else {
				return "str";
			}
		}

		/// \brief Converts a call result to `T`, or a strict TypeMismatch.
		///
		/// `context` is "module.function", for the diagnostic message.
		template<typename T>
		Result<T> convertReturnValue(const py::handle &value,
									 const std::string &context)
		{
			T converted {};
			switch (tryConvertReturnValue(value, converted)) {
				case ConversionResult::Converted:
					return Result<T>(std::move(converted));
				case ConversionResult::OutOfRange:
					// Only an int can exceed the range; the message names it.
					return Result<T>(
						Error(ErrorCode::TypeMismatch,
							  "'" + context
								  + "' returned an int outside the range of "
									"std::int64_t"));
				case ConversionResult::WrongType:
				default:
					return Result<T>(
						Error(ErrorCode::TypeMismatch,
							  "'" + context + "' returned "
								  + pythonTypeName(value) + ", but "
								  + expectedTypeName<T>() + " was requested"));
			}
		}
	}	 // namespace

	struct ScriptRuntime::Impl {
		/// False until initialize() succeeds, and again after shutdown().
		bool running = false;

		/// Owns the interpreter; resetting it finalizes Python.
		std::unique_ptr<py::scoped_interpreter> interpreter;

		/// Modules queued by registerModule(), kept for a later restart.
		BindingRegistry bindings;

		/// Directories added by addSearchPath(), in insertion order.
		std::vector<std::string> searchPaths;

		/// Every module the runtime imported or created, by module name.
		std::unordered_map<std::string, py::module_> modules;

		/// The `traceback` module, imported once in initialize() so that
		/// reporting a failure never imports anything while handling one.
		py::object tracebackModule;

		/// Builds an Error carrying the Python message and traceback.
		[[nodiscard]] Error
			fromPythonError(const py::error_already_set &error) const
		{
			const ErrorCode code = classifyPythonError(error);
			std::string message	 = error.what();
			if (message.empty()) {
				message = "Python raised " + pythonErrorTypeName(error);
			}
			return Error(code, std::move(message), formatTraceback(error));
		}

		/// Formats the exception pybind11 already captured, without touching
		/// the process-wide error indicator.
		[[nodiscard]] std::string
			formatTraceback(const py::error_already_set &error) const
		{
			if (!tracebackModule) {
				return {};
			}
			try {
				py::object lines = tracebackModule.attr("format_exception")(
					error.type(), error.value(), error.trace());
				return py::str("").attr("join")(lines).cast<std::string>();
			} catch (const std::exception &) {
				// A traceback is best-effort; the error message still stands.
				PyErr_Clear();
				return {};
			}
		}

		/// Converts one argument into the Python object to pass along.
		/// \returns false and fills `error` when the argument cannot be
		/// converted; `out` is untouched in that case.
		bool convertArgument(const ScriptArgument &argument, std::size_t index,
							 py::object &out, Error &error) const
		{
			const ScriptArgument::NativeObject *native =
				argument.nativeObject();
			if (native == nullptr) {
				out = std::visit(
					[](const auto &value) -> py::object {
						using Payload = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<
										  Payload,
										  ScriptArgument::NativeObject>) {
							// Native arguments are handled above.
							return py::none();
						} else {
							return py::cast(value);
						}
					},
					argument.value());
				return true;
			}

			if (!native->converter) {
				error = Error(ErrorCode::InvalidArgument,
							  "argument " + std::to_string(index)
								  + " is a native object but has no converter");
				return false;
			}

			void *handle = nullptr;
			try {
				handle = native->converter(native->value);
			} catch (py::error_already_set &pythonError) {
				Error failure = fromPythonError(pythonError);
				error		  = Error(failure.code(),
									  "argument " + std::to_string(index)
										  + " could not be converted to Python: "
										  + failure.message(),
									  failure.traceback());
				return false;
			} catch (const std::exception &conversionError) {
				PyErr_Clear();
				error = Error(ErrorCode::TypeMismatch,
							  "argument " + std::to_string(index)
								  + " could not be converted to Python: "
								  + conversionError.what());
				return false;
			}

			py::object converted;
			if (handle != nullptr) {
				converted = py::reinterpret_steal<py::object>(
					reinterpret_cast<PyObject *>(handle));
			}
			if (!converted || PyErr_Occurred()) {
				// Either the converter refused the value, or pybind11 left a
				// Python error set — an unregistered class does exactly that.
				// Fetch it now so it cannot leak into the next Python call.
				if (PyErr_Occurred()) {
					py::error_already_set pythonError;
					Error failure = fromPythonError(pythonError);
					error		  = Error(failure.code(),
										  "argument " + std::to_string(index)
											  + " could not be converted to Python: "
											  + failure.message(),
										  failure.traceback());
					return false;
				}
				error = Error(ErrorCode::TypeMismatch,
							  "argument " + std::to_string(index)
								  + " (native) could not be converted to a "
									"Python object");
				return false;
			}
			out = std::move(converted);
			return true;
		}

		/// Prepends `path` to `sys.path`; requires a live interpreter.
		Error insertSearchPath(const std::string &path)
		{
			try {
				py::object sysPath = py::module_::import("sys").attr("path");
				sysPath.attr("insert")(0, py::str(path));
				return Error::ok();
			} catch (py::error_already_set &error) {
				return fromPythonError(error);
			} catch (const std::exception &error) {
				return Error(ErrorCode::InterpreterFailure,
							 "could not add '" + path
								 + "' to sys.path: " + error.what());
			}
		}

		/// Creates one embedded module and runs the host's binding callback.
		Error applyBinding(const ModuleBinding &binding)
		{
			py::module_ module = py::reinterpret_steal<py::module_>(
				PyModule_New(binding.name.c_str()));
			if (!module) {
				return Error(ErrorCode::InterpreterFailure,
							 "could not create the embedded module '"
								 + binding.name + "'");
			}

			// Register before running the callback so the module can already
			// import itself, and so scripts can `import <name>` afterwards.
			py::object sysModules = py::module_::import("sys").attr("modules");
			sysModules[binding.name.c_str()] = module;

			try {
				// The handle carries the runtime's own module wrapper, so
				// PythonModule::as<pybind11::module_>() hands the callback the
				// module it should populate.
				PythonModule handle(&module);
				binding.callback(handle);
			} catch (py::error_already_set &error) {
				Error failure = fromPythonError(error);
				removeModule(binding.name, sysModules);
				return Error(failure.code(),
							 "binding module '" + binding.name
								 + "' failed: " + failure.message(),
							 failure.traceback());
			} catch (const std::exception &error) {
				removeModule(binding.name, sysModules);
				return Error(ErrorCode::InvocationFailed,
							 "binding module '" + binding.name
								 + "' threw: " + error.what());
			}

			modules.emplace(binding.name, module);
			return Error::ok();
		}

		/// Undoes a partially applied binding.
		void removeModule(const std::string &name, py::object &sysModules)
		{
			try {
				sysModules.attr("__delitem__")(py::str(name));
			} catch (const std::exception &) {
				PyErr_Clear();
			}
		}

		/// Releases Python references, then finalizes the interpreter.
		void stop() noexcept
		{
			// Python objects must be dropped while the interpreter is alive, so
			// the module table is emptied before finalization.
			modules.clear();
			tracebackModule = py::object();
			try {
				interpreter.reset();
			} catch (const std::exception &) {
				// Finalization is best-effort; the runtime state is reset
				// below.
			}
			running = false;
		}

		/// The Python result of one invocation, or the error that replaced it.
		struct CallOutcome {
			Error error;
			py::object value;
		};

		/// Shared argument checks for every call() overload.
		[[nodiscard]] Error validateCall(std::string_view moduleName,
										 std::string_view functionName) const
		{
			if (!running) {
				return Error(ErrorCode::NotInitialized,
							 "the scripting runtime is not running");
			}
			if (moduleName.empty()) {
				return Error(ErrorCode::InvalidArgument,
							 "module name is empty");
			}
			if (functionName.empty()) {
				return Error(ErrorCode::InvalidArgument,
							 "function name is empty");
			}
			return Error::ok();
		}

		/// \brief Finds and invokes the function; the caller holds the GIL.
		///
		/// A missing function is not an error when `optional`: the outcome is
		/// then Ok without a value, which the typed path reports as a result
		/// without a value and the untyped path as plain success.
		[[nodiscard]] CallOutcome
			invoke(const std::string &moduleKey, const std::string &functionKey,
				   const std::vector<ScriptArgument> &arguments, bool optional)
		{
			CallOutcome outcome;
			try {
				auto module = modules.find(moduleKey);
				if (module == modules.end()) {
					outcome.error =
						Error(ErrorCode::ModuleNotFound,
							  "script module '" + moduleKey
								  + "' is not loaded; call loadModule() first");
					return outcome;
				}

				py::object function = py::getattr(
					module->second, functionKey.c_str(), py::none());
				if (function.is_none()) {
					if (!optional) {
						outcome.error =
							Error(ErrorCode::FunctionNotFound,
								  "'" + moduleKey + "' has no function '"
									  + functionKey + "'");
					}
					return outcome;
				}
				if (!PyCallable_Check(function.ptr())) {
					outcome.error = Error(ErrorCode::TypeMismatch,
										  "'" + moduleKey + "." + functionKey
											  + "' is not callable");
					return outcome;
				}

				// Convert every argument before touching Python so a failed
				// conversion leaves no partially built tuple behind.
				std::vector<py::object> converted;
				converted.reserve(arguments.size());
				for (std::size_t index = 0; index < arguments.size(); ++index) {
					py::object object;
					Error conversionFailure;
					if (!convertArgument(arguments[index], index, object,
										 conversionFailure)) {
						return conversionFailure;
					}
					converted.push_back(std::move(object));
				}

				py::tuple packed(converted.size());
				for (std::size_t index = 0; index < converted.size(); ++index) {
					// PyTuple_SetItem steals the reference it is handed.
					if (PyTuple_SetItem(packed.ptr(),
										static_cast<Py_ssize_t>(index),
										converted[index].release().ptr())
						!= 0) {
						throw py::error_already_set();
					}
				}

				outcome.value = py::reinterpret_steal<py::object>(
					PyObject_CallObject(function.ptr(), packed.ptr()));
				if (!outcome.value) {
					throw py::error_already_set();
				}
				return outcome;
			} catch (py::error_already_set &error) {
				outcome.value = py::object();
				outcome.error = fromPythonError(error);
				return outcome;
			} catch (const std::exception &error) {
				outcome.value = py::object();
				outcome.error =
					Error(ErrorCode::InvocationFailed,
						  "calling '" + moduleKey + "." + functionKey
							  + "' failed: " + error.what());
				return outcome;
			}
		}

		/// Implements the Error-returning call() and callOptional().
		Error callImpl(std::string_view moduleName,
					   std::string_view functionName,
					   const std::vector<ScriptArgument> &arguments,
					   bool optional)
		{
			if (Error invalid = validateCall(moduleName, functionName);
				invalid) {
				return invalid;
			}

			py::gil_scoped_acquire gil;
			CallOutcome outcome =
				invoke(std::string(moduleName), std::string(functionName),
					   arguments, optional);
			return std::move(outcome.error);
		}

		/// Implements the typed call() and callOptional() overloads.
		template<ScriptReturnType T>
		Result<T> callTyped(std::string_view moduleName,
							std::string_view functionName,
							const std::vector<ScriptArgument> &arguments,
							bool optional)
		{
			if (Error invalid = validateCall(moduleName, functionName);
				invalid) {
				return Result<T>(std::move(invalid));
			}

			py::gil_scoped_acquire gil;
			CallOutcome outcome =
				invoke(std::string(moduleName), std::string(functionName),
					   arguments, optional);
			if (outcome.error) {
				return Result<T>(std::move(outcome.error));
			}
			if (!outcome.value || outcome.value.is_none()) {
				return Result<T>::none();
			}

			const std::string context =
				std::string(moduleName) + "." + std::string(functionName);
			return convertReturnValue<T>(outcome.value, context);
		}
	};

	ScriptArgument::ScriptArgument(ScriptArgument::Value value)
		: _value(std::move(value))
	{
	}

	ScriptArgument ScriptArgument::integer(std::int64_t value)
	{
		return ScriptArgument(Value { value });
	}

	ScriptArgument ScriptArgument::number(double value)
	{
		return ScriptArgument(Value { value });
	}

	ScriptArgument ScriptArgument::boolean(bool value)
	{
		return ScriptArgument(Value { value });
	}

	ScriptArgument ScriptArgument::string(std::string value)
	{
		return ScriptArgument(Value { std::move(value) });
	}

	ScriptArgument ScriptArgument::native(const void *value,
										  NativeConverter converter)
	{
		return ScriptArgument(
			Value { NativeObject { value, std::move(converter) } });
	}

	const ScriptArgument::Value &ScriptArgument::value() const noexcept
	{
		return _value;
	}

	const ScriptArgument::NativeObject *
		ScriptArgument::nativeObject() const noexcept
	{
		return std::get_if<NativeObject>(&_value);
	}

	const char *ScriptArgument::typeName() const noexcept
	{
		return std::visit(
			[](const auto &value) -> const char * {
				using Payload = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<Payload, std::int64_t>) {
					return "int";
				} else if constexpr (std::is_same_v<Payload, double>) {
					return "float";
				} else if constexpr (std::is_same_v<Payload, bool>) {
					return "bool";
				} else if constexpr (std::is_same_v<Payload, NativeObject>) {
					return "native";
				} else {
					return "str";
				}
			},
			_value);
	}

	ScriptRuntime::ScriptRuntime()
		: _impl(std::make_unique<Impl>())
	{
	}

	ScriptRuntime::~ScriptRuntime()
	{
		_impl->stop();
	}

	Error ScriptRuntime::registerModule(std::string name,
										BindingCallback callback)
	{
		if (_impl->running) {
			return Error(ErrorCode::AlreadyInitialized,
						 "registerModule() must be called before initialize()");
		}
		try {
			_impl->bindings.add(std::move(name), std::move(callback));
		} catch (const std::invalid_argument &error) {
			return Error(ErrorCode::InvalidArgument, error.what());
		}
		return Error::ok();
	}

	const BindingRegistry &ScriptRuntime::bindings() const noexcept
	{
		return _impl->bindings;
	}

	Error ScriptRuntime::initialize()
	{
		if (_impl->running) {
			return Error(ErrorCode::AlreadyInitialized,
						 "this runtime already owns a running interpreter");
		}
		if (Py_IsInitialized()) {
			return Error(
				ErrorCode::InterpreterFailure,
				"a Python interpreter is already running in this process; "
				"maïs owns its own interpreter exclusively");
		}

		try {
			// Signal handlers belong to the host, so they are left untouched,
			// and sys.path is managed through addSearchPath() instead of the
			// executable's directory.
			_impl->interpreter = std::make_unique<py::scoped_interpreter>(
				false, 0, nullptr, false);
		} catch (const std::exception &error) {
			return Error(ErrorCode::InterpreterFailure,
						 std::string("could not start Python: ")
							 + error.what());
		}
		_impl->running = true;

		try {
			// Cache the module now so that reporting a later failure never has
			// to import anything while handling an exception.
			_impl->tracebackModule = py::module_::import("traceback");
		} catch (const std::exception &error) {
			_impl->stop();
			return Error(
				ErrorCode::InterpreterFailure,
				std::string("could not import the Python 'traceback' module: ")
					+ error.what());
		}

		// initialize() is atomic: a failure leaves Python stopped rather than
		// handing the host a half-configured runtime.
		for (const std::string &path: _impl->searchPaths) {
			Error error = _impl->insertSearchPath(path);
			if (error) {
				_impl->stop();
				return error;
			}
		}

		for (const ModuleBinding &binding: _impl->bindings.modules()) {
			Error error = _impl->applyBinding(binding);
			if (error) {
				_impl->stop();
				return error;
			}
		}
		return Error::ok();
	}

	Error ScriptRuntime::shutdown()
	{
		if (!_impl->running) {
			return Error(ErrorCode::NotInitialized,
						 "the scripting runtime is not running");
		}
		_impl->stop();
		return Error::ok();
	}

	bool ScriptRuntime::isRunning() const noexcept
	{
		return _impl->running;
	}

	Error ScriptRuntime::addSearchPath(const std::string &path)
	{
		if (path.empty()) {
			return Error(ErrorCode::InvalidArgument, "search path is empty");
		}

		std::error_code filesystemError;
		if (!std::filesystem::is_directory(path, filesystemError)) {
			return Error(ErrorCode::ScriptNotFound,
						 "'" + path + "' is not an existing directory");
		}

		const std::vector<std::string> &paths = _impl->searchPaths;
		if (std::find(paths.begin(), paths.end(), path) != paths.end()) {
			return Error::ok();
		}
		_impl->searchPaths.push_back(path);

		if (_impl->running) {
			py::gil_scoped_acquire gil;
			return _impl->insertSearchPath(path);
		}
		return Error::ok();
	}

	std::vector<std::string> ScriptRuntime::searchPaths() const
	{
		return _impl->searchPaths;
	}

	Error ScriptRuntime::loadModule(const std::string &moduleName)
	{
		if (!_impl->running) {
			return Error(ErrorCode::NotInitialized,
						 "the scripting runtime is not running");
		}
		if (moduleName.empty()) {
			return Error(ErrorCode::InvalidArgument, "module name is empty");
		}

		py::gil_scoped_acquire gil;
		try {
			py::module_ module = py::module_::import(moduleName.c_str());
			_impl->modules[moduleName] = module;
			return Error::ok();
		} catch (py::error_already_set &error) {
			Error failure		   = _impl->fromPythonError(error);
			const std::string what = "could not import script module '"
				+ moduleName + "': " + failure.message();
			return Error(failure.code(), what, failure.traceback());
		} catch (const std::exception &error) {
			return Error(ErrorCode::InvocationFailed,
						 "could not import script module '" + moduleName
							 + "': " + error.what());
		}
	}

	bool ScriptRuntime::isModuleLoaded(const std::string &moduleName) const
	{
		return _impl->modules.find(moduleName) != _impl->modules.end();
	}

	Error ScriptRuntime::call(std::string_view moduleName,
							  std::string_view functionName)
	{
		return _impl->callImpl(moduleName, functionName, {}, false);
	}

	Error ScriptRuntime::call(std::string_view moduleName,
							  std::string_view functionName,
							  std::initializer_list<ScriptArgument> arguments)
	{
		return _impl->callImpl(moduleName, functionName,
							   std::vector<ScriptArgument>(arguments), false);
	}

	Error ScriptRuntime::callOptional(std::string_view moduleName,
									  std::string_view functionName)
	{
		return _impl->callImpl(moduleName, functionName, {}, true);
	}

	Error ScriptRuntime::callOptional(
		std::string_view moduleName, std::string_view functionName,
		std::initializer_list<ScriptArgument> arguments)
	{
		return _impl->callImpl(moduleName, functionName,
							   std::vector<ScriptArgument>(arguments), true);
	}

	template<ScriptReturnType T>
	Result<T> ScriptRuntime::call(std::string_view moduleName,
								  std::string_view functionName)
	{
		return _impl->callTyped<T>(moduleName, functionName, {}, false);
	}

	template<ScriptReturnType T> Result<T>
		ScriptRuntime::call(std::string_view moduleName,
							std::string_view functionName,
							std::initializer_list<ScriptArgument> arguments)
	{
		return _impl->callTyped<T>(moduleName, functionName,
								   std::vector<ScriptArgument>(arguments),
								   false);
	}

	template<ScriptReturnType T>
	Result<T> ScriptRuntime::callOptional(std::string_view moduleName,
										  std::string_view functionName)
	{
		return _impl->callTyped<T>(moduleName, functionName, {}, true);
	}

	template<ScriptReturnType T> Result<T> ScriptRuntime::callOptional(
		std::string_view moduleName, std::string_view functionName,
		std::initializer_list<ScriptArgument> arguments)
	{
		return _impl->callTyped<T>(moduleName, functionName,
								   std::vector<ScriptArgument>(arguments),
								   true);
	}

	// The ScriptReturnType concept admits exactly these four types, so these
	// explicit instantiations are the complete link surface for typed calls.
	// Defining them in this translation unit is what keeps pybind11 out of
	// headers/mais/.
	template Result<bool> ScriptRuntime::call<bool>(std::string_view,
													std::string_view);
	template Result<std::int64_t>
		ScriptRuntime::call<std::int64_t>(std::string_view, std::string_view);
	template Result<double> ScriptRuntime::call<double>(std::string_view,
														std::string_view);
	template Result<std::string>
		ScriptRuntime::call<std::string>(std::string_view, std::string_view);

	template Result<bool>
		ScriptRuntime::call<bool>(std::string_view, std::string_view,
								  std::initializer_list<ScriptArgument>);
	template Result<std::int64_t> ScriptRuntime::call<std::int64_t>(
		std::string_view, std::string_view,
		std::initializer_list<ScriptArgument>);
	template Result<double>
		ScriptRuntime::call<double>(std::string_view, std::string_view,
									std::initializer_list<ScriptArgument>);
	template Result<std::string>
		ScriptRuntime::call<std::string>(std::string_view, std::string_view,
										 std::initializer_list<ScriptArgument>);

	template Result<bool> ScriptRuntime::callOptional<bool>(std::string_view,
															std::string_view);
	template Result<std::int64_t>
		ScriptRuntime::callOptional<std::int64_t>(std::string_view,
												  std::string_view);
	template Result<double>
		ScriptRuntime::callOptional<double>(std::string_view, std::string_view);
	template Result<std::string>
		ScriptRuntime::callOptional<std::string>(std::string_view,
												 std::string_view);

	template Result<bool> ScriptRuntime::callOptional<bool>(
		std::string_view, std::string_view,
		std::initializer_list<ScriptArgument>);
	template Result<std::int64_t> ScriptRuntime::callOptional<std::int64_t>(
		std::string_view, std::string_view,
		std::initializer_list<ScriptArgument>);
	template Result<double> ScriptRuntime::callOptional<double>(
		std::string_view, std::string_view,
		std::initializer_list<ScriptArgument>);
	template Result<std::string> ScriptRuntime::callOptional<std::string>(
		std::string_view, std::string_view,
		std::initializer_list<ScriptArgument>);
}	 // namespace mais
