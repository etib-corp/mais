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

#include <cstdint>
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

	/// A configuration facade a script may read and edit; the host validates
	/// it after the hook returns. Registered by bindFakeFacade() so it can be
	/// passed to a hook as a native argument.
	struct FakeSettings {
		std::string title = "untitled";
		int width		  = 640;
		int height		  = 480;
	};

	/// Per-frame state a script may read and advance.
	struct FakeContext {
		std::uint64_t frame = 0;
		double elapsed		= 0.0;
	};

	/// Records what a script read from a facade it was handed, so a test can
	/// assert the script saw the host's own object rather than a copy.
	struct FakeObserver {
		const FakeSettings *lastSettings = nullptr;
		int observedWidth				 = 0;
		std::uint64_t observedFrame		 = 0;
	};

	/// Hands a FakeSettings to a hook without transferring ownership.
	inline mais::ScriptArgument::NativeConverter settingsConverter()
	{
		return +[](const void *value) -> void * {
			return pybind11::cast(static_cast<const FakeSettings *>(value),
								  pybind11::return_value_policy::reference)
				.release()
				.ptr();
		};
	}

	/// Hands a FakeContext to a hook without transferring ownership.
	inline mais::ScriptArgument::NativeConverter contextConverter()
	{
		return +[](const void *value) -> void * {
			return pybind11::cast(static_cast<const FakeContext *>(value),
								  pybind11::return_value_policy::reference)
				.release()
				.ptr();
		};
	}

	/// Registers a `facade` module exposing the types a hook can receive, and
	/// the observer the fixture scripts report back through.
	inline void bindFakeFacade(mais::ScriptRuntime &runtime,
							   FakeObserver &observer)
	{
		mais::Error error = runtime.registerModule(
			"facade", [&observer](mais::PythonModule &module) {
				pybind11::module_ &facade = module.as<pybind11::module_>();
				pybind11::class_<FakeSettings>(facade, "FakeSettings")
					.def_readwrite("title", &FakeSettings::title)
					.def_readwrite("width", &FakeSettings::width)
					.def_readwrite("height", &FakeSettings::height);
				pybind11::class_<FakeContext>(facade, "FakeContext")
					.def_readwrite("frame", &FakeContext::frame)
					.def_readwrite("elapsed", &FakeContext::elapsed);
				pybind11::class_<FakeObserver>(facade, "FakeObserver")
					.def("record_settings",
						 [&observer](FakeObserver &,
									 const FakeSettings &settings) {
							 observer.lastSettings	= &settings;
							 observer.observedWidth = settings.width;
						 })
					.def("record_context",
						 [&observer](FakeObserver &,
									 const FakeContext &context) {
							 observer.observedFrame = context.frame;
						 });
				// The observer outlives the runtime, so Python only borrows it.
				facade.attr("observer") = pybind11::cast(
					&observer, pybind11::return_value_policy::reference);
			});
		ASSERT_TRUE(error.isOk()) << error.toString();
	}
}	 // namespace mais_test
