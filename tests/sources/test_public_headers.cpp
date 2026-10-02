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

// The acceptance criterion for the native-argument work: every public header
// must compile without pybind11 or Python.h. This translation unit is built
// into its own target, whose only include directory is headers/, and it never
// links maïs. Reaching the end of this file means the boundary still holds.

#include "mais/BindingRegistry.hpp"
#include "mais/Error.hpp"
#include "mais/ScriptRuntime.hpp"

namespace mais_test
{
	/// Touches the public surface so the compiler cannot skip parsing it.
	inline void touchPublicApi(mais::ScriptRuntime &runtime) noexcept
	{
		(void)runtime.isRunning();
		(void)runtime.bindings().size();
		(void)runtime.searchPaths();
	}

	/// Instantiates the header-only accessors.
	inline void touchHandles(mais::PythonModule &module,
							 mais::ScriptArgument &argument) noexcept
	{
		(void)module.valid();
		(void)argument.value();
		(void)argument.typeName();
	}
}	 // namespace mais_test
