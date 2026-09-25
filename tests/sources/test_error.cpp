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

#include "mais/Error.hpp"

#include <gtest/gtest.h>

#include <string>

TEST(ErrorTest, DefaultConstructedErrorIsOk)
{
	mais::Error error;

	EXPECT_TRUE(error.isOk());
	EXPECT_FALSE(static_cast<bool>(error));
	EXPECT_EQ(error.code(), mais::ErrorCode::Ok);
	EXPECT_TRUE(error.message().empty());
	EXPECT_FALSE(error.hasTraceback());
	EXPECT_EQ(error.toString(), "[Ok]");
}

TEST(ErrorTest, CarriesAMessage)
{
	mais::Error error(mais::ErrorCode::ModuleNotFound, "no such module");

	EXPECT_FALSE(error.isOk());
	EXPECT_TRUE(static_cast<bool>(error));
	EXPECT_EQ(error.code(), mais::ErrorCode::ModuleNotFound);
	EXPECT_EQ(error.message(), "no such module");
	EXPECT_FALSE(error.hasTraceback());
	EXPECT_EQ(error.toString(), "[ModuleNotFound] no such module");
}

TEST(ErrorTest, AppendsTheTracebackWhenPresent)
{
	mais::Error error(mais::ErrorCode::InvocationFailed, "ValueError: boom",
					  "Traceback (most recent call last):\nValueError: boom");

	EXPECT_TRUE(error.hasTraceback());
	EXPECT_EQ(error.toString(),
			  "[InvocationFailed] ValueError: boom\n"
			  "Traceback (most recent call last):\nValueError: boom");
}

TEST(ErrorTest, NamesEveryCode)
{
	EXPECT_EQ(mais::errorCodeName(mais::ErrorCode::Ok), "Ok");
	EXPECT_EQ(mais::errorCodeName(mais::ErrorCode::AlreadyInitialized),
			  "AlreadyInitialized");
	EXPECT_EQ(mais::errorCodeName(mais::ErrorCode::ScriptNotFound),
			  "ScriptNotFound");
	EXPECT_EQ(mais::errorCodeName(mais::ErrorCode::InvocationFailed),
			  "InvocationFailed");
}

TEST(ErrorCodeTest, OnlyInterpreterFailureIsFatal)
{
	EXPECT_TRUE(mais::isFatal(mais::ErrorCode::InterpreterFailure));
	EXPECT_FALSE(mais::isFatal(mais::ErrorCode::NotInitialized));
	EXPECT_FALSE(mais::isFatal(mais::ErrorCode::Ok));

	EXPECT_TRUE(mais::isRecoverable(mais::ErrorCode::FunctionNotFound));
	EXPECT_TRUE(mais::isRecoverable(mais::ErrorCode::InvocationFailed));
	EXPECT_FALSE(mais::isRecoverable(mais::ErrorCode::Ok));
	EXPECT_FALSE(mais::isRecoverable(mais::ErrorCode::InterpreterFailure));

	EXPECT_TRUE(mais::isOk(mais::ErrorCode::Ok));
	EXPECT_FALSE(mais::isOk(mais::ErrorCode::TypeMismatch));
}
