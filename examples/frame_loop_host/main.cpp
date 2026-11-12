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

// A frame-loop host that models the shape a real XR/UI consumer uses.
//
// The host owns the settings facade, the frame context, the fixed-step loop,
// and the failure policy. Python only proposes settings and reacts to the
// steps it is handed: maïs supplies no engine, no renderer, no window, and no
// UI.
//
// Run it with a script directory and, optionally, a module name:
//   frame_loop_host <script-directory> [module]

#include "mais/ScriptRuntime.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace py = pybind11;

namespace
{
	/// Number of fixed steps the example runs before it stops on its own. A
	/// real host derives steps from wall-clock time instead.
	constexpr std::uint64_t plannedFrameCount = 120;

	/// The fixed step the host advances its simulation by.
	constexpr double fixedDeltaSeconds = 1.0 / 90.0;

	/// \brief The narrow settings surface a script may write to.
	///
	/// A real consumer's adapter exposes a deliberate subset of its engine
	/// configuration here instead of the whole engine API. The host validates
	/// the proposal before starting the engine, so an invalid value never
	/// reaches native code.
	struct SettingsFacade {
		std::string windowTitle = "mais frame loop host";
		int width				= 1280;
		int height				= 720;
	};

	/// \brief The context every hook receives, not a scalar.
	///
	/// The host rewrites the timing fields each fixed step and reads
	/// `scriptSteps` back for its summary, which is how the script reports
	/// what it did with the context it was handed.
	struct FrameContext {
		std::uint64_t index		  = 0;		// 0-based frame index.
		double deltaSeconds		  = 0.0;	// Fixed step for this frame.
		double elapsedSeconds	  = 0.0;	// Simulated time at the step.
		std::uint64_t scriptSteps = 0;		// Script-owned output.
	};

	/// \brief A stand-in for the engine a real consumer owns.
	///
	/// maïs must not depend on evan, so this example only records the settings
	/// it was started with. An adapter would hand the same validated values to
	/// the real engine here.
	class StubEngine
	{
		public:
		/// Starts the engine with settings the host has already validated.
		void start(const SettingsFacade &settings)
		{
			_settings = settings;
			_running  = true;
		}

		/// Advances the native side of one fixed step before the script runs.
		void advanceFrame() noexcept
		{
			if (_running) {
				++_frames;
			}
		}

		/// Stops the engine after the shutdown hook has completed.
		void stop() noexcept
		{
			_running = false;
		}

		/// Fixed steps the engine has advanced through.
		[[nodiscard]] std::uint64_t frameCount() const noexcept
		{
			return _frames;
		}

		/// The settings the engine was started with.
		[[nodiscard]] const SettingsFacade &settings() const noexcept
		{
			return _settings;
		}

		private:
		SettingsFacade _settings;
		std::uint64_t _frames = 0;
		bool _running		  = false;
	};

	/// \brief Problems that make a settings proposal unusable.
	///
	/// Empty means the host may apply the proposal. The messages name the
	/// field and the accepted range, so a console can show them as they are.
	[[nodiscard]] std::vector<std::string>
		validateSettings(const SettingsFacade &settings)
	{
		std::vector<std::string> problems;

		if (settings.windowTitle.empty()) {
			problems.emplace_back("window title must not be empty");
		} else if (settings.windowTitle.size() > 64) {
			problems.emplace_back("window title must be at most 64 characters");
		}

		if (settings.width < 64 || settings.width > 7680) {
			problems.emplace_back(
				"width must be between 64 and 7680 pixels (got "
				+ std::to_string(settings.width) + ')');
		}
		if (settings.height < 64 || settings.height > 4320) {
			problems.emplace_back(
				"height must be between 64 and 4320 pixels (got "
				+ std::to_string(settings.height) + ')');
		}
		return problems;
	}

	/// Joins validation messages into one line for the rejection report.
	[[nodiscard]] std::string
		joinProblems(const std::vector<std::string> &problems)
	{
		std::string joined;

		for (const std::string &problem: problems) {
			if (!joined.empty()) {
				joined.append("; ");
			}
			joined.append(problem);
		}
		return joined;
	}

