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

#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "mais/Error.hpp"

namespace mais
{
	/// \brief Types a script call can hand back.
	///
	/// The set matches ScriptArgument exactly, so the same four scalar types
	/// cross the call boundary in both directions.
	template<typename T>
	concept ScriptReturnType =
		std::same_as<T, bool> || std::same_as<T, std::int64_t>
		|| std::same_as<T, double> || std::same_as<T, std::string>;

	/// \brief The outcome of a typed script call: a value, no value, or a
	/// failure.
	///
	/// The three states are:
	/// - hasValue(): a value of type T was read back; isOk() is true.
	/// - isOk() and !hasValue(): the call succeeded but returned no value,
	///   because the script returned None (or an optional hook is absent).
	/// - !isOk(): the call failed; read error().
	///
	/// ```cpp
	/// mais::Result<bool> quit =
	///     runtime.callOptional<bool>("game", "wants_quit");
	/// if (!quit.isOk()) {
	///     log(quit.error());
	/// } else if (quit.hasValue() && quit.value()) {
	///     ...
	/// }
	/// ```
	template<typename T> class [[nodiscard]] Result
	{
		public:
		/// A call that read a value back.
		Result(T value)
			: _value(std::move(value))
		{
		}

		/// A call that failed.
		Result(Error error)
			: _error(std::move(error))
		{
		}

		/// A call that succeeded without producing a value.
		[[nodiscard]] static Result<T> none()
		{
			return Result<T>(Error::ok());
		}

		/// True when the call itself succeeded; a None result is Ok.
		[[nodiscard]] bool isOk() const noexcept
		{
			return _error.isOk();
		}

		/// True when a value of type T was read.
		[[nodiscard]] bool hasValue() const noexcept
		{
			return _value.has_value();
		}

		/// \brief The value the script returned.
		/// \throws std::logic_error when no value was read; check
		/// hasValue() first.
		[[nodiscard]] const T &value() const
		{
			if (!_value.has_value()) {
				throw std::logic_error(
					"mais::Result<T>::value() called without a value; "
					"check hasValue() first");
			}
			return *_value;
		}

		/// The failure, or an Ok error for a value or None result.
		[[nodiscard]] const Error &error() const noexcept
		{
			return _error;
		}

		private:
		Error _error;
		std::optional<T> _value;
	};
}	 // namespace mais
