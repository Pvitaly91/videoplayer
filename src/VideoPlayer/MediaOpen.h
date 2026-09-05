#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace videoplayer {

enum class InheritedMediaPathStatus {
    NotRequested,
    Ready,
    Error,
};

std::vector<std::wstring> CollectMediaFileArguments(
    int argc,
    wchar_t* const* argv);

std::vector<std::wstring> ParseOpenDialogPaths(
    const wchar_t* buffer,
    std::size_t capacity);

std::wstring QuoteWindowsCommandLineArgument(std::wstring_view argument);

InheritedMediaPathStatus ReadInheritedMediaPath(
    int argc,
    wchar_t* const* argv,
    std::wstring& mediaPath,
    std::wstring& error);

bool LaunchMediaInNewInstance(
    const std::wstring& mediaPath,
    std::wstring& error);

}  // namespace videoplayer
