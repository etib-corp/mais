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
#include <thread>

namespace
{
	const std::string &scriptDirectory()
	{
		static const std::string directory = MAIS_TEST_SCRIPT_DIR;
		return directory;
	}
}	 // namespace

/// The threading contract from docs/THREADING.md: only the thread that called
/// initialize() may touch the interpreter, and a violation is reported instead
/// of blocking on a GIL that thread never releases.
TEST(ThreadAffinityTest, RefusesPythonCallsFromAnotherThread)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("affinity");

	mais_test::bindFakeHost(runtime, counter);
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());

	mais::Error callError;
	mais::Error loadError;
	mais::Error searchPathError;
	mais::Error shutdownError;

	std::thread other([&] {
		callError		= runtime.call("fake_host", "bump");
		loadError		= runtime.loadModule("fake_host");
		searchPathError = runtime.addSearchPath(scriptDirectory());
		shutdownError	= runtime.shutdown();
	});
	other.join();

	EXPECT_EQ(callError.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(loadError.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(searchPathError.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(shutdownError.code(), mais::ErrorCode::WrongThread);
	EXPECT_FALSE(callError.message().empty());
	EXPECT_FALSE(mais::isFatal(callError.code()));

	// None of the refused calls changed anything: no script ran, the runtime
	// still owns its interpreter, and the owning thread keeps working.
	EXPECT_EQ(counter.count(), 0);
	EXPECT_TRUE(runtime.isRunning());
	EXPECT_EQ(runtime.searchPaths().size(), 1U);

	mais::Error error = runtime.call("fake_host", "bump");
	ASSERT_TRUE(error.isOk()) << error.toString();
	EXPECT_EQ(counter.count(), 1);

	ASSERT_TRUE(runtime.shutdown().isOk());
}

/// Ownership is established by initialize(), not by where the runtime object
/// was constructed, so a dedicated script thread is a supported arrangement.
TEST(ThreadAffinityTest, TheInitializingThreadBecomesTheOwner)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("script-thread");

	mais_test::bindFakeHost(runtime, counter);
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());

	mais::Error workerError;
	std::thread worker([&] {
		workerError = runtime.initialize();
		if (workerError) {
			return;
		}
		workerError = runtime.loadModule("fake_host");
		if (workerError) {
			return;
		}
		workerError = runtime.call("fake_host", "bump");
		if (workerError) {
			return;
		}
		workerError = runtime.shutdown();
	});
	worker.join();

	EXPECT_TRUE(workerError.isOk()) << workerError.toString();
	EXPECT_EQ(counter.count(), 1);
	EXPECT_FALSE(runtime.isRunning());
}

/// Every initialize() re-establishes the owner, so ownership follows the
/// restart rather than the lifetime of the runtime object.
TEST(ThreadAffinityTest, OwnershipFollowsTheMostRecentInitialize)
{
	mais::ScriptRuntime runtime;
	mais_test::FakeCounter counter("handover");

	mais_test::bindFakeHost(runtime, counter);
	ASSERT_TRUE(runtime.addSearchPath(scriptDirectory()).isOk());

	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.loadModule("fake_host").isOk());
	ASSERT_TRUE(runtime.call("fake_host", "bump").isOk());
	ASSERT_TRUE(runtime.shutdown().isOk());

	mais::Error workerError;
	std::thread worker([&] {
		workerError = runtime.initialize();
		if (workerError) {
			return;
		}
		workerError = runtime.loadModule("fake_host");
		if (workerError) {
			return;
		}
		workerError = runtime.call("fake_host", "bump");
		if (workerError) {
			return;
		}
		// The owner must also be the one to stop: shutting down from another
		// thread is refused, and an owner that exits leaves a runtime nobody
		// can finalize.
		workerError = runtime.shutdown();
	});
	worker.join();

	ASSERT_TRUE(workerError.isOk()) << workerError.toString();
	EXPECT_EQ(counter.count(), 2);

	// The runtime is free again, and this thread becomes the new owner.
	ASSERT_TRUE(runtime.initialize().isOk());
	ASSERT_TRUE(runtime.shutdown().isOk());
}
