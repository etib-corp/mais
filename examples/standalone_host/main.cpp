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

// A minimal host that embeds Python through maïs.
//
// It has no dependency on guillaume or evan: the host owns the native services,
// the frame loop, and the script hooks it chooses to call. Python only proposes
// settings and reacts to the points in the loop where the host runs it.

#include "mais/ScriptRuntime.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

namespace py = pybind11;

namespace
{
	/// Settings a script may propose; the host validates them before use.
	struct HostSettings {
		std::string windowTitle = "untitled";
		int width				= 640;
		int height				= 480;
	};

	/// A host-owned service the script is allowed to drive.
	struct HostMetrics {
		std::uint64_t frames  = 0;
		double elapsedSeconds = 0.0;
	};

	/// Hand a host object to a hook without transferring ownership. maïs
	/// borrows the address for the duration of the call, so the object keeps
	/// outliving the runtime.
	mais::ScriptArgument::NativeConverter settingsConverter()
	{
		return +[](const void *value) -> void * {
			return py::cast(static_cast<const HostSettings *>(value),
							py::return_value_policy::reference)
				.release()
				.ptr();
		};
	}

	mais::ScriptArgument::NativeConverter metricsConverter()
	{
		return +[](const void *value) -> void * {
			return py::cast(static_cast<const HostMetrics *>(value),
							py::return_value_policy::reference)
				.release()
				.ptr();
		};
	}

	[[nodiscard]] bool isUsable(const HostSettings &settings)
	{
		return !settings.windowTitle.empty() && settings.width > 0
			&& settings.height > 0;
	}

	void reportError(const mais::Error &error)
	{
		std::cerr << error.toString() << '\n';
	}
}	 // namespace

int main(int argc, char **argv)
{
	const std::filesystem::path scriptDirectory = argc > 1
		? std::filesystem::path(argv[1])
		: std::filesystem::path("examples/standalone_host/scripts");

	HostSettings settings;
	HostMetrics metrics;

	mais::ScriptRuntime runtime;

	// Bindings are queued before Python starts.
	mais::Error binding =
		runtime.registerModule("host", [](mais::PythonModule &module) {
			py::module_ &host = module.as<py::module_>();
			// The classes must be registered before a host object can be passed
			// as an argument; the objects themselves are handed to each hook.
			py::class_<HostSettings>(host, "HostSettings")
				.def_readwrite("window_title", &HostSettings::windowTitle)
				.def_readwrite("width", &HostSettings::width)
				.def_readwrite("height", &HostSettings::height);
			py::class_<HostMetrics>(host, "HostMetrics")
				.def_readwrite("frames", &HostMetrics::frames)
				.def_readwrite("elapsed_seconds", &HostMetrics::elapsedSeconds);
		});
	if (binding) {
		reportError(binding);
		return 1;
	}

	if (mais::Error error = runtime.initialize(); error) {
		reportError(error);
		return 1;
	}
	if (mais::Error error = runtime.addSearchPath(scriptDirectory.string());
		error) {
		reportError(error);
		return 1;
	}
	if (mais::Error error = runtime.loadModule("game"); error) {
		reportError(error);
		runtime.shutdown();
		return 1;
	}

	// Hooks are optional: a script may implement none of them. The settings
	// object is handed in, not imported from the bindings.
	if (mais::Error error = runtime.callOptional(
			"game", "configure",
			{ mais::ScriptArgument::native(&settings, settingsConverter()) });
		error) {
		reportError(error);
	}

	// The host validates whatever the script proposed before using it.
	if (!isUsable(settings)) {
		std::cerr << "the script proposed unusable settings\n";
		runtime.shutdown();
		return 1;
	}
	std::cout << "host: configured title=\"" << settings.windowTitle << "\" "
			  << settings.width << "x" << settings.height << '\n';

	if (mais::Error error = runtime.callOptional("game", "on_start"); error) {
		reportError(error);
	}

	// The host owns the loop; scripts run only where the host decides.
	constexpr int frameCount	  = 3;
	constexpr double deltaSeconds = 1.0 / 60.0;
	for (int frame = 0; frame < frameCount; ++frame) {
		metrics.elapsedSeconds += deltaSeconds;
		if (mais::Error error = runtime.callOptional(
				"game", "on_update",
				{ mais::ScriptArgument::native(&metrics, metricsConverter()),
				  mais::ScriptArgument::number(deltaSeconds) });
			error) {
			reportError(error);
		}
	}

	if (mais::Error error = runtime.callOptional(
			"game", "on_shutdown",
			{ mais::ScriptArgument::native(&metrics, metricsConverter()) });
		error) {
		reportError(error);
	}

	std::cout << "host: observed frames=" << metrics.frames
			  << " elapsed=" << metrics.elapsedSeconds << "s\n";

	if (mais::Error error = runtime.shutdown(); error) {
		reportError(error);
		return 1;
	}
	return 0;
}
