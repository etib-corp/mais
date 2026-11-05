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

TEST(LifecycleTest, ReportsMisuseBeforeAndAfterRunning)
{
	mais::ScriptRuntime runtime;

	EXPECT_FALSE(runtime.isRunning());
	EXPECT_EQ(runtime.shutdown().code(), mais::ErrorCode::NotInitialized);
	EXPECT_EQ(runtime.call("host", "bump").code(),
			  mais::ErrorCode::NotInitialized);
	EXPECT_EQ(runtime.loadModule("fake_host").code(),
			  mais::ErrorCode::NotInitialized);
	EXPECT_EQ(runtime.addSearchPath("").code(),
			  mais::ErrorCode::InvalidArgument);

	ASSERT_TRUE(runtime.initialize().isOk());
	EXPECT_TRUE(runtime.isRunning());

	// Re-initializing an existing owner is refused instead of quietly creating
	// a second interpreter.
	mais::Error error = runtime.initialize();
	EXPECT_EQ(error.code(), mais::ErrorCode::AlreadyInitialized);
	EXPECT_NE(error.message().find("already owns"), std::string::npos);

	ASSERT_TRUE(runtime.shutdown().isOk());
	EXPECT_FALSE(runtime.isRunning());
}

TEST(LifecycleTest, RefusesBindingsRegisteredAfterInitialize)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime.initialize().isOk());
	EXPECT_EQ(runtime
				  .registerModule("late",
								  [](mais::PythonModule &) {
								  })
				  .code(),
			  mais::ErrorCode::AlreadyInitialized);
}

TEST(LifecycleTest, KeepsASingleInterpreterOwnerPerProcess)
{
	mais::ScriptRuntime first;
	ASSERT_TRUE(first.initialize().isOk());

	mais::ScriptRuntime second;
	mais::Error error = second.initialize();

	EXPECT_EQ(error.code(), mais::ErrorCode::InterpreterFailure);
	EXPECT_TRUE(mais::isFatal(error.code()));
	EXPECT_FALSE(second.isRunning());

	// The second runtime never took the interpreter over, so the first still
	// owns and stops it.
	ASSERT_TRUE(first.shutdown().isOk());
}

TEST(LifecycleTest, RestartsCleanlyAndReleasesReferences)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("restart");

	mais_test::bindFakeHost(runtime, counter);
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());

	for (int round = 0; round < 3; ++round) {
		ASSERT_TRUE(runtime.initialize().isOk()) << "round " << round;
		ASSERT_TRUE(runtime.loadModule("fake_host").isOk())
			<< "round " << round;
		ASSERT_TRUE(runtime.call("fake_host", "bump").isOk())
			<< "round " << round;
		ASSERT_EQ(counter.count(), round + 1);
		ASSERT_TRUE(runtime.shutdown().isOk()) << "round " << round;
	}

	EXPECT_EQ(counter.count(), 3);
}

TEST(LifecycleTest, DestructorStopsTheInterpreter)
{
	{
		mais::ScriptRuntime runtime;
		ASSERT_TRUE(runtime.initialize().isOk());
	}

	// A runtime destroyed without an explicit shutdown() must still release the
	// interpreter so the next owner can start.
	mais::ScriptRuntime replacement;
	EXPECT_TRUE(replacement.initialize().isOk());
	EXPECT_TRUE(replacement.shutdown().isOk());
}

TEST(LifecycleTest, SearchPathsSurviveARestart)
{
	mais::ScriptRuntime runtime;

	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	EXPECT_EQ(runtime.searchPaths().size(), 1U);

	// A path added while running is applied immediately.
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.shutdown().isOk());
	EXPECT_EQ(runtime.searchPaths().size(), 1U);
}

TEST(LifecycleTest, FailureCountersResetOnShutdown)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("policy-restart");

	mais_test::bindFakeHost(runtime, counter);
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());

	for (int round = 0; round < 3; ++round) {
		ASSERT_TRUE(runtime.initialize().isOk()) << "round " << round;
		ASSERT_TRUE(runtime.loadModule("fake_host").isOk())
			<< "round " << round;

		// Every round starts with a clean slate: the first failure is
		// reported, not suppressed by the previous run.
		mais::CallReport first = runtime.call(mais::FailurePolicy::ReportOnce,
											  "fake_host", "explode");
		EXPECT_FALSE(first.isOk()) << "round " << round;
		EXPECT_FALSE(first.suppressed) << "round " << round;
		EXPECT_EQ(first.consecutiveFailures, 1U) << "round " << round;

		mais::CallReport repeat = runtime.call(mais::FailurePolicy::ReportOnce,
											   "fake_host", "explode");
		EXPECT_TRUE(repeat.isOk()) << "round " << round;
		EXPECT_TRUE(repeat.suppressed) << "round " << round;
		EXPECT_EQ(repeat.consecutiveFailures, 2U) << "round " << round;

		ASSERT_TRUE(runtime.shutdown().isOk()) << "round " << round;
	}
}

TEST(LifecycleTest, DestructorAlsoClearsFailureCounters)
{
	mais_test::FakeCounter counter("policy-destructor");
	{
		mais::ScriptRuntime runtime;

		mais_test::bindFakeHost(runtime, counter);
		ASSERT_TRUE(runtime.initialize().isOk());
		ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
		ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

		mais::CallReport failure = runtime.call(mais::FailurePolicy::ReportOnce,
												"fake_host", "explode");
		ASSERT_FALSE(failure.isOk());
		ASSERT_TRUE(
			runtime
				.call(mais::FailurePolicy::ReportOnce, "fake_host", "explode")
				.suppressed);
	}

	// A new runtime never inherits the old counters, even though the previous
	// one was only destroyed rather than shut down.
	mais::ScriptRuntime replacement;

	mais_test::bindFakeHost(replacement, counter);
	ASSERT_TRUE(replacement.initialize().isOk());
	ASSERT_TRUE(replacement.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(replacement.loadModule("fake_host").isOk());

	mais::CallReport failure = replacement.call(mais::FailurePolicy::ReportOnce,
												"fake_host", "explode");
	EXPECT_FALSE(failure.isOk());
	EXPECT_FALSE(failure.suppressed);
	EXPECT_EQ(failure.consecutiveFailures, 1U);

	ASSERT_TRUE(replacement.shutdown().isOk());
}
