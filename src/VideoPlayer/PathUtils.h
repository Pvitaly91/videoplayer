#pragma once

#include <string>

namespace videoplayer {

enum class MediaPathStatus {
    Ready,
    Empty,
    CannotResolve,
    NotFound,
    Directory,
    Inaccessible,
};

// Resolves relative input without requiring the target to exist. If the
// ordinary Win32 form hits MAX_PATH, resolution is retried with an extended
// path prefix so the same code also works on Windows 7.
bool MakeAbsolutePath(const std::wstring& path, std::wstring& absolutePath);

// Converts an absolute local or UNC path to the extended Win32 form only when
// it is needed. Existing device/extended prefixes are preserved.
std::wstring PathForWin32Io(const std::wstring& absolutePath);

// Applies the exact normalization and file classification used before media
// is handed to PlayerEngine. The display path never receives an extended
// prefix; ioPath does when Win32 long-path access requires it.
MediaPathStatus PrepareMediaFilePath(
    const std::wstring& path,
    std::wstring& displayPath,
    std::wstring& ioPath);

}  // namespace videoplayer
