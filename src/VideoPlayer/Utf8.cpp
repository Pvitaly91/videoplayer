#include "Utf8.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <limits>

namespace videoplayer {

bool WideToUtf8(const std::wstring_view input, std::string& output) noexcept {
    output.clear();
    if (input.empty()) {
        return true;
    }
    if (input.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        input.find(L'\0') != std::wstring_view::npos) {
        return false;
    }

    const int inputLength = static_cast<int>(input.size());
    const int required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        inputLength,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return false;
    }

    try {
        output.resize(static_cast<std::size_t>(required));
    } catch (...) {
        output.clear();
        return false;
    }

    const int written = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        input.data(),
        inputLength,
        output.data(),
        required,
        nullptr,
        nullptr);
    if (written != required) {
        output.clear();
        return false;
    }
    return true;
}

}  // namespace videoplayer
