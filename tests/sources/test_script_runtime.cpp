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

#include <cstdint>
#include <stdexcept>
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

TEST(TypedReturnTest, ReadsABoolean)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-bools");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<bool> flagged = runtime.call<bool>("fake_host", "flag");
	ASSERT_TRUE(flagged.isOk()) << flagged.error().toString();
	ASSERT_TRUE(flagged.hasValue());
	EXPECT_TRUE(flagged.value());

	mais::Result<bool> passed = runtime.call<bool>(
		"fake_host", "identity", { mais::ScriptArgument::boolean(false) });
	ASSERT_TRUE(passed.isOk()) << passed.error().toString();
	ASSERT_TRUE(passed.hasValue());
	EXPECT_FALSE(passed.value());
}

TEST(TypedReturnTest, ReadsAnInteger)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-integers");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	// 2^62: a value that only fits a 64-bit integer.
	constexpr std::int64_t large	= 4611686018427387904LL;
	mais::Result<std::int64_t> read = runtime.call<std::int64_t>(
		"fake_host", "identity", { mais::ScriptArgument::integer(large) });
	ASSERT_TRUE(read.isOk()) << read.error().toString();
	ASSERT_TRUE(read.hasValue());
	EXPECT_EQ(read.value(), large);

	ASSERT_TRUE(runtime.call("fake_host", "reset").isOk());
	ASSERT_TRUE(runtime
					.call("fake_host", "bump_many",
						  { mais::ScriptArgument::integer(3) })
					.isOk());

	mais::Result<std::int64_t> count =
		runtime.call<std::int64_t>("fake_host", "count");
	ASSERT_TRUE(count.isOk()) << count.error().toString();
	ASSERT_TRUE(count.hasValue());
	EXPECT_EQ(count.value(), 3);
}

TEST(TypedReturnTest, ReadsADouble)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-doubles");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<double> ratio =
		runtime.call<double>("fake_host", "divide",
							 { mais::ScriptArgument::number(1.0),
							   mais::ScriptArgument::number(2.0) });
	ASSERT_TRUE(ratio.isOk()) << ratio.error().toString();
	ASSERT_TRUE(ratio.hasValue());
	EXPECT_DOUBLE_EQ(ratio.value(), 0.5);
}

TEST(TypedReturnTest, ReadsAString)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-strings");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<std::string> labelled =
		runtime.call<std::string>("fake_host", "apply",
								  { mais::ScriptArgument::string("label"),
									mais::ScriptArgument::boolean(true) });
	ASSERT_TRUE(labelled.isOk()) << labelled.error().toString();
	ASSERT_TRUE(labelled.hasValue());
	EXPECT_EQ(labelled.value(), "label");
	EXPECT_EQ(counter.count(), 1);

	// The script returns "héllo maïs"; the escapes keep this file ASCII.
	mais::Result<std::string> unicode =
		runtime.call<std::string>("fake_host", "label");
	ASSERT_TRUE(unicode.isOk()) << unicode.error().toString();
	ASSERT_TRUE(unicode.hasValue());
	EXPECT_EQ(unicode.value(), "h\xC3\xA9llo ma\xC3\xAFs");
}

TEST(TypedReturnTest, ReportsNoneAsOkWithoutValue)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-none");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<std::int64_t> explicitNone =
		runtime.call<std::int64_t>("fake_host", "nothing");
	EXPECT_TRUE(explicitNone.isOk());
	EXPECT_FALSE(explicitNone.hasValue());
	EXPECT_TRUE(explicitNone.error().isOk());

	// `reset()` returns None implicitly: it has no return statement.
	mais::Result<bool> implicitNone =
		runtime.callOptional<bool>("fake_host", "reset");
	EXPECT_TRUE(implicitNone.isOk());
	EXPECT_FALSE(implicitNone.hasValue());
}

TEST(TypedReturnTest, ReportsFunctionNotFoundForTheStrictVariant)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-strict-missing");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<bool> missing = runtime.call<bool>("fake_host", "on_start");
	ASSERT_FALSE(missing.isOk());
	EXPECT_EQ(missing.error().code(), mais::ErrorCode::FunctionNotFound);
	EXPECT_FALSE(missing.hasValue());
	EXPECT_NE(missing.error().message().find("on_start"), std::string::npos);
}

TEST(TypedReturnTest, ToleratesMissingFunctionsInTheOptionalVariant)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-optional-missing");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<bool> missing =
		runtime.callOptional<bool>("fake_host", "on_start");
	EXPECT_TRUE(missing.isOk());
	EXPECT_FALSE(missing.hasValue());
}

