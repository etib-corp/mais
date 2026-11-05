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

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	const std::string &scriptDirectory()
	{
		static const std::string directory = MAIS_TEST_SCRIPT_DIR;
		return directory;
	}

	/// How long a call from a second thread may take. A correct refusal returns
	/// at once, so reaching this bound means the thread is blocked.
	constexpr std::chrono::seconds refusalTimeout { 10 };

	/// Failure message for a call from a second thread that never came back.
	std::string blockedMessage(const std::string &operations)
	{
		return operations + " did not return within "
			+ std::to_string(refusalTimeout.count())
			+ "s: a second thread is waiting for the GIL instead of being "
			  "refused with WrongThread";
	}

	/// \brief Runs `work` on a second thread and returns its result.
	///
	/// The thread is detached rather than joined on purpose. If the affinity
	/// guard regresses, the call waits for a GIL the owning thread never
	/// releases, and join() would stall the whole test binary instead of
	/// failing one test. The bounded wait turns that into a failed expectation,
	/// and callers capture their runtime through shared_ptr so a thread left
	/// waiting cannot outlive the runtime it is using.
	template<typename Result, typename Work>
	std::optional<Result> runOnForeignThread(Work work)
	{
		auto promise			   = std::make_shared<std::promise<Result>>();
		std::future<Result> future = promise->get_future();

		std::thread([promise, work = std::move(work)] {
			try {
				promise->set_value(work());
			} catch (...) {
				promise->set_exception(std::current_exception());
			}
		}).detach();

		if (future.wait_for(refusalTimeout) != std::future_status::ready) {
			return std::nullopt;
		}
		return future.get();
	}
}	 // namespace

/// The threading contract from docs/THREADING.md: only the thread that called
/// initialize() may touch the interpreter, and a violation is reported instead
/// of blocking on a GIL that thread never releases.
TEST(ThreadAffinityTest, RefusesPythonCallsFromAnotherThread)
{
	auto runtime = std::make_shared<mais::ScriptRuntime>();
	mais_test::FakeCounter counter("affinity");

	mais_test::bindFakeHost(*runtime, counter);
	ASSERT_TRUE(runtime->addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime->initialize().isOk());
	ASSERT_TRUE(runtime->loadModule("fake_host").isOk());

	struct Refusals {
		mais::Error call;
		mais::Error loadModule;
		mais::Error addSearchPath;
		mais::Error shutdown;
	};

	std::optional<Refusals> refusals = runOnForeignThread<Refusals>([runtime] {
		return Refusals {
			runtime->call("fake_host", "bump"),
			runtime->loadModule("fake_host"),
			runtime->addSearchPath(scriptDirectory()),
			runtime->shutdown(),
		};
	});
	ASSERT_TRUE(refusals.has_value())
		<< blockedMessage("call(), loadModule(), addSearchPath(), shutdown()");

	EXPECT_EQ(refusals->call.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusals->loadModule.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusals->addSearchPath.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusals->shutdown.code(), mais::ErrorCode::WrongThread);
	EXPECT_FALSE(refusals->call.message().empty());
	EXPECT_FALSE(mais::isFatal(refusals->call.code()));

	// None of the refused calls changed anything: no script ran, the runtime
	// still owns its interpreter, and the owning thread keeps working.
	EXPECT_EQ(counter.count(), 0);
	EXPECT_TRUE(runtime->isRunning());
	EXPECT_EQ(runtime->searchPaths().size(), 1U);

	mais::Error error = runtime->call("fake_host", "bump");
	ASSERT_TRUE(error.isOk()) << error.toString();
	EXPECT_EQ(counter.count(), 1);

	ASSERT_TRUE(runtime->shutdown().isOk());
}

/// Ownership is established by initialize(), not by where the runtime object
/// was constructed, so a dedicated script thread is a supported arrangement.
TEST(ThreadAffinityTest, TheInitializingThreadBecomesTheOwner)
{
	auto runtime = std::make_shared<mais::ScriptRuntime>();
	mais_test::FakeCounter counter("script-thread");

	mais_test::bindFakeHost(*runtime, counter);
	ASSERT_TRUE(runtime->addSearchPath(scriptDirectory()).isOk());

	std::optional<mais::Error> workerError =
		runOnForeignThread<mais::Error>([runtime] {
			mais::Error error = runtime->initialize();
			if (error) {
				return error;
			}
			error = runtime->loadModule("fake_host");
			if (error) {
				return error;
			}
			error = runtime->call("fake_host", "bump");
			if (error) {
				return error;
			}
			return runtime->shutdown();
		});
	ASSERT_TRUE(workerError.has_value())
		<< blockedMessage("the script thread's initialize(), loadModule(), "
						  "call(), shutdown()");

	EXPECT_TRUE(workerError->isOk()) << workerError->toString();
	EXPECT_EQ(counter.count(), 1);
	EXPECT_FALSE(runtime->isRunning());
}

