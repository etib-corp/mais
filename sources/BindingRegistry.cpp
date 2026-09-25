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

#include "mais/BindingRegistry.hpp"

#include <stdexcept>
#include <utility>

namespace mais
{
	PythonModule::PythonModule(void *handle) noexcept
		: _handle(handle)
	{
	}

	bool PythonModule::valid() const noexcept
	{
		return _handle != nullptr;
	}

	void BindingRegistry::add(std::string name, BindingCallback callback)
	{
		if (name.empty()) {
			throw std::invalid_argument(
				"BindingRegistry::add: module name is empty");
		}
		if (!callback) {
			throw std::invalid_argument(
				"BindingRegistry::add: binding callback for '" + name
				+ "' is empty");
		}
		_modules.push_back(
			ModuleBinding { std::move(name), std::move(callback) });
	}

	bool BindingRegistry::empty() const noexcept
	{
		return _modules.empty();
	}

	std::size_t BindingRegistry::size() const noexcept
	{
		return _modules.size();
	}

	const std::vector<ModuleBinding> &BindingRegistry::modules() const noexcept
	{
		return _modules;
	}

	void BindingRegistry::clear() noexcept
	{
		_modules.clear();
	}
}	 // namespace mais
