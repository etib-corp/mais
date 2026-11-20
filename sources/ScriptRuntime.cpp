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

#include <frameobject.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
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

		/// Converts a Python string to UTF-8; empty when it is not one.
		std::string unicodeToUtf8(PyObject *object)
		{
			if (object == nullptr || !PyUnicode_Check(object)) {
				return {};
			}

			Py_ssize_t size	 = 0;
			const char *text = PyUnicode_AsUTF8AndSize(object, &size);

			if (text == nullptr) {
				PyErr_Clear();
				return {};
			}
			return std::string(text, static_cast<std::size_t>(size));
		}

		/// \brief Sets `__mais_generation__` on a module's namespace.
		///
		/// False when the module has no dictionary or the insertion fails; the
		/// caller reports that rather than leaving a half-configured module.
		bool setModuleGeneration(py::module_ &module, std::uint64_t generation)
		{
			PyObject *namespaceObject = PyModule_GetDict(module.ptr());
			if (namespaceObject == nullptr) {
				PyErr_Clear();
				return false;
			}

			py::object value = py::cast(generation);
			if (PyDict_SetItemString(namespaceObject, "__mais_generation__",
									 value.ptr())
				!= 0) {
				PyErr_Clear();
				return false;
			}
			return true;
		}

		/// \brief The absolute source path of an importable module, or empty.
		///
		/// Only a `.py` file is a source the reload can compile. A host binding
		/// has no `__file__`, an extension module points at a shared library
		/// and a sourceless `.pyc` points at bytecode, so none of them is
		/// source-backed and all are refused by reloadModule().
		std::string capturedSourcePath(const py::module_ &module)
		{
			py::object file = py::getattr(module, "__file__", py::none());
			if (!file || !PyUnicode_Check(file.ptr())) {
				return {};
			}

			const std::filesystem::path path(unicodeToUtf8(file.ptr()));
			std::string extension = path.extension().string();
			std::transform(extension.begin(), extension.end(),
						   extension.begin(), [](unsigned char character) {
							   return static_cast<char>(
								   std::tolower(character));
						   });
			if (extension != ".py") {
				return {};
			}

			std::error_code error;
			const std::filesystem::path absolute =
				std::filesystem::absolute(path, error);
			if (error) {
				return path.lexically_normal().string();
			}
			return absolute.lexically_normal().string();
		}

		/// Copies frames out of the traceback pybind11 already captured.
		/// Keeps the innermost Error::maxFrames frames, outermost first.
		std::vector<Error::Frame>
			extractFrames(const py::error_already_set &error)
		{
			try {
				const py::object &trace = error.trace();

				if (!trace || !PyTraceBack_Check(trace.ptr())) {
					return {};
				}

				std::deque<Error::Frame> frames;

				for (PyTracebackObject *entry =
						 reinterpret_cast<PyTracebackObject *>(trace.ptr());
					 entry != nullptr; entry = entry->tb_next) {
					Error::Frame frame;
					// Python 3.14 stores -1 and resolves the line lazily,
					// exactly as the tb_lineno getter does.
					int line = entry->tb_lineno;

					if (entry->tb_frame != nullptr) {
						py::object code = py::reinterpret_steal<py::object>(
							reinterpret_cast<PyObject *>(
								PyFrame_GetCode(entry->tb_frame)));

						if (code) {
							auto *codeObject =
								reinterpret_cast<PyCodeObject *>(code.ptr());

							frame.file = unicodeToUtf8(codeObject->co_filename);
							frame.function = unicodeToUtf8(codeObject->co_name);
							if (line < 0) {
								line = PyCode_Addr2Line(codeObject,
														entry->tb_lasti);
							}
						}
					}
					if (line > 0) {
						frame.line = static_cast<std::size_t>(line);
					}
					frames.push_back(std::move(frame));
					if (frames.size() > Error::maxFrames) {
						frames.pop_front();
					}
				}
				return { std::make_move_iterator(frames.begin()),
						 std::make_move_iterator(frames.end()) };
			} catch (const std::exception &) {
				// Frames are best-effort; the error message still stands.
				PyErr_Clear();
				return {};
			}
		}

		/// Rebuilds `failure` with a host-facing message while preserving
		/// every Python diagnostic it carries.
		Error withDiagnostics(const Error &failure, std::string message)
		{
			return Error(failure.code(), std::move(message),
						 failure.traceback(), failure.exceptionType(),
						 failure.frames());
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

		/// The refusal for a Python-touching call from a thread that does not
		/// own the interpreter. Without it the caller would wait for a GIL the
		/// owning thread never releases.
		Error threadAffinityError(std::string_view operation)
		{
			return Error(ErrorCode::WrongThread,
						 std::string(operation)
							 + " must run on the thread that called "
							   "initialize(): a ScriptRuntime keeps its "
							   "interpreter and the GIL on that thread");
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

		/// State of a name in a module namespace.
		enum class NamespaceEntry {
			Absent,
			NotCallable,
			Callable,
		};

		/// \brief Looks a name up directly in a module's dictionary.
		///
		/// Deliberately bypasses getattr: a module-level __getattr__ is script
		/// code, and introspection must not run any. The lookup also avoids
		/// dir(), which may call a script-defined __dir__.
		NamespaceEntry namespaceEntry(PyObject *namespaceObject,
									  std::string_view name)
		{
			py::object key =
				py::reinterpret_steal<py::object>(PyUnicode_FromStringAndSize(
					name.data(), static_cast<Py_ssize_t>(name.size())));
			if (!key) {
				// Not representable as Python text, so no key can match it.
				PyErr_Clear();
				return NamespaceEntry::Absent;
			}

			PyObject *value = PyDict_GetItem(namespaceObject, key.ptr());
			if (value == nullptr) {
				return NamespaceEntry::Absent;
			}
			return PyCallable_Check(value) != 0 ? NamespaceEntry::Callable
												: NamespaceEntry::NotCallable;
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

		/// \brief Key of the per-hook failure counters.
		///
		/// The unit separator keeps the usual pair unambiguous. It is not a
		/// guarantee: names are validated as non-empty, not as Python
		/// identifiers, so a name that itself contains the separator can
		/// collide with another pair. The damage is bounded — such a name
		/// cannot resolve to a Python module or attribute, so the calls whose
		/// streaks get merged fail either way.
		std::string hookKey(std::string_view moduleName,
							std::string_view functionName)
		{
			std::string key;

			key.reserve(moduleName.size() + functionName.size() + 1);
			key.append(moduleName);
			key.push_back('\x1f');
			key.append(functionName);
			return key;
		}

		/// \brief Identity of a failure for FailurePolicy::ReportOnce.
		///
		/// Two failures are the same failure when they share an ErrorCode, a
		/// Python exception type, and a message. The traceback is excluded on
		/// purpose: a line can move while the failure stays the same.
		std::string failureSignature(const Error &failure)
		{
			std::string signature;

			signature.reserve(failure.message().size()
							  + failure.exceptionType().size() + 32);
			signature.append(errorCodeName(failure.code()));
			signature.push_back('\x1f');
			signature.append(failure.exceptionType());
			signature.push_back('\x1f');
			signature.append(failure.message());
			return signature;
		}
	}	 // namespace

	struct ScriptRuntime::Impl {
		/// False until initialize() succeeds, and again after shutdown().
		bool running = false;

		/// Thread that called initialize(); no thread while stopped.
		std::thread::id ownerThread;

		/// Owns the interpreter; resetting it finalizes Python.
		std::unique_ptr<py::scoped_interpreter> interpreter;

		/// Modules queued by registerModule(), kept for a later restart.
		BindingRegistry bindings;

		/// Directories added by addSearchPath(), in insertion order.
		std::vector<std::string> searchPaths;

		/// \brief A module the runtime imported or created.
		///
		/// A host binding has no source path and no generation: it is
		/// interpreter infrastructure, not a reloadable script.
		struct LoadedModule {
			py::module_ module;
			std::string sourcePath;
			std::uint64_t generation = 0;
			bool binding			 = false;
		};

		/// Every module the runtime imported or created, by module name.
		std::unordered_map<std::string, LoadedModule> modules;

		/// The `traceback` module, imported once in initialize() so that
		/// reporting a failure never imports anything while handling one.
		py::object tracebackModule;

		/// Cached `builtins`, used by reloadModule() to compile source text.
		py::object builtinsModule;

		/// Cached `importlib`, used for invalidate_caches().
		py::object importlibModule;

		/// Cached `importlib.util`, used to rebuild a module from its spec.
		py::object importlibUtilModule;

		/// \brief Failure accounting for one hook in the current run.
		struct FailureRecord {
			/// Consecutive failures, reset by a successful call.
			std::uint64_t consecutiveFailures = 0;

			/// Error code, exception type, and message of the last failure.
			std::string lastSignature;
		};

		/// \brief Per-hook failure counters, keyed by module and function.
		///
		/// Cleared by stop(), so a restart starts from zero and never inherits
		/// a suppression from the previous run.
		std::unordered_map<std::string, FailureRecord> failureRecords;

		/// \brief Result of booking one failed call.
		struct FailureVisit {
			/// Failures of this hook since its last success.
			std::uint64_t consecutiveFailures = 0;

			/// True when this failure matches the previous one exactly.
			bool repeated = false;
		};

		/// Counts one failure for `moduleName.functionName`.
		[[nodiscard]] FailureVisit noteFailure(std::string_view moduleName,
											   std::string_view functionName,
											   const Error &failure)
		{
			FailureRecord &record =
				failureRecords[hookKey(moduleName, functionName)];

			++record.consecutiveFailures;

			std::string signature = failureSignature(failure);
			const bool repeated	  = signature == record.lastSignature;

			record.lastSignature = std::move(signature);
			return FailureVisit { record.consecutiveFailures, repeated };
		}

		/// \brief Ends a hook's failure streak after a successful call.
		///
		/// A successful call is the common case, so the empty-map check keeps
		/// the fast path free of key construction and hashing.
		void noteSuccess(std::string_view moduleName,
						 std::string_view functionName)
		{
			if (!failureRecords.empty()) {
				failureRecords.erase(hookKey(moduleName, functionName));
			}
		}

		/// True when the calling thread started the running interpreter.
		[[nodiscard]] bool ownsInterpreter() const noexcept
		{
			return std::this_thread::get_id() == ownerThread;
		}

		/// Builds an Error carrying the Python message, traceback, exception
		/// type, and structured frames.
		[[nodiscard]] Error
			fromPythonError(const py::error_already_set &error) const
		{
			const ErrorCode code	  = classifyPythonError(error);
			std::string exceptionType = pythonErrorTypeName(error);
			std::string message		  = error.what();

			if (message.empty()) {
				message = "Python raised " + exceptionType;
			}
			return Error(code, std::move(message), formatTraceback(error),
						 std::move(exceptionType), extractFrames(error));
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
				// compile() raises without a traceback object, but
				// format_exception still renders the exception and its
				// file/line when given None explicitly.
				py::object trace = error.trace();

				if (!trace) {
					trace = py::none();
				}

				py::object lines = tracebackModule.attr("format_exception")(
					error.type(), error.value(), trace);
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

				error =
					withDiagnostics(failure,
									"argument " + std::to_string(index)
										+ " could not be converted to Python: "
										+ failure.message());
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

					error = withDiagnostics(
						failure,
						"argument " + std::to_string(index)
							+ " could not be converted to Python: "
							+ failure.message());
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

		/// \brief Inserts `path` into `sys.path` at `index`.
		///
		/// The runtime's paths form one leading block in insertion order, so
		/// the directory added first is the one Python searches first.
		/// Requires a live interpreter.
		Error insertSearchPath(const std::string &path, std::size_t index)
		{
			try {
				py::object sysPath = py::module_::import("sys").attr("path");
				sysPath.attr("insert")(index, py::str(path));
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
				return withDiagnostics(failure,
									   "binding module '" + binding.name
										   + "' failed: " + failure.message());
			} catch (const std::exception &error) {
				removeModule(binding.name, sysModules);
				return Error(ErrorCode::InvocationFailed,
							 "binding module '" + binding.name
								 + "' threw: " + error.what());
			}

			LoadedModule loaded;

			loaded.module  = module;
			loaded.binding = true;
			modules.emplace(binding.name, std::move(loaded));
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
			// Failure counters belong to one run: a restart must not inherit a
			// suppression from the previous one.
			failureRecords.clear();

			// Python objects must be dropped while the interpreter is alive, so
			// the module table is emptied before finalization.
			modules.clear();
			tracebackModule		= py::object();
			builtinsModule		= py::object();
			importlibModule		= py::object();
			importlibUtilModule = py::object();
			try {
				interpreter.reset();
			} catch (const std::exception &) {
				// Finalization is best-effort; the runtime state is reset
				// below.
			}
			running		= false;
			ownerThread = std::thread::id();
		}

		/// The Python result of one invocation, or the error that replaced it.
		struct CallOutcome {
			Error error;
			py::object value;
		};

		/// \brief Finds a module loaded by loadModule() or a host binding.
		///
		/// Fills `error` with the same ModuleNotFound report call() uses, so
		/// invocation and introspection cannot drift apart.
		[[nodiscard]] py::module_ *findLoadedModule(std::string_view moduleName,
													Error &error)
		{
			auto module = modules.find(std::string(moduleName));

			if (module == modules.end()) {
				error = Error(ErrorCode::ModuleNotFound,
							  "script module '" + std::string(moduleName)
								  + "' is not loaded; call loadModule() first");
				return nullptr;
			}
			return &module->second.module;
		}

		/// Shared module checks for calls and introspection queries.
		/// `operation` names the refused call in a WrongThread diagnostic.
		[[nodiscard]] Error validateModule(std::string_view moduleName,
										   std::string_view operation) const
		{
			if (!running) {
				return Error(ErrorCode::NotInitialized,
							 "the scripting runtime is not running");
			}
			if (!ownsInterpreter()) {
				return threadAffinityError(operation);
			}
			if (moduleName.empty()) {
				return Error(ErrorCode::InvalidArgument,
							 "module name is empty");
			}
			return Error::ok();
		}

		/// Shared argument checks for every call() overload.
		[[nodiscard]] Error validateCall(std::string_view moduleName,
										 std::string_view functionName,
										 std::string_view operation) const
		{
			Error invalid = validateModule(moduleName, operation);

			if (invalid) {
				return invalid;
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
				Error lookupFailure;
				py::module_ *module =
					findLoadedModule(moduleKey, lookupFailure);

				if (module == nullptr) {
					outcome.error = std::move(lookupFailure);
					return outcome;
				}

				py::object function =
					py::getattr(*module, functionKey.c_str(), py::none());

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
						outcome.error = std::move(conversionFailure);
						return outcome;
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
			if (Error invalid = validateCall(moduleName, functionName,
											 "call()/callOptional()");
				invalid) {
				return invalid;
			}

			py::gil_scoped_acquire gil;
			CallOutcome outcome =
				invoke(std::string(moduleName), std::string(functionName),
					   arguments, optional);
			noteOutcome(moduleName, functionName, outcome.error);
			return std::move(outcome.error);
		}

		/// Keeps the per-hook counters truthful across every overload: a
		/// success ends the streak, a failure advances it. Only the
		/// policy-bearing overloads use the result to suppress a report.
		void noteOutcome(std::string_view moduleName,
						 std::string_view functionName, const Error &error)
		{
			if (error.isOk()) {
				noteSuccess(moduleName, functionName);
			} else {
				// The counters advance; only a policy-bearing call acts on it.
				(void)noteFailure(moduleName, functionName, error);
			}
		}

		/// Implements the typed call() and callOptional() overloads.
		template<ScriptReturnType T>
		Result<T> callTyped(std::string_view moduleName,
							std::string_view functionName,
							const std::vector<ScriptArgument> &arguments,
							bool optional)
		{
			if (Error invalid = validateCall(moduleName, functionName,
											 "call()/callOptional()");
				invalid) {
				return Result<T>(std::move(invalid));
			}

			py::gil_scoped_acquire gil;
			CallOutcome outcome =
				invoke(std::string(moduleName), std::string(functionName),
					   arguments, optional);
			if (outcome.error) {
				noteOutcome(moduleName, functionName, outcome.error);
				return Result<T>(std::move(outcome.error));
			}
			if (!outcome.value || outcome.value.is_none()) {
				noteSuccess(moduleName, functionName);
				return Result<T>::none();
			}

			const std::string context =
				std::string(moduleName) + "." + std::string(functionName);
			Result<T> converted = convertReturnValue<T>(outcome.value, context);

			// A wrong return type is a failure of the call too, so the streak
			// stays truthful whichever overload the host used.
			noteOutcome(moduleName, functionName, converted.error());
			return converted;
		}

		/// \brief Implements the policy-bearing call() and callOptional()
		/// overloads.
		///
		/// The clock spans validation, argument conversion, the Python call,
		/// and the capture of its failure, so `duration` is the cost the host
		/// actually pays for that hook.
		[[nodiscard]] CallReport
			callWithPolicy(FailurePolicy policy, std::string_view moduleName,
						   std::string_view functionName,
						   const std::vector<ScriptArgument> &arguments,
						   bool optional)
		{
			CallReport report;
			const auto started = std::chrono::steady_clock::now();

			Error invalid =
				validateCall(moduleName, functionName, "call()/callOptional()");
			// The call was rejected before the hook ran, so there is no hook
			// failure to count.
			const bool rejected = !invalid.isOk();

			if (rejected) {
				report.error = std::move(invalid);
			} else {
				py::gil_scoped_acquire gil;
				CallOutcome outcome =
					invoke(std::string(moduleName), std::string(functionName),
						   arguments, optional);

				report.error = std::move(outcome.error);
			}

			report.duration =
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - started);

			if (report.error.isOk()) {
				noteSuccess(moduleName, functionName);
				return report;
			}

			// The host asked for Stop, and the runtime only advises: it never
			// leaves the loop or stops the interpreter on its own.
			report.stopRequested = policy == FailurePolicy::Stop;

			if (rejected) {
				// API misuse, not a failing hook: reported, never counted, so a
				// misspelled name cannot consume a hook's one report.
				return report;
			}

			const FailureVisit visit =
				noteFailure(moduleName, functionName, report.error);

			report.consecutiveFailures = visit.consecutiveFailures;

			if (policy == FailurePolicy::ReportOnce && visit.repeated) {
				report.suppressed = true;
				report.error	  = Error::ok();
			}
			return report;
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

		// A run always starts from zero, so a suppression never survives a
		// restart. shutdown() clears the same counters when the run ends.
		_impl->failureRecords.clear();

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
		_impl->running	   = true;
		_impl->ownerThread = std::this_thread::get_id();

		try {
			// Cache the modules now so that reporting a later failure never has
			// to import anything while handling an exception.
			_impl->tracebackModule	   = py::module_::import("traceback");
			_impl->builtinsModule	   = py::module_::import("builtins");
			_impl->importlibModule	   = py::module_::import("importlib");
			_impl->importlibUtilModule = py::module_::import("importlib.util");
		} catch (const std::exception &error) {
			_impl->stop();
			return Error(
				ErrorCode::InterpreterFailure,
				std::string("could not import the Python standard modules "
							"the runtime needs: ")
					+ error.what());
		}

		// initialize() is atomic: a failure leaves Python stopped rather than
		// handing the host a half-configured runtime.
		const std::vector<std::string> &paths = _impl->searchPaths;
		for (std::size_t index = 0; index < paths.size(); ++index) {
			Error error = _impl->insertSearchPath(paths[index], index);
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
		if (!_impl->ownsInterpreter()) {
			return threadAffinityError("shutdown()");
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
		if (_impl->running && !_impl->ownsInterpreter()) {
			return threadAffinityError("addSearchPath()");
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

		// The path becomes the last entry of the runtime's block, never the
		// first, so adding a path cannot reorder the ones already configured.
		const std::size_t index = paths.size();
		_impl->searchPaths.push_back(path);

		if (_impl->running) {
			py::gil_scoped_acquire gil;
			return _impl->insertSearchPath(path, index);
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
		if (!_impl->ownsInterpreter()) {
			return threadAffinityError("loadModule()");
		}
		if (moduleName.empty()) {
			return Error(ErrorCode::InvalidArgument, "module name is empty");
		}

		// A second load of a module the runtime already owns is a no-op that
		// keeps its generation; re-execution is reloadModule()'s job.
		if (_impl->modules.find(moduleName) != _impl->modules.end()) {
			return Error::ok();
		}

		py::gil_scoped_acquire gil;
		try {
			py::module_ module = py::module_::import(moduleName.c_str());
			Impl::LoadedModule loaded;

			loaded.module	  = module;
			loaded.sourcePath = capturedSourcePath(module);

			// Only a source-backed script module gets a generation; a
			// non-source module is loaded but neither reloadable nor
			// versioned.
			if (!loaded.sourcePath.empty()) {
				loaded.generation = 1;
				if (!setModuleGeneration(loaded.module, loaded.generation)) {
					return Error(
						ErrorCode::InterpreterFailure,
						"could not set __mais_generation__ on script module '"
							+ moduleName + "'");
				}
			}
			_impl->modules.emplace(moduleName, std::move(loaded));
			return Error::ok();
		} catch (py::error_already_set &error) {
			Error failure		   = _impl->fromPythonError(error);
			const std::string what = "could not import script module '"
				+ moduleName + "': " + failure.message();
			return withDiagnostics(failure, what);
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

	ReloadReport ScriptRuntime::reloadModule(const std::string &moduleName,
											 const ReloadOptions &options)
	{
		ReloadReport report;
		const auto started = std::chrono::steady_clock::now();

		auto elapsed = [&started]() {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - started);
		};
		auto since = [](std::chrono::steady_clock::time_point start) {
			return std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - start);
		};
		auto fail = [&report, &elapsed](ReloadPhase phase, Error error) {
			report.phase	= phase;
			report.error	= std::move(error);
			report.duration = elapsed();
			return report;
		};

		if (!_impl->running) {
			return fail(ReloadPhase::Validate,
						Error(ErrorCode::NotInitialized,
							  "reloadModule('" + moduleName
								  + "'): the scripting runtime is not "
									"running"));
		}

		// The report names the generation that stays live after a failure, so
		// it is read before the thread check. The map itself is only mutated
		// by the owning thread, and the host must not race that call.
		Impl::LoadedModule *loaded = nullptr;

		if (!moduleName.empty()) {
			auto module = _impl->modules.find(moduleName);

			if (module != _impl->modules.end()) {
				loaded = &module->second;
				if (!loaded->binding) {
					report.generation = loaded->generation;
				}
			}
		}

		if (!_impl->ownsInterpreter()) {
			return fail(ReloadPhase::Validate,
						threadAffinityError("reloadModule()"));
		}
		if (moduleName.empty()) {
			return fail(
				ReloadPhase::Validate,
				Error(ErrorCode::InvalidArgument, "module name is empty"));
		}
		if (loaded == nullptr) {
			return fail(ReloadPhase::Validate,
						Error(ErrorCode::ModuleNotFound,
							  "script module '" + moduleName
								  + "' is not loaded; call loadModule() "
									"first"));
		}
		if (loaded->binding) {
			return fail(ReloadPhase::Validate,
						Error(ErrorCode::InvalidArgument,
							  "host binding '" + moduleName
								  + "' cannot be reloaded: bindings are "
									"interpreter infrastructure, not script "
									"modules"));
		}
		if (loaded->sourcePath.empty()) {
			return fail(ReloadPhase::Validate,
						Error(ErrorCode::InvalidArgument,
							  "script module '" + moduleName
								  + "' has no source file and cannot be "
									"reloaded"));
		}

		const bool exportRequested = !options.exportFunction.empty();
		const bool importRequested = !options.importFunction.empty();

		if (exportRequested != importRequested) {
			return fail(ReloadPhase::Validate,
						Error(ErrorCode::InvalidArgument,
							  "state migration for '" + moduleName
								  + "' needs both exportFunction and "
									"importFunction, or neither"));
		}

		py::gil_scoped_acquire gil;
		ReloadPhase phase = ReloadPhase::Validate;

		py::dict sysModules;
		py::object previousEntry;
		bool hadPrevious = false;
		bool swapped	 = false;

		auto restoreSysModules = [&]() noexcept {
			if (!swapped) {
				return;
			}
			if (hadPrevious) {
				if (PyDict_SetItemString(sysModules.ptr(), moduleName.c_str(),
										 previousEntry.ptr())
					!= 0) {
					PyErr_Clear();
				}
			} else if (PyDict_DelItemString(sysModules.ptr(),
											moduleName.c_str())
					   != 0) {
				PyErr_Clear();
			}
			swapped = false;
		};

		try {
			py::module_ &previous			   = loaded->module;
			const std::string sourcePath	   = loaded->sourcePath;
			const std::uint64_t nextGeneration = loaded->generation + 1;

			// -- Export ------------------------------------------------------
			std::string stateText;

			if (exportRequested) {
				phase				  = ReloadPhase::Export;
				const auto phaseStart = std::chrono::steady_clock::now();
				py::object exporter	  = py::getattr(
					  previous, options.exportFunction.c_str(), py::none());

				if (!exporter || exporter.is_none()) {
					report.stateDuration += since(phaseStart);
					return fail(ReloadPhase::Export,
								Error(ErrorCode::FunctionNotFound,
									  "'" + moduleName + "' has no function '"
										  + options.exportFunction
										  + "' to export state"));
				}
				if (!PyCallable_Check(exporter.ptr())) {
					report.stateDuration += since(phaseStart);
					return fail(ReloadPhase::Export,
								Error(ErrorCode::TypeMismatch,
									  "'" + moduleName + "."
										  + options.exportFunction
										  + "' is not callable"));
				}

				py::object exported;

				try {
					exported = exporter();
				} catch (py::error_already_set &error) {
					report.stateDuration += since(phaseStart);
					Error failure = _impl->fromPythonError(error);

					return fail(
						ReloadPhase::Export,
						withDiagnostics(
							failure,
							"state export '" + moduleName + "."
								+ options.exportFunction
								+ "' failed; the previous generation is "
								  "still current: "
								+ failure.message()));
				}
				if (!PyUnicode_Check(exported.ptr())) {
					report.stateDuration += since(phaseStart);
					return fail(
						ReloadPhase::Export,
						Error(ErrorCode::TypeMismatch,
							  "'" + moduleName + "." + options.exportFunction
								  + "' returned " + pythonTypeName(exported)
								  + ", but str was expected"));
				}

				stateText			 = unicodeToUtf8(exported.ptr());
				report.stateExported = true;
				report.stateDuration += since(phaseStart);
			}

			// -- Compile -----------------------------------------------------
			phase					= ReloadPhase::Compile;
			const auto compileStart = std::chrono::steady_clock::now();
			std::error_code filesystemError;

			if (!std::filesystem::is_regular_file(sourcePath,
												  filesystemError)) {
				const std::string reason = filesystemError
					? ": " + filesystemError.message()
					: std::string();

				report.compileDuration += since(compileStart);
				return fail(ReloadPhase::Compile,
							Error(ErrorCode::ModuleNotFound,
								  "source file '" + sourcePath
									  + "' for script module '" + moduleName
									  + "' is missing or unreadable" + reason));
			}

			std::ifstream sourceFile(sourcePath, std::ios::binary);

			if (!sourceFile) {
				report.compileDuration += since(compileStart);
				return fail(ReloadPhase::Compile,
							Error(ErrorCode::ModuleNotFound,
								  "could not open source file '" + sourcePath
									  + "' for script module '" + moduleName
									  + "'"));
			}

			const std::string sourceText(
				(std::istreambuf_iterator<char>(sourceFile)),
				std::istreambuf_iterator<char>());

			if (sourceFile.bad()) {
				report.compileDuration += since(compileStart);
				return fail(ReloadPhase::Compile,
							Error(ErrorCode::ModuleNotFound,
								  "could not read source file '" + sourcePath
									  + "' for script module '" + moduleName
									  + "'"));
			}

			py::object code;

			try {
				code = _impl->builtinsModule.attr("compile")(
					py::bytes(sourceText), py::str(sourcePath), py::str("exec"),
					py::int_(0), py::bool_(true));
			} catch (py::error_already_set &error) {
				report.compileDuration += since(compileStart);
				Error failure = _impl->fromPythonError(error);

				return fail(ReloadPhase::Compile,
							withDiagnostics(failure,
											"could not compile '" + sourcePath
												+ "' for script module '"
												+ moduleName
												+ "': " + failure.message()));
			}
			report.compileDuration += since(compileStart);

			// -- Execute -----------------------------------------------------
			phase = ReloadPhase::Execute;

			py::object spec = py::getattr(previous, "__spec__", py::none());
			py::module_ newModule;

			if (spec && !spec.is_none()) {
				newModule =
					_impl->importlibUtilModule.attr("module_from_spec")(spec)
						.cast<py::module_>();
			}
			if (!newModule) {
				newModule = py::reinterpret_steal<py::module_>(
					PyModule_New(moduleName.c_str()));
			}
			if (!newModule) {
				throw py::error_already_set();
			}

			py::dict moduleNamespace = newModule.attr("__dict__");

			// A module executed by the import machinery already has builtins
			// in its namespace; a fresh module object does not.
			moduleNamespace["__builtins__"] = _impl->builtinsModule;
			py::setattr(newModule, "__file__", py::str(sourcePath));

			// An import performed by the new body must see the current
			// filesystem rather than a cache from the previous generation.
			_impl->importlibModule.attr("invalidate_caches")();

			sysModules = py::module_::import("sys").attr("modules");

			PyObject *entry =
				PyDict_GetItemString(sysModules.ptr(), moduleName.c_str());

			if (entry != nullptr) {
				previousEntry = py::reinterpret_borrow<py::object>(entry);
				hadPrevious	  = true;
			}
			// The new body imports under its own name, exactly as during a
			// normal import; the previous entry is restored on any failure.
			if (PyDict_SetItemString(sysModules.ptr(), moduleName.c_str(),
									 newModule.ptr())
				!= 0) {
				throw py::error_already_set();
			}
			swapped = true;

			const auto executeStart = std::chrono::steady_clock::now();

			try {
				// The source was compiled above with dont_inherit, so the
				// code object is executed as-is rather than recompiled from a
				// string.
				py::object result = py::reinterpret_steal<py::object>(
					PyEval_EvalCode(code.ptr(), moduleNamespace.ptr(),
									moduleNamespace.ptr()));

				if (!result) {
					throw py::error_already_set();
				}
			} catch (py::error_already_set &error) {
				report.executeDuration += since(executeStart);
				restoreSysModules();

				Error failure = _impl->fromPythonError(error);

				return fail(
					ReloadPhase::Execute,
					withDiagnostics(
						failure,
						"script module '" + moduleName
							+ "' raised while executing the new generation: "
							+ failure.message()));
			} catch (const std::exception &error) {
				report.executeDuration += since(executeStart);
				restoreSysModules();
				return fail(ReloadPhase::Execute,
							Error(ErrorCode::InvocationFailed,
								  "script module '" + moduleName
									  + "' could not execute the new "
										"generation: "
									  + error.what()));
			}
			report.executeDuration += since(executeStart);

			if (!setModuleGeneration(newModule, nextGeneration)) {
				restoreSysModules();
				return fail(ReloadPhase::Execute,
							Error(ErrorCode::InterpreterFailure,
								  "could not set __mais_generation__ on script "
								  "module '"
									  + moduleName + "'"));
			}

			// -- Restore -----------------------------------------------------
			if (importRequested) {
				phase				  = ReloadPhase::Restore;
				const auto phaseStart = std::chrono::steady_clock::now();
				py::object importer	  = py::getattr(
					  newModule, options.importFunction.c_str(), py::none());

				if (!importer || importer.is_none()) {
					report.stateDuration += since(phaseStart);
					restoreSysModules();
					return fail(ReloadPhase::Restore,
								Error(ErrorCode::FunctionNotFound,
									  "new generation of '" + moduleName
										  + "' has no function '"
										  + options.importFunction
										  + "' to import state"));
				}
				if (!PyCallable_Check(importer.ptr())) {
					report.stateDuration += since(phaseStart);
					restoreSysModules();
					return fail(ReloadPhase::Restore,
								Error(ErrorCode::TypeMismatch,
									  "'" + moduleName + "."
										  + options.importFunction
										  + "' is not callable"));
				}

				try {
					importer(py::str(stateText));
				} catch (py::error_already_set &error) {
					report.stateDuration += since(phaseStart);
					restoreSysModules();

					Error failure = _impl->fromPythonError(error);

					return fail(
						ReloadPhase::Restore,
						withDiagnostics(
							failure,
							"state import '" + moduleName + "."
								+ options.importFunction
								+ "' failed; the previous generation and its "
								  "state are still current: "
								+ failure.message()));
				} catch (const std::exception &error) {
					report.stateDuration += since(phaseStart);
					restoreSysModules();
					return fail(
						ReloadPhase::Restore,
						Error(ErrorCode::InvocationFailed,
							  "state import '" + moduleName + "."
								  + options.importFunction
								  + "' failed; the previous generation is "
									"still current: "
								  + error.what()));
				}
				report.stateRestored = true;
				report.stateDuration += since(phaseStart);
			}

			// -- Commit ------------------------------------------------------
			phase = ReloadPhase::Commit;

			// The only point after which the new generation is current. The
			// previous module reference is dropped here; if nothing else
			// holds it, its finalizers run now on the owning thread.
			loaded->module	   = newModule;
			loaded->generation = nextGeneration;

			report.phase	  = ReloadPhase::Commit;
			report.error	  = Error::ok();
			report.generation = nextGeneration;
			report.duration	  = elapsed();
			return report;
		} catch (py::error_already_set &error) {
			restoreSysModules();

			Error failure = _impl->fromPythonError(error);

			report.phase = phase;
			report.error =
				withDiagnostics(failure,
								"reloadModule('" + moduleName
									+ "') failed: " + failure.message());
		} catch (const std::exception &error) {
			restoreSysModules();

			report.phase = phase;
			report.error = Error(ErrorCode::InvocationFailed,
								 "reloadModule('" + moduleName
									 + "') failed: " + error.what());
		}

		report.duration = elapsed();
		return report;
	}

	Error ScriptRuntime::unloadModule(const std::string &moduleName)
	{
		if (!_impl->running) {
			return Error(ErrorCode::NotInitialized,
						 "the scripting runtime is not running");
		}
		if (!_impl->ownsInterpreter()) {
			return threadAffinityError("unloadModule()");
		}
		if (moduleName.empty()) {
			return Error(ErrorCode::InvalidArgument, "module name is empty");
		}

		auto module = _impl->modules.find(moduleName);

		if (module == _impl->modules.end()) {
			return Error(ErrorCode::ModuleNotFound,
						 "script module '" + moduleName
							 + "' is not loaded; call loadModule() first");
		}
		if (module->second.binding) {
			return Error(ErrorCode::InvalidArgument,
						 "host binding '" + moduleName
							 + "' cannot be unloaded: bindings are "
							   "interpreter infrastructure, not script "
							   "modules");
		}

		py::gil_scoped_acquire gil;
		try {
			py::dict sysModules = py::module_::import("sys").attr("modules");
			PyObject *entry =
				PyDict_GetItemString(sysModules.ptr(), moduleName.c_str());

			// Script code that replaced or deleted the entry is unsupported:
			// the runtime removes only the entry it still owns.
			if (entry != nullptr && entry == module->second.module.ptr()) {
				if (PyDict_DelItemString(sysModules.ptr(), moduleName.c_str())
					!= 0) {
					PyErr_Clear();
				}
			}
			_impl->modules.erase(module);
			return Error::ok();
		} catch (py::error_already_set &error) {
			return _impl->fromPythonError(error);
		} catch (const std::exception &error) {
			return Error(ErrorCode::InvocationFailed,
						 "could not unload script module '" + moduleName
							 + "': " + error.what());
		}
	}

	Result<std::uint64_t>
		ScriptRuntime::moduleGeneration(std::string_view moduleName) const
	{
		if (!_impl->running) {
			return Result<std::uint64_t>(
				Error(ErrorCode::NotInitialized,
					  "the scripting runtime is not running"));
		}
		if (moduleName.empty()) {
			return Result<std::uint64_t>(
				Error(ErrorCode::InvalidArgument, "module name is empty"));
		}

		auto module = _impl->modules.find(std::string(moduleName));

		if (module == _impl->modules.end()) {
			return Result<std::uint64_t>(Error(ErrorCode::ModuleNotFound,
											   "script module '"
												   + std::string(moduleName)
												   + "' is not loaded"));
		}
		if (module->second.binding) {
			return Result<std::uint64_t>(Error(ErrorCode::InvalidArgument,
											   "host binding '"
												   + std::string(moduleName)
												   + "' has no generation"));
		}
		if (module->second.sourcePath.empty()) {
			return Result<std::uint64_t>(
				Error(ErrorCode::InvalidArgument,
					  "script module '" + std::string(moduleName)
						  + "' has no source file and no generation"));
		}
		return Result<std::uint64_t>(module->second.generation);
	}

	Result<bool> ScriptRuntime::hasFunction(std::string_view moduleName,
											std::string_view functionName) const
	{
		Error invalid =
			_impl->validateCall(moduleName, functionName, "hasFunction()");

		if (invalid) {
			return Result<bool>(std::move(invalid));
		}

		py::gil_scoped_acquire gil;
		Error lookupFailure;
		py::module_ *module =
			_impl->findLoadedModule(moduleName, lookupFailure);

		if (module == nullptr) {
			return Result<bool>(std::move(lookupFailure));
		}

		PyObject *namespaceObject = PyModule_GetDict(module->ptr());
		if (namespaceObject == nullptr) {
			PyErr_Clear();
			return Result<bool>(Error(ErrorCode::InterpreterFailure,
									  "module '" + std::string(moduleName)
										  + "' has no namespace"));
		}

		switch (namespaceEntry(namespaceObject, functionName)) {
			case NamespaceEntry::Absent:
				return Result<bool>(false);
			case NamespaceEntry::NotCallable:
				return Result<bool>(Error(ErrorCode::TypeMismatch,
										  "'" + std::string(moduleName) + "."
											  + std::string(functionName)
											  + "' is not callable"));
			case NamespaceEntry::Callable:
			default:
				return Result<bool>(true);
		}
	}

	Result<std::vector<std::string>>
		ScriptRuntime::listFunctions(std::string_view moduleName) const
	{
		Error invalid = _impl->validateModule(moduleName, "listFunctions()");

		if (invalid) {
			return Result<std::vector<std::string>>(std::move(invalid));
		}

		py::gil_scoped_acquire gil;
		Error lookupFailure;
		py::module_ *module =
			_impl->findLoadedModule(moduleName, lookupFailure);

		if (module == nullptr) {
			return Result<std::vector<std::string>>(std::move(lookupFailure));
		}

		PyObject *namespaceObject = PyModule_GetDict(module->ptr());
		if (namespaceObject == nullptr) {
			PyErr_Clear();
			return Result<std::vector<std::string>>(Error(
				ErrorCode::InterpreterFailure,
				"module '" + std::string(moduleName) + "' has no namespace"));
		}

		std::vector<std::string> names;
		Py_ssize_t position = 0;

		PyObject *key	= nullptr;
		PyObject *value = nullptr;
		while (PyDict_Next(namespaceObject, &position, &key, &value)) {
			// Non-string keys can only come from direct dict manipulation;
			// they are not addressable as functions.
			if (!PyUnicode_Check(key) || !PyCallable_Check(value)) {
				continue;
			}

			Py_ssize_t size	 = 0;
			const char *text = PyUnicode_AsUTF8AndSize(key, &size);

			if (text == nullptr) {
				// A string that cannot be encoded as UTF-8 cannot be passed to
				// call() either; skip it rather than failing the whole list.
				PyErr_Clear();
				continue;
			}

			std::string name(text, static_cast<std::size_t>(size));

			if (name.empty() || name.front() == '_') {
				continue;
			}
			names.push_back(std::move(name));
		}

		// A console must render the same order on every run and platform.
		std::sort(names.begin(), names.end());
		return Result<std::vector<std::string>>(std::move(names));
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

	CallReport ScriptRuntime::call(FailurePolicy policy,
								   std::string_view moduleName,
								   std::string_view functionName)
	{
		return _impl->callWithPolicy(policy, moduleName, functionName, {},
									 false);
	}

	CallReport
		ScriptRuntime::call(FailurePolicy policy, std::string_view moduleName,
							std::string_view functionName,
							std::initializer_list<ScriptArgument> arguments)
	{
		return _impl->callWithPolicy(policy, moduleName, functionName,
									 std::vector<ScriptArgument>(arguments),
									 false);
	}

	CallReport ScriptRuntime::callOptional(FailurePolicy policy,
										   std::string_view moduleName,
										   std::string_view functionName)
	{
		return _impl->callWithPolicy(policy, moduleName, functionName, {},
									 true);
	}

	CallReport ScriptRuntime::callOptional(
		FailurePolicy policy, std::string_view moduleName,
		std::string_view functionName,
		std::initializer_list<ScriptArgument> arguments)
	{
		return _impl->callWithPolicy(policy, moduleName, functionName,
									 std::vector<ScriptArgument>(arguments),
									 true);
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
