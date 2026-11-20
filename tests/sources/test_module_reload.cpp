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

#include "FakeHost.hpp"

#include "mais/ScriptRuntime.hpp"

#include <pybind11/embed.h>
#include <pybind11/pybind11.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace
{
	/// \brief A directory of scripts created for one test.
	///
	/// Reload tests mutate their source, so they never touch the checked-in
	/// fixture scripts under MAIS_TEST_SCRIPT_DIR.
	class TempScriptDirectory
	{
		public:
		TempScriptDirectory()
		{
			static std::atomic<std::uint64_t> sequence { 0 };
			const auto stamp =
				std::chrono::steady_clock::now().time_since_epoch().count();

			_path = std::filesystem::temp_directory_path()
				/ ("mais_reload_" + std::to_string(stamp) + "_"
				   + std::to_string(sequence.fetch_add(1)));
			std::filesystem::create_directories(_path);
		}

		~TempScriptDirectory()
		{
			std::error_code error;

			std::filesystem::remove_all(_path, error);
		}

		TempScriptDirectory(const TempScriptDirectory &)			= delete;
		TempScriptDirectory &operator=(const TempScriptDirectory &) = delete;

		[[nodiscard]] const std::filesystem::path &path() const noexcept
		{
			return _path;
		}

		[[nodiscard]] std::string pathOf(const std::string &relative) const
		{
			return (_path / relative).string();
		}

		void write(const std::string &relative, const std::string &text) const
		{
			const std::filesystem::path target = _path / relative;

			if (target.has_parent_path()) {
				std::filesystem::create_directories(target.parent_path());
			}

			std::ofstream file(target, std::ios::binary | std::ios::trunc);

			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			file.close();
		}

		void remove(const std::string &relative) const
		{
			std::error_code error;

			std::filesystem::remove(_path / relative, error);
		}

		private:
		std::filesystem::path _path;
	};

	/// The `sys.modules` dictionary of the running interpreter.
	py::dict sysModules()
	{
		return py::module_::import("sys").attr("modules");
	}

	/// The module object Python currently resolves for `name`.
	py::object moduleObject(const std::string &name)
	{
		py::dict modules  = sysModules();
		py::object module = modules[py::str(name)];

		return module;
	}

	bool hasModuleObject(const std::string &name)
	{
		py::dict modules = sysModules();
		py::object key	 = py::str(name);

		return PyDict_Contains(modules.ptr(), key.ptr()) == 1;
	}

	/// A weak reference, so a test can prove the target was released.
	py::object weakReference(const py::object &target)
	{
		return py::module_::import("weakref").attr("ref")(target);
	}

	void collectGarbage()
	{
		py::module_::import("gc").attr("collect")();
	}

	void invalidateImportCaches()
	{
		py::module_::import("importlib").attr("invalidate_caches")();
	}

	/// True when `path` ends with `suffix`.
	bool endsWith(const std::string &path, const std::string &suffix)
	{
		return path.size() >= suffix.size()
			&& path.compare(path.size() - suffix.size(), suffix.size(), suffix)
			== 0;
	}
}	 // namespace

