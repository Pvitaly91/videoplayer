#pragma once

#include <functional>

void RunPreviewLifecycleTests(
    const std::function<void(bool, const wchar_t*)>& check);
