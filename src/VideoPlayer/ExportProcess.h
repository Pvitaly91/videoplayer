#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace videoplayer {

struct ExportProcessResult final {
    bool started = false;
    bool cancelled = false;
    unsigned long exitCode = static_cast<unsigned long>(-1);
    std::string standardOutput;
    std::string standardError;
    std::wstring error;
};

using ExportProgressLine = std::function<void(const std::string&)>;

std::wstring ExportToolsDirectory();
ExportProcessResult RunHiddenExportProcess(
    const std::wstring& executable,
    const std::vector<std::wstring>& arguments,
    std::atomic_bool* cancellation = nullptr,
    ExportProgressLine progress = {},
    std::size_t outputLimit = 1024U * 1024U,
    const std::string& standardInput = {});

}  // namespace videoplayer
