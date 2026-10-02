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

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace
{
	const std::string &scriptDirectory()
	{
		static const std::string directory = MAIS_TEST_SCRIPT_DIR;
		return directory;
	}

	/// A type no binding registers, so converting it to Python must fail
	/// rather than crash.
	struct UnregisteredFacade {
		int value = 7;
	};

	mais::ScriptArgument::NativeConverter unregisteredConverter()
	{
		return +[](const void *value) -> void * {
			return pybind11::cast(
					   static_cast<const UnregisteredFacade *>(value),
					   pybind11::return_value_policy::reference)
				.release()
				.ptr();
		};
	}
}	 // namespace

/// The standalone acceptance test: a host injects a native service, invokes a
/// Python function that uses it, and sees the native object change.
TEST(FakeHostAcceptanceTest, InjectsAHostServiceAndInvokesAScript)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("actions");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	EXPECT_EQ(counter.count(), 0);

	mais::Error error = runtime.call("fake_host", "bump");
	ASSERT_TRUE(error.isOk()) << error.toString();
	EXPECT_EQ(counter.count(), 1);

	ASSERT_TRUE(runtime.call("fake_host", "bump").isOk());
	EXPECT_EQ(counter.count(), 2);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

TEST(FakeHostAcceptanceTest, ReportsAPythonExceptionWithATraceback)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("errors");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Error error = runtime.call("fake_host", "explode");

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_TRUE(mais::isRecoverable(error.code()));
	EXPECT_NE(error.message().find("boom from Python"), std::string::npos);
	ASSERT_TRUE(error.hasTraceback());
	EXPECT_NE(error.traceback().find("Traceback (most recent call last)"),
			  std::string::npos);
	EXPECT_NE(error.traceback().find("in explode"), std::string::npos);
	EXPECT_NE(error.traceback().find("fake_host.py"), std::string::npos);
}

TEST(FakeHostAcceptanceTest, KeepsRunningAfterAScriptError)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("recovery");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	EXPECT_FALSE(runtime.call("fake_host", "explode").isOk());
	EXPECT_FALSE(runtime.call("fake_host", "explode").isOk());

	EXPECT_TRUE(runtime.isRunning());
	ASSERT_TRUE(runtime.call("fake_host", "bump").isOk());
	EXPECT_EQ(counter.count(), 1);
}

TEST(ScriptArgumentTest, PassesScalarsInOrder)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("arguments");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Error counted = runtime.call("fake_host", "bump_many",
									   { mais::ScriptArgument::integer(4) });
	ASSERT_TRUE(counted.isOk()) << counted.toString();
	EXPECT_EQ(counter.count(), 4);

	ASSERT_TRUE(runtime.call("fake_host", "reset").isOk());
	ASSERT_TRUE(runtime
					.call("fake_host", "apply",
						  { mais::ScriptArgument::string("label"),
							mais::ScriptArgument::boolean(true) })
					.isOk());
	EXPECT_EQ(counter.count(), 1);

	ASSERT_TRUE(runtime
					.call("fake_host", "apply",
						  { mais::ScriptArgument::string("label"),
							mais::ScriptArgument::boolean(false) })
					.isOk());
	EXPECT_EQ(counter.count(), 1);
}

TEST(ScriptArgumentTest, ReportsArgumentTypeErrors)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("type-errors");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Error error =
		runtime.call("fake_host", "bump_many",
					 { mais::ScriptArgument::string("not a number") });

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::TypeMismatch);
	EXPECT_TRUE(error.hasTraceback());
}

TEST(OptionalHookTest, ReportsMissingFunctions)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("hooks");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Error error = runtime.call("fake_host", "on_start");

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::FunctionNotFound);
	EXPECT_NE(error.message().find("on_start"), std::string::npos);
}

TEST(OptionalHookTest, ToleratesMissingFunctionsButRunsPresentOnes)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("optional-hooks");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	EXPECT_TRUE(runtime.callOptional("fake_host", "configure").isOk());
	EXPECT_TRUE(runtime.callOptional("fake_host", "on_start").isOk());
	EXPECT_EQ(counter.count(), 0);

	EXPECT_TRUE(runtime.callOptional("fake_host", "bump").isOk());
	EXPECT_EQ(counter.count(), 1);
}