	/// Hand a host object to a hook without transferring ownership. maïs
	/// borrows the address for the duration of the call, so the object must
	/// still outlive the runtime.
	mais::ScriptArgument::NativeConverter settingsConverter()
	{
		return +[](const void *value) -> void * {
			return py::cast(static_cast<const SettingsFacade *>(value),
							py::return_value_policy::reference)
				.release()
				.ptr();
		};
	}

	mais::ScriptArgument::NativeConverter contextConverter()
	{
		return +[](const void *value) -> void * {
			return py::cast(static_cast<const FrameContext *>(value),
							py::return_value_policy::reference)
				.release()
				.ptr();
		};
	}

	void reportError(std::string_view what, const mais::Error &error)
	{
		std::cerr << "host: " << what << ": " << error.toString() << '\n';
	}
}	 // namespace

int main(int argc, char **argv)
{
	const std::filesystem::path scriptDirectory = argc > 1
		? std::filesystem::path(argv[1])
		: std::filesystem::path("examples/frame_loop_host/scripts");
	const std::string moduleName				= argc > 2 ? argv[2] : "game";

	// These objects are handed to Python and must outlive the runtime, so they
	// are declared before it and destroyed after shutdown().
	StubEngine engine;
	const SettingsFacade hostDefaults;
	SettingsFacade proposal = hostDefaults;
	FrameContext context;

	mais::ScriptRuntime runtime;

	// Bindings are queued before Python starts. The classes must be registered
	// before a hook can receive an instance as an argument.
	mais::Error binding =
		runtime.registerModule("host", [](mais::PythonModule &module) {
			py::module_ &host = module.as<py::module_>();
			py::class_<SettingsFacade>(host, "SettingsFacade")
				.def_readwrite("window_title", &SettingsFacade::windowTitle)
				.def_readwrite("width", &SettingsFacade::width)
				.def_readwrite("height", &SettingsFacade::height);
			py::class_<FrameContext>(host, "FrameContext")
				.def_readonly("index", &FrameContext::index)
				.def_readonly("delta_seconds", &FrameContext::deltaSeconds)
				.def_readonly("elapsed_seconds", &FrameContext::elapsedSeconds)
				.def_readwrite("script_steps", &FrameContext::scriptSteps);
		});
	if (binding) {
		reportError("registerModule", binding);
		return 1;
	}

	if (mais::Error error = runtime.initialize(); error) {
		reportError("initialize", error);
		return 1;
	}

	// From here on the runtime owns Python until shutdown() returns.
	if (mais::Error error = runtime.addSearchPath(scriptDirectory.string());
		error) {
		reportError("addSearchPath", error);
		runtime.shutdown();
		return 1;
	}
	if (mais::Error error = runtime.loadModule(moduleName); error) {
		reportError("loadModule", error);
		runtime.shutdown();
		return 1;
	}

	// -- configure: the script proposes, the host validates -----------------
	//
	// Stop is the right policy here: a hook that raises may have written only
	// part of the facade, so the host must not start an engine with it.
	mais::CallReport configured = runtime.callOptional(
		mais::FailurePolicy::Stop, moduleName, "configure",
		{ mais::ScriptArgument::native(&proposal, settingsConverter()) });
	if (configured) {
		reportError("configure", configured.error);
	}
	if (configured.stopRequested) {
		std::cerr << "host: not starting the engine after a failed "
					 "configure\n";
		runtime.shutdown();
		return 1;
	}

	// Validation is host policy, not a script concern. A rejected proposal is
	// not applied: the engine starts with the host's previous settings.
	const std::vector<std::string> problems = validateSettings(proposal);
	if (!problems.empty()) {
		std::cerr << "host: rejected settings proposal: "
				  << joinProblems(problems) << '\n';
		proposal = hostDefaults;
		std::cerr << "host: starting with the previous settings: "
				  << proposal.width << 'x' << proposal.height << " \""
				  << proposal.windowTitle << "\"\n";
	} else {
		std::cout << "host: accepted settings proposal\n";
	}

	engine.start(proposal);
	std::cout << "host: engine started " << engine.settings().width << 'x'
			  << engine.settings().height << " \""
			  << engine.settings().windowTitle << "\"\n";

	// -- on_start: Stop -----------------------------------------------------
	//
	// A session whose start hook failed should not enter its loop.
	context.deltaSeconds	 = fixedDeltaSeconds;
	mais::CallReport started = runtime.callOptional(
		mais::FailurePolicy::Stop, moduleName, "on_start",
		{ mais::ScriptArgument::native(&context, contextConverter()) });
	if (started) {
		reportError("on_start", started.error);
	}
	if (started.stopRequested) {
		std::cerr << "host: leaving the loop before the first frame\n";
		engine.stop();
		runtime.shutdown();
		return 1;
	}

	// -- fixed-step loop ----------------------------------------------------
	//
	// on_update gets ReportOnce: the first failure of a streak is reported
	// with its traceback, identical repeats are suppressed, and the host owns
	// the loop, so a bad hook cannot abort it.
	std::uint64_t executedFrames		 = 0;
	std::uint64_t reportedUpdateFailures = 0;
	std::uint64_t suppressedFailures	 = 0;
	std::uint64_t updateDurationNs		 = 0;

	for (std::uint64_t index = 0; index < plannedFrameCount; ++index) {
		context.index		   = index;
		context.deltaSeconds   = fixedDeltaSeconds;
		context.elapsedSeconds = static_cast<double>(index) * fixedDeltaSeconds;

		// The host advances its engine before giving the step to the script.
		engine.advanceFrame();

		mais::CallReport report = runtime.callOptional(
			mais::FailurePolicy::ReportOnce, moduleName, "on_update",
			{ mais::ScriptArgument::native(&context, contextConverter()) });

		++executedFrames;
		updateDurationNs += static_cast<std::uint64_t>(report.duration.count());

		if (report) {
			++reportedUpdateFailures;
			reportError("on_update", report.error);
		} else if (report.suppressed) {
			++suppressedFailures;
		}

		if (report.stopRequested) {
			// ReportOnce never asks to stop; a hook that chose Stop would
			// leave the loop here instead.
			break;
		}
	}

	// The shutdown hook sees the end-of-session context, not a fresh object.
	context.index = executedFrames;
	context.elapsedSeconds =
		static_cast<double>(executedFrames) * fixedDeltaSeconds;

	// Shutdown must always be attempted, so it uses Continue: a failing
	// on_shutdown is reported but cannot prevent the host from stopping.
	mais::CallReport stopped = runtime.callOptional(
		mais::FailurePolicy::Continue, moduleName, "on_shutdown",
		{ mais::ScriptArgument::native(&context, contextConverter()) });
	if (stopped) {
		reportError("on_shutdown", stopped.error);
	}

	engine.stop();

	if (mais::Error error = runtime.shutdown(); error) {
		reportError("shutdown", error);
		return 1;
	}

	// -- shutdown summary ---------------------------------------------------
	const double simulatedSeconds =
		static_cast<double>(executedFrames) * fixedDeltaSeconds;
	const double meanUpdateNs = executedFrames > 0
		? static_cast<double>(updateDurationNs)
			/ static_cast<double>(executedFrames)
		: 0.0;

	std::cout << "host: summary frames=" << executedFrames << '/'
			  << plannedFrameCount << " engine_frames=" << engine.frameCount()
			  << " simulated=" << std::fixed << std::setprecision(3)
			  << simulatedSeconds << "s script_steps=" << context.scriptSteps
			  << " update_reports=" << reportedUpdateFailures
			  << " suppressed=" << suppressedFailures
			  << " mean_update_ns=" << std::setprecision(0) << meanUpdateNs
			  << " settings=" << engine.settings().width << 'x'
			  << engine.settings().height << " \""
			  << engine.settings().windowTitle << "\"\n";
	return 0;
}