/// Every initialize() re-establishes the owner, so ownership follows the
/// restart rather than the lifetime of the runtime object.
TEST(ThreadAffinityTest, OwnershipFollowsTheMostRecentInitialize)
{
	auto runtime = std::make_shared<mais::ScriptRuntime>();
	mais_test::FakeCounter counter("handover");

	mais_test::bindFakeHost(*runtime, counter);
	ASSERT_TRUE(runtime->addSearchPath(scriptDirectory()).isOk());

	ASSERT_TRUE(runtime->initialize().isOk());
	ASSERT_TRUE(runtime->loadModule("fake_host").isOk());
	ASSERT_TRUE(runtime->call("fake_host", "bump").isOk());
	ASSERT_TRUE(runtime->shutdown().isOk());

	std::optional<mais::Error> workerError =
		runOnForeignThread<mais::Error>([runtime] {
			mais::Error error = runtime->initialize();
			if (error) {
				return error;
			}
			error = runtime->loadModule("fake_host");
			if (error) {
				return error;
			}
			error = runtime->call("fake_host", "bump");
			if (error) {
				return error;
			}
			// The owner must also be the one to stop: shutting down from
			// another thread is refused, and an owner that exits leaves a
			// runtime nobody can finalize.
			return runtime->shutdown();
		});
	ASSERT_TRUE(workerError.has_value())
		<< blockedMessage("the restarted script thread's initialize(), "
						  "loadModule(), call(), shutdown()");

	ASSERT_TRUE(workerError->isOk()) << workerError->toString();
	EXPECT_EQ(counter.count(), 2);

	// The runtime is free again, and this thread becomes the new owner.
	ASSERT_TRUE(runtime->initialize().isOk());
	ASSERT_TRUE(runtime->shutdown().isOk());
}

/// Introspection and typed calls read Python like call() does, so they obey the
/// same contract: a foreign thread is refused instead of waiting on the GIL.
TEST(ThreadAffinityTest, RefusesIntrospectionAndTypedCallsFromAnotherThread)
{
	auto runtime = std::make_shared<mais::ScriptRuntime>();
	mais_test::FakeCounter counter("introspection");

	mais_test::bindFakeHost(*runtime, counter);
	ASSERT_TRUE(runtime->addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime->initialize().isOk());
	ASSERT_TRUE(runtime->loadModule("fake_host").isOk());

	struct Refusals {
		mais::Error hasFunction;
		mais::Error listFunctions;
		mais::Error typedCall;
		mais::Error typedOptional;
	};

	std::optional<Refusals> refusals = runOnForeignThread<Refusals>([runtime] {
		return Refusals {
			runtime->hasFunction("fake_host", "bump").error(),
			runtime->listFunctions("fake_host").error(),
			runtime->call<bool>("fake_host", "flag").error(),
			// An absent optional hook is success on the owning thread;
			// from the wrong thread it must still be refused, never
			// reported as "no hook".
			runtime->callOptional<bool>("fake_host", "absent_hook").error(),
		};
	});
	ASSERT_TRUE(refusals.has_value())
		<< blockedMessage("hasFunction(), listFunctions(), call<bool>(), "
						  "callOptional<bool>()");

	EXPECT_EQ(refusals->hasFunction.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusals->listFunctions.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusals->typedCall.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusals->typedOptional.code(), mais::ErrorCode::WrongThread);

	// The owning thread still gets the real answers.
	mais::Result<bool> present = runtime->hasFunction("fake_host", "bump");
	ASSERT_TRUE(present.isOk()) << present.error().toString();
	EXPECT_TRUE(present.value());

	mais::Result<std::vector<std::string>> hooks =
		runtime->listFunctions("fake_host");
	ASSERT_TRUE(hooks.isOk()) << hooks.error().toString();
	EXPECT_FALSE(hooks.value().empty());

	mais::Result<bool> typed = runtime->call<bool>("fake_host", "flag");
	ASSERT_TRUE(typed.isOk()) << typed.error().toString();
	EXPECT_TRUE(typed.value());

	EXPECT_TRUE(runtime->callOptional<bool>("fake_host", "absent_hook").isOk());

	ASSERT_TRUE(runtime->shutdown().isOk());
}

/// A policy-bearing call is a Python-touching call too, so it is refused from a
/// foreign thread before it can start a failure streak, and the owning thread
/// still sees the real count.
TEST(ThreadAffinityTest, RefusesPolicyCallsFromAnotherThread)
{
	auto runtime = std::make_shared<mais::ScriptRuntime>();
	mais_test::FakeCounter counter("policy-affinity");

	mais_test::bindFakeHost(*runtime, counter);
	ASSERT_TRUE(runtime->addSearchPath(scriptDirectory()).isOk());
	ASSERT_TRUE(runtime->initialize().isOk());
	ASSERT_TRUE(runtime->loadModule("fake_host").isOk());

	std::optional<mais::CallReport> refusal =
		runOnForeignThread<mais::CallReport>([runtime] {
			return runtime->call(mais::FailurePolicy::ReportOnce, "fake_host",
								 "explode");
		});
	ASSERT_TRUE(refusal.has_value())
		<< blockedMessage("call(FailurePolicy::ReportOnce)");

	EXPECT_EQ(refusal->error.code(), mais::ErrorCode::WrongThread);
	EXPECT_EQ(refusal->consecutiveFailures, 0U);
	EXPECT_FALSE(refusal->suppressed);

	// The refused call never reached the hook, so the first real failure is
	// still the first failure of that streak.
	mais::CallReport first =
		runtime->call(mais::FailurePolicy::ReportOnce, "fake_host", "explode");
	EXPECT_EQ(first.error.code(), mais::ErrorCode::InvocationFailed);
	EXPECT_EQ(first.consecutiveFailures, 1U);
	EXPECT_FALSE(first.suppressed);

	ASSERT_TRUE(runtime->shutdown().isOk());
}