TEST(OptionalHookTest, ReportsCallingAModuleThatWasNeverLoaded)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime.initialize().isOk());

	mais::Error error = runtime.call("fake_host", "bump");

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::ModuleNotFound);
	EXPECT_FALSE(runtime.isModuleLoaded("fake_host"));
}

TEST(ScriptLoadingTest, ReportsMissingModules)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());

	mais::Error error = runtime.loadModule("does_not_exist");

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::ModuleNotFound);
	EXPECT_NE(error.message().find("does_not_exist"), std::string::npos);
	EXPECT_FALSE(runtime.isModuleLoaded("does_not_exist"));
}

TEST(ScriptLoadingTest, ReportsImportFailuresWithATraceback)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());

	mais::Error error = runtime.loadModule("broken_module");

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_NE(error.message().find("broken_module"), std::string::npos);
	EXPECT_NE(error.message().find("always fails to import"),
			  std::string::npos);
	EXPECT_TRUE(error.hasTraceback());
	EXPECT_FALSE(runtime.isModuleLoaded("broken_module"));
	EXPECT_TRUE(runtime.isRunning());
}

TEST(ScriptLoadingTest, ValidatesSearchPaths)
{
	mais::ScriptRuntime runtime;

	EXPECT_EQ(runtime.addSearchPath("").code(),
			  mais::ErrorCode::InvalidArgument);

	mais::Error error =
		runtime.addSearchPath(MAIS_TEST_SCRIPT_DIR "/not-a-directory");
	EXPECT_EQ(error.code(), mais::ErrorCode::ScriptNotFound);
	EXPECT_NE(error.message().find("not-a-directory"), std::string::npos);

	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	EXPECT_EQ(runtime.searchPaths().size(), 1U);

	// Adding the same directory twice is idempotent.
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	EXPECT_EQ(runtime.searchPaths().size(), 1U);
}

TEST(BindingTest, KeepsBindingsAfterInitialize)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("bindings");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_EQ(runtime.bindings().size(), 1U);
	ASSERT_TRUE(runtime.initialize().isOk());
	EXPECT_EQ(runtime.bindings().size(), 1U);
	EXPECT_EQ(runtime.bindings().modules()[0].name, "host");
	EXPECT_TRUE(runtime.isModuleLoaded("host"));
}

TEST(BindingTest, ReportsAFailingHostBinding)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime
					.registerModule("host",
									[](mais::PythonModule &) {
										throw std::runtime_error(
											"binding blew up");
									})
					.isOk());

	mais::Error error = runtime.initialize();

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_NE(error.message().find("binding blew up"), std::string::npos);
	// initialize() is atomic: a failed binding leaves Python stopped.
	EXPECT_FALSE(runtime.isRunning());
}

TEST(BindingTest, ReportsAPythonExceptionRaisedByABinding)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime
					.registerModule("host",
									[](mais::PythonModule &module) {
										pybind11::module_ &host =
											module.as<pybind11::module_>();
										pybind11::exec(
											"raise RuntimeError('binding "
											"failed')",
											host.attr("__dict__"));
									})
					.isOk());

	mais::Error error = runtime.initialize();

	ASSERT_FALSE(error.isOk());
	EXPECT_NE(error.message().find("binding failed"), std::string::npos);
	EXPECT_FALSE(runtime.isRunning());
}

TEST(BindingTest, RejectsUnusableRegistrations)
{
	mais::ScriptRuntime runtime;

	EXPECT_EQ(runtime
				  .registerModule("",
								  [](mais::PythonModule &) {
								  })
				  .code(),
			  mais::ErrorCode::InvalidArgument);
	EXPECT_EQ(runtime.registerModule("host", {}).code(),
			  mais::ErrorCode::InvalidArgument);
	EXPECT_TRUE(runtime.bindings().empty());
}

