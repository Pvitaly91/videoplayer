#pragma once

#include <string>
#include <string_view>

namespace videoplayer {

// Converts UTF-16 to UTF-8 without normalizing or otherwise changing a path.
bool WideToUtf8(std::wstring_view input, std::string& output) noexcept;

}  // namespace videoplayer