TEST(TypedReturnTest, ReportsTypeMismatches)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-mismatches");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	// `count()` returns an int, not a bool.
	mais::Result<bool> asBool = runtime.call<bool>("fake_host", "count");
	ASSERT_FALSE(asBool.isOk());
	EXPECT_EQ(asBool.error().code(), mais::ErrorCode::TypeMismatch);
	EXPECT_FALSE(asBool.hasValue());
	EXPECT_NE(asBool.error().message().find("returned int"), std::string::npos);
	EXPECT_NE(asBool.error().message().find("bool was requested"),
			  std::string::npos);
	EXPECT_NE(asBool.error().message().find("count"), std::string::npos);

	// True is a Python int as well, but it does not read back as one.
	mais::Result<std::int64_t> asInteger =
		runtime.call<std::int64_t>("fake_host", "flag");
	ASSERT_FALSE(asInteger.isOk());
	EXPECT_EQ(asInteger.error().code(), mais::ErrorCode::TypeMismatch);
	EXPECT_NE(asInteger.error().message().find("returned bool"),
			  std::string::npos);
	EXPECT_NE(asInteger.error().message().find("int was requested"),
			  std::string::npos);

	// A float is never truncated to an integer.
	mais::Result<std::int64_t> truncated =
		runtime.call<std::int64_t>("fake_host", "divide",
								   { mais::ScriptArgument::number(3.0),
									 mais::ScriptArgument::number(2.0) });
	ASSERT_FALSE(truncated.isOk());
	EXPECT_EQ(truncated.error().code(), mais::ErrorCode::TypeMismatch);
	EXPECT_NE(truncated.error().message().find("returned float"),
			  std::string::npos);

	// An int is not silently widened to a float either.
	mais::Result<double> widened = runtime.call<double>("fake_host", "count");
	ASSERT_FALSE(widened.isOk());
	EXPECT_EQ(widened.error().code(), mais::ErrorCode::TypeMismatch);
	EXPECT_NE(widened.error().message().find("returned int"),
			  std::string::npos);
	EXPECT_NE(widened.error().message().find("float was requested"),
			  std::string::npos);

	// And a dict is not a string.
	mais::Result<std::string> mapped =
		runtime.call<std::string>("fake_host", "mapping");
	ASSERT_FALSE(mapped.isOk());
	EXPECT_EQ(mapped.error().code(), mais::ErrorCode::TypeMismatch);
	EXPECT_NE(mapped.error().message().find("returned dict"),
			  std::string::npos);
	EXPECT_NE(mapped.error().message().find("str was requested"),
			  std::string::npos);
}

TEST(TypedReturnTest, ReportsIntegersThatDoNotFitInt64)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-overflow");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<std::int64_t> huge =
		runtime.call<std::int64_t>("fake_host", "huge");
	ASSERT_FALSE(huge.isOk());
	EXPECT_EQ(huge.error().code(), mais::ErrorCode::TypeMismatch);
	EXPECT_NE(huge.error().message().find("outside the range"),
			  std::string::npos);
}

TEST(TypedReturnTest, KeepsReportingPythonExceptions)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-exceptions");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Result<bool> error = runtime.call<bool>("fake_host", "explode");
	ASSERT_FALSE(error.isOk());
	EXPECT_EQ(error.error().code(), mais::ErrorCode::InvocationFailed);
	EXPECT_FALSE(error.hasValue());
	EXPECT_NE(error.error().message().find("boom from Python"),
			  std::string::npos);
	ASSERT_TRUE(error.error().hasTraceback());
	EXPECT_NE(error.error().traceback().find("in explode"), std::string::npos);
}

TEST(TypedReturnTest, KeepsRunningAfterATypeMismatch)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("typed-recovery");

	mais_test::bindFakeHost(runtime, counter);

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	EXPECT_FALSE(runtime.call<bool>("fake_host", "count").isOk());
	EXPECT_TRUE(runtime.isRunning());

	mais::Result<std::int64_t> count =
		runtime.call<std::int64_t>("fake_host", "count");
	ASSERT_TRUE(count.isOk()) << count.error().toString();
	ASSERT_TRUE(count.hasValue());
	EXPECT_EQ(count.value(), 0);
}

TEST(ResultTest, ValueWithoutAValueThrows)
{
	mais::Result<bool> none = mais::Result<bool>::none();

	EXPECT_TRUE(none.isOk());
	EXPECT_FALSE(none.hasValue());
	EXPECT_THROW((void)none.value(), std::logic_error);
}

TEST(ResultTest, CarriesAValueOrAnError)
{
	mais::Result<bool> value(true);
	EXPECT_TRUE(value.isOk());
	EXPECT_TRUE(value.hasValue());
	EXPECT_TRUE(value.value());
	EXPECT_TRUE(value.error().isOk());

	mais::Result<bool> failure(
		mais::Error(mais::ErrorCode::TypeMismatch, "boom"));
	EXPECT_FALSE(failure.isOk());
	EXPECT_FALSE(failure.hasValue());
	EXPECT_EQ(failure.error().code(), mais::ErrorCode::TypeMismatch);
}
