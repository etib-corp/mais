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

#include <string>

namespace
{
	const std::string &scriptDirectory()
	{
		static const std::string directory = MAIS_TEST_SCRIPT_DIR;
		return directory;
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
