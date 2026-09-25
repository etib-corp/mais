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

#include "mais/ScriptRuntime.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <gtest/gtest.h>

#include <string>
#include <utility>

namespace mais_test
{
	/// A host-owned service the fake-host tests inject into Python.
	class FakeCounter
	{
		public:
		explicit FakeCounter(std::string name)
			: _name(std::move(name))
		{
		}

		int increment()
		{
			return ++_count;
		}

		[[nodiscard]] int count() const noexcept
		{
			return _count;
		}

		[[nodiscard]] const std::string &name() const noexcept
		{
			return _name;
		}

		void reset() noexcept
		{
			_count = 0;
		}

		private:
		std::string _name;
		int _count = 0;
	};

	/// Registers the embedded `host` module and exposes the counter as
	/// `host.counter`, the way an application adapter would.
	inline void bindFakeHost(mais::ScriptRuntime &runtime, FakeCounter &counter)
	{
		mais::Error error = runtime.registerModule(
			"host", [&counter](mais::PythonModule &module) {
				pybind11::module_ &host = module.as<pybind11::module_>();
				pybind11::class_<FakeCounter>(host, "FakeCounter")
					.def("increment", &FakeCounter::increment)
					.def("reset", &FakeCounter::reset)
					.def_property_readonly("count", &FakeCounter::count);
				// maïs never owns host objects: Python gets a reference that
				// must not outlive the counter.
				host.attr("counter") = pybind11::cast(
					&counter, pybind11::return_value_policy::reference);
			});
		ASSERT_TRUE(error.isOk()) << error.toString();
	}
}	 // namespace mais_test
