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

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>

namespace py = pybind11;

namespace
{
	/// A host-owned service the benchmark script calls back into.
	struct HostCounter {
		std::uint64_t value = 0;
	};

	mais::ScriptRuntime *gRuntime = nullptr;

	/// Registers `host` and starts the interpreter once for every benchmark.
	mais::Error setUp(mais::ScriptRuntime &runtime, HostCounter &counter)
	{
		mais::Error binding = runtime.registerModule(
			"host", [&counter](mais::PythonModule &module) {
				py::module_ &host = module.as<py::module_>();
				py::class_<HostCounter>(host, "HostCounter")
					.def_readwrite("value", &HostCounter::value);
				host.attr("counter") =
					py::cast(&counter, py::return_value_policy::reference);
			});
		if (binding) {
			return binding;
		}

		if (mais::Error error = runtime.initialize(); error) {
			return error;
		}
		if (mais::Error error =
				runtime.addSearchPath(MAIS_BENCHMARK_SCRIPT_DIR);
			error) {
			return error;
		}
		return runtime.loadModule("bench");
	}
}	 // namespace

static void BM_CallWithoutArguments(benchmark::State &state)
{
	for (auto _: state) {
		mais::Error error = gRuntime->call("bench", "noop");
		benchmark::DoNotOptimize(error);
	}
}
BENCHMARK(BM_CallWithoutArguments);

static void BM_CallWithANumberArgument(benchmark::State &state)
{
	for (auto _: state) {
		mais::Error error = gRuntime->call(
			"bench", "with_delta", { mais::ScriptArgument::number(0.016) });
		benchmark::DoNotOptimize(error);
	}
}
BENCHMARK(BM_CallWithANumberArgument);

static void BM_CallOptionalMissingFunction(benchmark::State &state)
{
	for (auto _: state) {
		mais::Error error = gRuntime->callOptional("bench", "on_resize");
		benchmark::DoNotOptimize(error);
	}
}
BENCHMARK(BM_CallOptionalMissingFunction);

/// Measures a full round trip: C++ calls Python, which calls back into C++.
static void BM_CallThatInvokesAHostBinding(benchmark::State &state)
{
	for (auto _: state) {
		mais::Error error = gRuntime->call("bench", "tick");
		benchmark::DoNotOptimize(error);
	}
}
BENCHMARK(BM_CallThatInvokesAHostBinding);

/// \brief Reports the duration the runtime measured for a successful call.
///
/// The counter is the number a console would display, so a regression in it is
/// visible next to Google Benchmark's own wall-clock time instead of being
/// assumed from the call count.
static void BM_CallWithReportedTiming(benchmark::State &state)
{
	std::chrono::nanoseconds total { 0 };

	for (auto _: state) {
		mais::CallReport report =
			gRuntime->call(mais::FailurePolicy::Continue, "bench", "noop");
		benchmark::DoNotOptimize(report);
		total += report.duration;
	}

	const double iterations =
		state.iterations() > 0 ? static_cast<double>(state.iterations()) : 1.0;

	state.counters["runtime_ns_per_call"] =
		benchmark::Counter(static_cast<double>(total.count()) / iterations);
}
BENCHMARK(BM_CallWithReportedTiming);

/// \brief Reports the duration of a call that raises, including the traceback
/// the host receives.
static void BM_FailingCallWithReportedTiming(benchmark::State &state)
{
	std::chrono::nanoseconds total { 0 };

	for (auto _: state) {
		mais::CallReport report =
			gRuntime->call(mais::FailurePolicy::Continue, "bench", "explode");
		benchmark::DoNotOptimize(report);
		total += report.duration;
	}

	const double iterations =
		state.iterations() > 0 ? static_cast<double>(state.iterations()) : 1.0;

	state.counters["runtime_ns_per_call"] =
		benchmark::Counter(static_cast<double>(total.count()) / iterations);
}
BENCHMARK(BM_FailingCallWithReportedTiming);

int main(int argc, char **argv)
{
	benchmark::Initialize(&argc, argv);
	if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
		return 1;
	}

	HostCounter counter;
	mais::ScriptRuntime runtime;
	if (mais::Error error = setUp(runtime, counter); error) {
		std::cerr << error.toString() << '\n';
		return 1;
	}

	gRuntime = &runtime;
	benchmark::RunSpecifiedBenchmarks();
	gRuntime = nullptr;

	if (mais::Error error = runtime.shutdown(); error) {
		std::cerr << error.toString() << '\n';
		return 1;
	}
	return 0;
}
