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

#include <string>
#include <string_view>

namespace mais
{
	/// \brief Category of a scripting failure.
	///
	/// The taxonomy is deliberately coarse. Hosts branch on whether the
	/// interpreter is still usable and read Error::message()/Error::traceback()
	/// for diagnostics.
	enum class ErrorCode {
		/// The operation succeeded.
		Ok = 0,

		/// The runtime was used before initialize() or after shutdown().
		NotInitialized,

		/// initialize() was called on a runtime that is already running.
		AlreadyInitialized,

		/// Python could not be started or stopped; scripting cannot continue.
		InterpreterFailure,

		/// A caller-supplied value is unusable: an empty name, a non-directory
		/// search path, a null binding callback.
		InvalidArgument,

		/// A configured search path does not exist.
		ScriptNotFound,

		/// A Python module could not be imported, or was never loaded.
		ModuleNotFound,

		/// The requested function is absent from the module.
		FunctionNotFound,

		/// A value could not be converted between C++ and Python.
		TypeMismatch,

		/// The script raised a Python exception; see Error::traceback().
		InvocationFailed,
	};

	/// Human-readable code name, for logs and test output.
	[[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

	/// True when `code` reports success.
	[[nodiscard]] constexpr bool isOk(ErrorCode code) noexcept
	{
		return code == ErrorCode::Ok;
	}

	/// \brief True when the interpreter itself failed, so scripting must stop.
	///
	/// Only InterpreterFailure qualifies: every other code describes a failure
	/// that leaves a usable interpreter behind.
	[[nodiscard]] constexpr bool isFatal(ErrorCode code) noexcept
	{
		return code == ErrorCode::InterpreterFailure;
	}

	/// True when the runtime can still be used after `code`.
	[[nodiscard]] constexpr bool isRecoverable(ErrorCode code) noexcept
	{
		return code != ErrorCode::Ok && !isFatal(code);
	}

	/// \brief A scripting failure, with Python traceback context when
	/// available.
	///
	/// A default-constructed Error is Ok, which makes `if (error)` read as
	/// "if something went wrong" (the same convention as std::error_code).
	class Error
	{
		public:
		/// Creates an Ok error.
		Error() noexcept = default;

		/// Creates an error with a message and no traceback.
		Error(ErrorCode code, std::string message);

		/// Creates an error with a message and captured Python traceback text.
		Error(ErrorCode code, std::string message, std::string traceback);

		/// An Ok error.
		[[nodiscard]] static Error ok() noexcept
		{
			return Error();
		}

		/// Category of the failure.
		[[nodiscard]] ErrorCode code() const noexcept
		{
			return _code;
		}

		/// True when the operation succeeded.
		[[nodiscard]] bool isOk() const noexcept
		{
			return _code == ErrorCode::Ok;
		}

		/// True when an error occurred; mirrors std::error_code semantics.
		explicit operator bool() const noexcept
		{
			return !isOk();
		}

		/// Description of the failure, without the traceback.
		[[nodiscard]] const std::string &message() const noexcept
		{
			return _message;
		}

		/// Python traceback captured when a script failed; empty when none.
		[[nodiscard]] const std::string &traceback() const noexcept
		{
			return _traceback;
		}

		/// True when a Python traceback was captured.
		[[nodiscard]] bool hasTraceback() const noexcept
		{
			return !_traceback.empty();
		}

		/// Formats the error as `[Code] message` followed by the traceback.
		[[nodiscard]] std::string toString() const;

		private:
		ErrorCode _code = ErrorCode::Ok;
		std::string _message;
		std::string _traceback;
	};
}	 // namespace mais