/// A reload re-executes the module body: the new constant and function body
/// are visible and the generation advances.
TEST(ModuleReloadTest, ReplacesCodeAndIncrementsGeneration)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_target.py", R"PY(
value = 1


def read():
    return value


def generation():
    return __mais_generation__
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_target").isOk());

	mais::Result<std::int64_t> initial =
		runtime.call<std::int64_t>("reload_target", "read");
	ASSERT_TRUE(initial.isOk());
	EXPECT_EQ(initial.value(), 1);

	mais::Result<std::uint64_t> generation =
		runtime.moduleGeneration("reload_target");
	ASSERT_TRUE(generation.isOk());
	EXPECT_EQ(generation.value(), 1U);

	py::object previous		= moduleObject("reload_target");
	py::object previousRead = previous.attr("read");

	directory.write("reload_target.py", R"PY(
value = 2


def read():
    return value


def generation():
    return __mais_generation__
)PY");

	mais::ReloadReport report = runtime.reloadModule("reload_target");
	ASSERT_TRUE(report.isOk()) << report.error.toString();
	EXPECT_EQ(report.phase, mais::ReloadPhase::Commit);
	EXPECT_EQ(report.generation, 2U);
	EXPECT_FALSE(report.stateExported);
	EXPECT_FALSE(report.stateRestored);
	EXPECT_FALSE(report.error.hasTraceback());

	EXPECT_EQ(runtime.moduleGeneration("reload_target").value(), 2U);

	mais::Result<std::int64_t> reloaded =
		runtime.call<std::int64_t>("reload_target", "read");
	ASSERT_TRUE(reloaded.isOk());
	EXPECT_EQ(reloaded.value(), 2);

	mais::Result<std::int64_t> generationHook =
		runtime.call<std::int64_t>("reload_target", "generation");
	ASSERT_TRUE(generationHook.isOk());
	EXPECT_EQ(generationHook.value(), 2);

	// The runtime's view and sys.modules agree on a new module object.
	py::object current = moduleObject("reload_target");
	EXPECT_NE(previous.ptr(), current.ptr());
	EXPECT_EQ(current.attr("__mais_generation__").cast<std::int64_t>(), 2);

	// An old reference is stale, not broken: it still runs the old code.
	EXPECT_EQ(previousRead().cast<std::int64_t>(), 1);

	// Python objects must be released while the interpreter is alive.
	previousRead = py::object();
	current		 = py::object();
	previous	 = py::object();

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Module-level state that is not migrated is initialized from the new body.
TEST(ModuleReloadTest, ResetsStateWithoutMigration)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_state.py", R"PY(
counter = 0


def advance():
    global counter
    counter += 1
    return counter
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_state").isOk());

	ASSERT_TRUE(runtime.call("reload_state", "advance").isOk());
	mais::Result<std::int64_t> second =
		runtime.call<std::int64_t>("reload_state", "advance");
	ASSERT_TRUE(second.isOk());
	EXPECT_EQ(second.value(), 2);

	directory.write("reload_state.py", R"PY(
marker = "second"

counter = 0


def advance():
    global counter
    counter += 1
    return counter
)PY");

	mais::ReloadReport report = runtime.reloadModule("reload_state");
	ASSERT_TRUE(report.isOk()) << report.error.toString();
	EXPECT_FALSE(report.stateExported);
	EXPECT_FALSE(report.stateRestored);

	mais::Result<std::int64_t> reset =
		runtime.call<std::int64_t>("reload_state", "advance");
	ASSERT_TRUE(reset.isOk());
	EXPECT_EQ(reset.value(), 1);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// A syntax error stops in Compile, leaves the previous generation current, and
/// keeps isModuleLoaded() honest.
TEST(ModuleReloadTest, RollsBackOnSyntaxError)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_syntax.py", R"PY(
value = 1


def read():
    return value
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_syntax").isOk());

	directory.write("reload_syntax.py", R"PY(
def broken(:
    pass
)PY");

	mais::ReloadReport report = runtime.reloadModule("reload_syntax");
	EXPECT_FALSE(report.isOk());
	EXPECT_EQ(report.phase, mais::ReloadPhase::Compile);
	EXPECT_EQ(report.error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_EQ(report.error.exceptionType(), "SyntaxError");
	EXPECT_TRUE(report.error.frames().empty());
	EXPECT_NE(report.error.traceback().find("SyntaxError"), std::string::npos);
	EXPECT_EQ(report.generation, 1U);

	EXPECT_TRUE(runtime.isModuleLoaded("reload_syntax"));
	EXPECT_EQ(runtime.moduleGeneration("reload_syntax").value(), 1U);

	mais::Result<std::int64_t> read =
		runtime.call<std::int64_t>("reload_syntax", "read");
	ASSERT_TRUE(read.isOk()) << read.error().toString();
	EXPECT_EQ(read.value(), 1);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// An exception in the new module body stops in Execute, restores sys.modules
/// and leaves the old hook callable with a traceback from the new body.
TEST(ModuleReloadTest, RollsBackOnBodyException)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_raise.py", R"PY(
value = 1


def read():
    return value
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_raise").isOk());

	py::object previous = moduleObject("reload_raise");

	directory.write("reload_raise.py", R"PY(
value = 2


def read():
    return value


raise RuntimeError("the new body failed")
)PY");

	mais::ReloadReport report = runtime.reloadModule("reload_raise");
	EXPECT_FALSE(report.isOk());
	EXPECT_EQ(report.phase, mais::ReloadPhase::Execute);
	EXPECT_EQ(report.error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_EQ(report.error.exceptionType(), "RuntimeError");
	EXPECT_TRUE(report.error.hasTraceback());
	EXPECT_NE(report.error.message().find("the new body failed"),
			  std::string::npos);
	ASSERT_FALSE(report.error.frames().empty());
	EXPECT_TRUE(endsWith(report.error.frames().back().file, "reload_raise.py"));
	EXPECT_EQ(report.generation, 1U);

	EXPECT_TRUE(runtime.isModuleLoaded("reload_raise"));
	EXPECT_EQ(runtime.moduleGeneration("reload_raise").value(), 1U);
	EXPECT_EQ(moduleObject("reload_raise").ptr(), previous.ptr());

	mais::Result<std::int64_t> read =
		runtime.call<std::int64_t>("reload_raise", "read");
	ASSERT_TRUE(read.isOk()) << read.error().toString();
	EXPECT_EQ(read.value(), 1);

	previous = py::object();

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// A deleted source is a Compile failure; the old generation is still current.
TEST(ModuleReloadTest, RollsBackWhenSourceIsMissing)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_missing.py", R"PY(
value = 1


def read():
    return value
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_missing").isOk());

	directory.remove("reload_missing.py");

	mais::ReloadReport report = runtime.reloadModule("reload_missing");
	EXPECT_FALSE(report.isOk());
	EXPECT_EQ(report.phase, mais::ReloadPhase::Compile);
	EXPECT_EQ(report.error.code(), mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(report.generation, 1U);

	EXPECT_TRUE(runtime.isModuleLoaded("reload_missing"));
	EXPECT_EQ(runtime.moduleGeneration("reload_missing").value(), 1U);

	mais::Result<std::int64_t> read =
		runtime.call<std::int64_t>("reload_missing", "read");
	ASSERT_TRUE(read.isOk()) << read.error().toString();
	EXPECT_EQ(read.value(), 1);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Host bindings are interpreter infrastructure, not reloadable script
/// modules, and an unknown name is ModuleNotFound.
TEST(ModuleReloadTest, RefusesBindingsAndUnknownModules)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("reload-binding");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());

	mais::ReloadReport binding = runtime.reloadModule("host");
	EXPECT_FALSE(binding.isOk());
	EXPECT_EQ(binding.phase, mais::ReloadPhase::Validate);
	EXPECT_EQ(binding.error.code(), mais::ErrorCode::InvalidArgument);

	mais::Error unloadBinding = runtime.unloadModule("host");
	EXPECT_EQ(unloadBinding.code(), mais::ErrorCode::InvalidArgument);
	EXPECT_TRUE(runtime.isModuleLoaded("host"));
	EXPECT_EQ(runtime.moduleGeneration("host").error().code(),
			  mais::ErrorCode::InvalidArgument);

	mais::ReloadReport unknown = runtime.reloadModule("never_loaded");
	EXPECT_FALSE(unknown.isOk());
	EXPECT_EQ(unknown.phase, mais::ReloadPhase::Validate);
	EXPECT_EQ(unknown.error.code(), mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(unknown.generation, 0U);

	EXPECT_EQ(runtime.unloadModule("never_loaded").code(),
			  mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(runtime.moduleGeneration("never_loaded").error().code(),
			  mais::ErrorCode::ModuleNotFound);

	mais::ReloadReport emptyName = runtime.reloadModule("");
	EXPECT_EQ(emptyName.phase, mais::ReloadPhase::Validate);
	EXPECT_EQ(emptyName.error.code(), mais::ErrorCode::InvalidArgument);
	EXPECT_EQ(runtime.unloadModule("").code(),
			  mais::ErrorCode::InvalidArgument);

	// A module the host can load but that has no source is not a script
	// module: it can be unloaded, but it has no generation and cannot be
	// reloaded.
	ASSERT_TRUE(runtime.loadModule("math").isOk());
	EXPECT_EQ(runtime.moduleGeneration("math").error().code(),
			  mais::ErrorCode::InvalidArgument);

	mais::ReloadReport notSource = runtime.reloadModule("math");

	EXPECT_EQ(notSource.phase, mais::ReloadPhase::Validate);
	EXPECT_EQ(notSource.error.code(), mais::ErrorCode::InvalidArgument);
	EXPECT_EQ(notSource.generation, 0U);

	ASSERT_TRUE(runtime.unloadModule("math").isOk());

	ASSERT_TRUE(runtime.shutdown().isOk());

	// A stopped runtime refuses before it looks at any name.
	mais::ReloadReport stopped = runtime.reloadModule("host");
	EXPECT_EQ(stopped.phase, mais::ReloadPhase::Validate);
	EXPECT_EQ(stopped.error.code(), mais::ErrorCode::NotInitialized);
	EXPECT_EQ(runtime.unloadModule("host").code(),
			  mais::ErrorCode::NotInitialized);
	EXPECT_EQ(runtime.moduleGeneration("host").error().code(),
			  mais::ErrorCode::NotInitialized);
}

/// Unloading removes the runtime's view and the sys.modules entry, so every
/// later operation reports ModuleNotFound.
TEST(ModuleReloadTest, UnloadForgetsTheModule)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_unload.py", R"PY(
def hook():
    return 1
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_unload").isOk());
	EXPECT_TRUE(hasModuleObject("reload_unload"));

	ASSERT_TRUE(runtime.unloadModule("reload_unload").isOk());
	EXPECT_FALSE(runtime.isModuleLoaded("reload_unload"));
	EXPECT_FALSE(hasModuleObject("reload_unload"));

	EXPECT_EQ(runtime.hasFunction("reload_unload", "hook").error().code(),
			  mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(runtime.call("reload_unload", "hook").code(),
			  mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(runtime.moduleGeneration("reload_unload").error().code(),
			  mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(runtime.unloadModule("reload_unload").code(),
			  mais::ErrorCode::ModuleNotFound);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// The runtime owns only the sys.modules entry it installed: if script code
/// replaced the entry, unload forgets its module but leaves the foreign value
/// alone.
TEST(ModuleReloadTest, LeavesAForeignSysModulesEntryAlone)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_foreign.py", R"PY(
import sys


def replace_entry():
    sys.modules[__name__] = object()


def hook():
    return 1
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_foreign").isOk());
	ASSERT_TRUE(runtime.call("reload_foreign", "replace_entry").isOk());

	ASSERT_TRUE(runtime.unloadModule("reload_foreign").isOk());
	EXPECT_FALSE(runtime.isModuleLoaded("reload_foreign"));
	EXPECT_TRUE(hasModuleObject("reload_foreign"))
		<< "unload must not delete an entry it no longer owns";

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Unload then load is a fresh import: generation starts at 1 and the current
/// source is what runs.
TEST(ModuleReloadTest, ReloadsFreshAfterUnload)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_fresh.py", R"PY(
VALUE = 1


def value():
    return VALUE
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_fresh").isOk());
	ASSERT_EQ(runtime.call<std::int64_t>("reload_fresh", "value").value(), 1);

	ASSERT_TRUE(runtime.unloadModule("reload_fresh").isOk());

	directory.write("reload_fresh.py", R"PY(
VALUE = 22
# A different length also invalidates a cached bytecode file on filesystems
# whose timestamp granularity could otherwise hide the edit.


def value():
    return VALUE
)PY");
	invalidateImportCaches();

	ASSERT_TRUE(runtime.loadModule("reload_fresh").isOk());
	EXPECT_EQ(runtime.moduleGeneration("reload_fresh").value(), 1U);
	EXPECT_EQ(runtime.call<std::int64_t>("reload_fresh", "value").value(), 22);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Retiring a generation really releases it: weakrefs to the old module, its
/// functions and its instances are dead after a collection.
TEST(ModuleReloadTest, RetiresThePreviousGeneration)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_retire.py", R"PY(
class Thing:
    pass


def make_thing():
    return Thing()


def hook():
    return 1
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_retire").isOk());

	py::object moduleRef;
	py::object functionRef;
	py::object instanceRef;

	{
		py::object previous = moduleObject("reload_retire");
		py::object instance = previous.attr("make_thing")();

		moduleRef	= weakReference(previous);
		functionRef = weakReference(previous.attr("hook"));
		instanceRef = weakReference(instance);
	}

	directory.write("reload_retire.py", R"PY(
class Thing:
    pass


def make_thing():
    return Thing()


def hook():
    return 2
)PY");

	mais::ReloadReport report = runtime.reloadModule("reload_retire");
	ASSERT_TRUE(report.isOk()) << report.error.toString();

	collectGarbage();

	EXPECT_TRUE(moduleRef().is_none())
		<< "the retired module object is still reachable";
	EXPECT_TRUE(functionRef().is_none())
		<< "a function of the retired generation is still reachable";
	EXPECT_TRUE(instanceRef().is_none())
		<< "an instance of the retired generation is still reachable";

	moduleRef	= py::object();
	functionRef = py::object();
	instanceRef = py::object();

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Bindings and host objects are interpreter state: many reloads must not
/// re-register them or invalidate the objects scripts reach through them.
TEST(ModuleReloadTest, KeepsBindingsAcrossManyReloads)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("reload-bindings");

	mais_test::bindFakeHost(runtime, counter);

	directory.write("reload_bindings.py", R"PY(
import host


def bump():
    host.counter.increment()


def count():
    return host.counter.count
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_bindings").isOk());

	constexpr std::uint64_t reloads = 20;

	for (std::uint64_t index = 0; index < reloads; ++index) {
		mais::Error bump = runtime.call("reload_bindings", "bump");

		ASSERT_TRUE(bump.isOk()) << bump.toString();

		mais::ReloadReport report = runtime.reloadModule("reload_bindings");

		ASSERT_TRUE(report.isOk()) << report.error.toString();
		EXPECT_EQ(report.generation, index + 2);
	}

	EXPECT_EQ(counter.count(), static_cast<int>(reloads));

	mais::Result<std::int64_t> count =
		runtime.call<std::int64_t>("reload_bindings", "count");
	ASSERT_TRUE(count.isOk());
	EXPECT_EQ(count.value(), static_cast<std::int64_t>(reloads));

	ASSERT_TRUE(runtime.shutdown().isOk());
	// The host object outlived the runtime, as the lifetime contract requires.
	EXPECT_EQ(counter.count(), static_cast<int>(reloads));
}

/// Native arguments are converted by the binding on every call, so a host can
/// keep passing its own objects to a hook through many generations.
TEST(ModuleReloadTest, KeepsNativeArgumentConversionAcrossReloads)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;
	mais_test::FakeObserver observer;

	mais_test::bindFakeFacade(runtime, observer);

	directory.write("reload_native.py", R"PY(
import facade


def observe(context):
    facade.observer.record_context(context)
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_native").isOk());

	mais_test::FakeContext context;

	context.frame = 7;

	for (int index = 0; index < 10; ++index) {
		mais::Error error =
			runtime.call("reload_native", "observe",
						 { mais::ScriptArgument::native(
							 &context, mais_test::contextConverter()) });

		ASSERT_TRUE(error.isOk()) << error.toString();
		EXPECT_EQ(observer.observedFrame, 7U);

		mais::ReloadReport report = runtime.reloadModule("reload_native");

		ASSERT_TRUE(report.isOk()) << report.error.toString();
	}

	EXPECT_EQ(observer.observedFrame, 7U);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// A native wrapper retained by another module keeps referring to the same
/// host object after a reload; ASan is the assertion that it did not dangle.
TEST(ModuleReloadTest, KeepsAnOldWrapperValid)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("reload-wrapper");

	mais_test::bindFakeHost(runtime, counter);

	directory.write("reload_holder.py", R"PY(
saved = None


def keep(value):
    global saved
    saved = value


def bump_saved():
    saved.increment()
)PY");
	directory.write("reload_game.py", R"PY(
import host
import reload_holder


def keep_counter():
    reload_holder.keep(host.counter)


def bump():
    host.counter.increment()
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_holder").isOk());
	ASSERT_TRUE(runtime.loadModule("reload_game").isOk());

	ASSERT_TRUE(runtime.call("reload_game", "keep_counter").isOk());

	for (int index = 0; index < 5; ++index) {
		mais::ReloadReport report = runtime.reloadModule("reload_game");

		ASSERT_TRUE(report.isOk()) << report.error.toString();

		mais::Error bump = runtime.call("reload_holder", "bump_saved");

		ASSERT_TRUE(bump.isOk()) << bump.toString();
	}

	EXPECT_EQ(counter.count(), 5);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// A package keeps its package identity and relative imports across a reload.
TEST(ModuleReloadTest, ReloadsAPackageWithoutLosingItsIdentity)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_pkg/helper.py", "VALUE = 1\n");
	directory.write("reload_pkg/__init__.py", R"PY(
from .helper import VALUE

LOCAL = 1


def value():
    return VALUE


def local():
    return LOCAL


def package_name():
    return __package__


def has_path():
    return isinstance(__path__, list)
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_pkg").isOk());

	mais::Result<std::string> before =
		runtime.call<std::string>("reload_pkg", "package_name");
	ASSERT_TRUE(before.isOk());
	EXPECT_EQ(before.value(), "reload_pkg");

	directory.write("reload_pkg/__init__.py", R"PY(
from .helper import VALUE

LOCAL = 2


def value():
    return VALUE


def local():
    return LOCAL


def package_name():
    return __package__


def has_path():
    return isinstance(__path__, list)
)PY");

	mais::ReloadReport report = runtime.reloadModule("reload_pkg");
	ASSERT_TRUE(report.isOk()) << report.error.toString();
	EXPECT_EQ(report.generation, 2U);

	mais::Result<std::string> name =
		runtime.call<std::string>("reload_pkg", "package_name");
	ASSERT_TRUE(name.isOk());
	EXPECT_EQ(name.value(), "reload_pkg");

	mais::Result<bool> hasPath = runtime.call<bool>("reload_pkg", "has_path");
	ASSERT_TRUE(hasPath.isOk());
	EXPECT_TRUE(hasPath.value());

	// The new body re-executed; its dependency did not.
	EXPECT_EQ(runtime.call<std::int64_t>("reload_pkg", "local").value(), 2);
	EXPECT_EQ(runtime.call<std::int64_t>("reload_pkg", "value").value(), 1);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Migration is opt-in and transports opaque text between two named hooks.
TEST(ModuleReloadTest, MigratesStateBetweenGenerations)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_migrate.py", R"PY(
import json

score = 0


def advance():
    global score
    score += 1
    return score


def export_state():
    return json.dumps({"score": score})


def import_state(blob):
    global score
    score = int(json.loads(blob)["score"])
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_migrate").isOk());
	ASSERT_TRUE(runtime.call("reload_migrate", "advance").isOk());
	ASSERT_TRUE(runtime.call("reload_migrate", "advance").isOk());

	mais::ReloadOptions options;

	options.exportFunction = "export_state";
	options.importFunction = "import_state";

	mais::ReloadReport report = runtime.reloadModule("reload_migrate", options);

	ASSERT_TRUE(report.isOk()) << report.error.toString();
	EXPECT_TRUE(report.stateExported);
	EXPECT_TRUE(report.stateRestored);
	EXPECT_GE(report.stateDuration.count(), 0);

	mais::Result<std::int64_t> advance =
		runtime.call<std::int64_t>("reload_migrate", "advance");
	ASSERT_TRUE(advance.isOk());
	EXPECT_EQ(advance.value(), 3);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Every failure of the migration hooks stops in its phase and leaves the
/// previous generation and its state current.
TEST(ModuleReloadTest, RollsBackOnMigrationFailures)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_migration_fail.py", R"PY(
import json

score = 0

export_state = 42


def advance():
    global score
    score += 1
    return score


def import_state(blob):
    global score
    score = int(json.loads(blob)["score"])
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_migration_fail").isOk());
	ASSERT_TRUE(runtime.call("reload_migration_fail", "advance").isOk());

	mais::ReloadOptions options;

	options.exportFunction = "export_state";
	options.importFunction = "import_state";

	mais::ReloadReport notCallable =
		runtime.reloadModule("reload_migration_fail", options);

	EXPECT_EQ(notCallable.phase, mais::ReloadPhase::Export);
	EXPECT_EQ(notCallable.error.code(), mais::ErrorCode::TypeMismatch);
	EXPECT_EQ(notCallable.generation, 1U);

	mais::ReloadOptions missing;

	missing.exportFunction = "absent_export";
	missing.importFunction = "import_state";

	mais::ReloadReport notFound =
		runtime.reloadModule("reload_migration_fail", missing);

	EXPECT_EQ(notFound.phase, mais::ReloadPhase::Export);
	EXPECT_EQ(notFound.error.code(), mais::ErrorCode::FunctionNotFound);

	mais::ReloadOptions half;

	half.exportFunction = "export_state";

	mais::ReloadReport incomplete =
		runtime.reloadModule("reload_migration_fail", half);

	EXPECT_EQ(incomplete.phase, mais::ReloadPhase::Validate);
	EXPECT_EQ(incomplete.error.code(), mais::ErrorCode::InvalidArgument);

	// The previous generation and its state are untouched by all of them.
	mais::Result<std::int64_t> advance =
		runtime.call<std::int64_t>("reload_migration_fail", "advance");
	ASSERT_TRUE(advance.isOk());
	EXPECT_EQ(advance.value(), 2);
	EXPECT_EQ(runtime.moduleGeneration("reload_migration_fail").value(), 1U);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// A non-text export and an exception in either hook are classified in the
/// phase that raised them.
TEST(ModuleReloadTest, ClassifiesMigrationHookErrors)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_migration_none.py", R"PY(
def export_state():
    return None


def import_state(blob):
    pass
)PY");
	directory.write("reload_migration_export_raises.py", R"PY(
def export_state():
    raise ValueError("export refused")


def import_state(blob):
    pass
)PY");
	directory.write("reload_migration_import_raises.py", R"PY(
def export_state():
    return "{}"


def import_state(blob):
    pass
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());

	mais::ReloadOptions options;

	options.exportFunction = "export_state";
	options.importFunction = "import_state";

	ASSERT_TRUE(runtime.loadModule("reload_migration_none").isOk());

	mais::ReloadReport nonText =
		runtime.reloadModule("reload_migration_none", options);

	EXPECT_EQ(nonText.phase, mais::ReloadPhase::Export);
	EXPECT_EQ(nonText.error.code(), mais::ErrorCode::TypeMismatch);

	ASSERT_TRUE(runtime.loadModule("reload_migration_export_raises").isOk());

	mais::ReloadReport exportRaised =
		runtime.reloadModule("reload_migration_export_raises", options);

	EXPECT_EQ(exportRaised.phase, mais::ReloadPhase::Export);
	EXPECT_EQ(exportRaised.error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_EQ(exportRaised.error.exceptionType(), "ValueError");

	// The import hook runs on the new generation, so the old generation must
	// succeed at export and the replacement source raises on import.
	ASSERT_TRUE(runtime.loadModule("reload_migration_import_raises").isOk());

	directory.write("reload_migration_import_raises.py", R"PY(
def export_state():
    return "{}"


def import_state(blob):
    raise RuntimeError("schema mismatch")
)PY");

	mais::ReloadReport importRaised =
		runtime.reloadModule("reload_migration_import_raises", options);

	EXPECT_EQ(importRaised.phase, mais::ReloadPhase::Restore);
	EXPECT_EQ(importRaised.error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_EQ(importRaised.error.exceptionType(), "RuntimeError");
	EXPECT_TRUE(importRaised.stateExported);
	EXPECT_FALSE(importRaised.stateRestored);
	EXPECT_EQ(importRaised.generation, 1U);
	EXPECT_TRUE(runtime.isModuleLoaded("reload_migration_import_raises"));
	EXPECT_EQ(
		runtime.moduleGeneration("reload_migration_import_raises").value(), 1U);

	// A named import hook the new body does not define is a Restore failure,
	// not a successful reload with state silently dropped.
	directory.write("reload_migration_missing_import.py", R"PY(
def export_state():
    return "{}"


def import_state(blob):
    pass
)PY");

	ASSERT_TRUE(runtime.loadModule("reload_migration_missing_import").isOk());

	directory.write("reload_migration_missing_import.py", R"PY(
def export_state():
    return "{}"
)PY");

	mais::ReloadReport missingImport =
		runtime.reloadModule("reload_migration_missing_import", options);

	EXPECT_EQ(missingImport.phase, mais::ReloadPhase::Restore);
	EXPECT_EQ(missingImport.error.code(), mais::ErrorCode::FunctionNotFound);
	EXPECT_TRUE(missingImport.stateExported);
	EXPECT_FALSE(missingImport.stateRestored);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// The soak gate: hundreds of reloads must not accumulate module objects.
TEST(ModuleReloadTest, DoesNotAccumulateGenerations)
{
	TempScriptDirectory directory;
	mais::ScriptRuntime runtime;

	directory.write("reload_soak.py", R"PY(
def hook():
    return __mais_generation__
)PY");

	ASSERT_TRUE(runtime.addSearchPath(directory.path().string()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("reload_soak").isOk());

	constexpr std::uint64_t reloadCount = 500;
	std::vector<py::object> retired;

	retired.reserve(reloadCount);

	for (std::uint64_t index = 0; index < reloadCount; ++index) {
		py::object current = moduleObject("reload_soak");

		retired.push_back(weakReference(current));

		mais::Result<std::int64_t> hook =
			runtime.call<std::int64_t>("reload_soak", "hook");

		ASSERT_TRUE(hook.isOk()) << hook.error().toString();
		EXPECT_EQ(hook.value(), static_cast<std::int64_t>(index + 1));

		mais::ReloadReport report = runtime.reloadModule("reload_soak");

		ASSERT_TRUE(report.isOk()) << report.error.toString();
	}

	collectGarbage();

	for (std::size_t index = 0; index < retired.size(); ++index) {
		ASSERT_TRUE(retired[index]().is_none())
			<< "generation " << (index + 1)
			<< " was not released by the reload";
	}

	mais::Result<std::int64_t> currentHook =
		runtime.call<std::int64_t>("reload_soak", "hook");
	ASSERT_TRUE(currentHook.isOk());
	EXPECT_EQ(currentHook.value(), static_cast<std::int64_t>(reloadCount + 1));
	EXPECT_EQ(runtime.moduleGeneration("reload_soak").value(), reloadCount + 1);

	retired.clear();

	ASSERT_TRUE(runtime.shutdown().isOk());
}