/// A host hands a native facade to a hook and the script mutates it.
TEST(NativeArgumentTest, LetsAScriptMutateAHostOwnedFacade)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeObserver observer;
	mais_test::FakeSettings settings;

	mais_test::bindFakeFacade(runtime, observer);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("native_args").isOk());

	mais::Error error =
		runtime.call("native_args", "configure",
					 { mais::ScriptArgument::native(
						 &settings, mais_test::settingsConverter()) });

	ASSERT_TRUE(error.isOk()) << error.toString();
	EXPECT_EQ(settings.title, "configured by script");
	EXPECT_EQ(settings.width, 1280);
	EXPECT_EQ(settings.height, 720);
	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// A host hands a native facade to a hook and the script reads it back,
/// still referring to the host's own object.
TEST(NativeArgumentTest, LetsAScriptReadTheHostObjectBack)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeObserver observer;
	mais_test::FakeSettings settings;

	mais_test::bindFakeFacade(runtime, observer);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("native_args").isOk());

	settings.width = 1920;
	mais::Error error =
		runtime.call("native_args", "observe_settings",
					 { mais::ScriptArgument::native(
						 &settings, mais_test::settingsConverter()) });

	ASSERT_TRUE(error.isOk()) << error.toString();
	EXPECT_EQ(observer.lastSettings, &settings);
	EXPECT_EQ(observer.observedWidth, 1920);
}

/// Native and scalar arguments keep their order within one call.
TEST(NativeArgumentTest, PassesNativeAndScalarArgumentsInOrder)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeObserver observer;
	mais_test::FakeContext context;

	mais_test::bindFakeFacade(runtime, observer);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("native_args").isOk());

	mais::Error error = runtime.call(
		"native_args", "observe_context",
		{ mais::ScriptArgument::native(&context, mais_test::contextConverter()),
		  mais::ScriptArgument::number(0.5) });

	ASSERT_TRUE(error.isOk()) << error.toString();
	EXPECT_EQ(context.frame, 1U);
	EXPECT_DOUBLE_EQ(context.elapsed, 0.5);
	EXPECT_EQ(observer.observedFrame, 1U);
}

/// An object the runtime cannot convert fails with TypeMismatch, never a
/// crash, and leaves the runtime usable.
TEST(NativeArgumentTest, ReportsAnObjectThatCannotBeConverted)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("native-errors");
	UnregisteredFacade facade;

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Error error = runtime.call(
		"fake_host", "bump",
		{ mais::ScriptArgument::native(&facade, unregisteredConverter()) });

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::TypeMismatch);
	EXPECT_TRUE(mais::isRecoverable(error.code()));
	EXPECT_NE(error.message().find("argument 0"), std::string::npos);
	EXPECT_TRUE(runtime.isRunning());

	mais::Error followUp = runtime.call("fake_host", "bump");
	ASSERT_TRUE(followUp.isOk()) << followUp.toString();
	EXPECT_EQ(counter.count(), 1);
}

/// A native argument without a converter is rejected before any Python call.
TEST(NativeArgumentTest, RejectsANativeArgumentWithoutAConverter)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeObserver observer;
	mais_test::FakeSettings settings;

	mais_test::bindFakeFacade(runtime, observer);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("native_args").isOk());

	mais::Error error =
		runtime.call("native_args", "configure",
					 { mais::ScriptArgument::native(&settings, {}) });

	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.code(), mais::ErrorCode::InvalidArgument);
	EXPECT_NE(error.message().find("argument 0"), std::string::npos);
}

/// The lifetime contract: the runtime may be destroyed before the host object
/// it borrowed, because shutdown() releases every Python wrapper first.
TEST(NativeArgumentTest, OutlivesTheRuntimeThatBorrowedIt)
{
	auto settings = std::make_unique<mais_test::FakeSettings>();
	{
		mais::ScriptRuntime runtime;
		mais_test::FakeObserver observer;

		mais_test::bindFakeFacade(runtime, observer);

		ASSERT_TRUE(runtime.initialize().isOk());
		ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
		ASSERT_TRUE(runtime.loadModule("native_args").isOk());
		ASSERT_TRUE(
			runtime
				.call("native_args", "configure",
					  { mais::ScriptArgument::native(
						  settings.get(), mais_test::settingsConverter()) })
				.isOk());
		ASSERT_TRUE(runtime.shutdown().isOk());
	}
	// The runtime is gone; the host object is intact and still usable.
	EXPECT_EQ(settings->width, 1280);
}
