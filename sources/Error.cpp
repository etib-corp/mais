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

#include "mais/Error.hpp"

#include <utility>

namespace mais
{
	std::string_view errorCodeName(ErrorCode code) noexcept
	{
		switch (code) {
			case ErrorCode::Ok:
				return "Ok";
			case ErrorCode::NotInitialized:
				return "NotInitialized";
			case ErrorCode::AlreadyInitialized:
				return "AlreadyInitialized";
			case ErrorCode::InterpreterFailure:
				return "InterpreterFailure";
			case ErrorCode::InvalidArgument:
				return "InvalidArgument";
			case ErrorCode::ScriptNotFound:
				return "ScriptNotFound";
			case ErrorCode::ModuleNotFound:
				return "ModuleNotFound";
			case ErrorCode::FunctionNotFound:
				return "FunctionNotFound";
			case ErrorCode::TypeMismatch:
				return "TypeMismatch";
			case ErrorCode::InvocationFailed:
				return "InvocationFailed";
		}
		return "Unknown";
	}

	Error::Error(ErrorCode code, std::string message)
		: _code(code)
		, _message(std::move(message))
	{
	}

	Error::Error(ErrorCode code, std::string message, std::string traceback)
		: _code(code)
		, _message(std::move(message))
		, _traceback(std::move(traceback))
	{
	}

	std::string Error::toString() const
	{
		if (isOk()) {
			return "[Ok]";
		}

		std::string text = "[";
		text += errorCodeName(_code);
		text += "] ";
		text += _message;
		if (!_traceback.empty()) {
			text += '\n';
			text += _traceback;
		}
		return text;
	}
}	 // namespace mais
