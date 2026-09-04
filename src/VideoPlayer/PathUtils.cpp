#include "targetver.h"
#include "PathUtils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwctype>
#include <vector>

namespace videoplayer {
namespace {

bool IsSeparator(const wchar_t character) noexcept {
    return character == L'\\' || character == L'/';
}

bool HasDrivePrefix(const std::wstring& path) noexcept {
    return path.size() >= 2 && std::iswalpha(path[0]) != 0 && path[1] == L':';
}

bool QueryFullPath(const std::wstring& path, std::wstring& fullPath) {
    const DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD written = GetFullPathNameW(
        path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (written == 0 || written >= static_cast<DWORD>(buffer.size())) {
        return false;
    }

    fullPath.assign(buffer.data(), written);
    return true;
}

bool QueryCurrentDirectory(std::wstring& directory) {
    const DWORD required = GetCurrentDirectoryW(0, nullptr);
    if (required == 0) {
        return false;
    }

    std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD written = GetCurrentDirectoryW(
        static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0 || written >= static_cast<DWORD>(buffer.size())) {
        return false;
    }

    directory.assign(buffer.data(), written);
    return true;
}

bool QueryDriveDirectory(const wchar_t driveLetter, std::wstring& directory) {
    wchar_t variableName[] = L"=C:";
    variableName[1] = static_cast<wchar_t>(std::towupper(driveLetter));

    const DWORD required = GetEnvironmentVariableW(variableName, nullptr, 0);
    if (required != 0) {
        std::vector<wchar_t> buffer(static_cast<std::size_t>(required) + 1, L'\0');
        const DWORD written = GetEnvironmentVariableW(
            variableName, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written != 0 && written < static_cast<DWORD>(buffer.size())) {
            directory.assign(buffer.data(), written);
            return true;
        }
    }

    directory.assign(1, static_cast<wchar_t>(std::towupper(driveLetter)));
    directory += L":\\";
    return true;
}

void AppendRelativePath(std::wstring& base, const std::wstring& relative) {
    if (!base.empty() && !IsSeparator(base.back())) {
        base.push_back(L'\\');
    }
    base += relative;
}

bool MakeRootedFallback(const std::wstring& path, std::wstring& rootedPath) {
    if (path.rfind(L"\\\\?\\", 0) == 0 ||
        path.rfind(L"\\\\.\\", 0) == 0 ||
        path.rfind(L"\\\\", 0) == 0 ||
        (HasDrivePrefix(path) && path.size() >= 3 && IsSeparator(path[2]))) {
        rootedPath = path;
        return true;
    }

    if (HasDrivePrefix(path)) {
        if (!QueryDriveDirectory(path[0], rootedPath)) {
            return false;
        }
        AppendRelativePath(rootedPath, path.substr(2));
        return true;
    }

    std::wstring currentDirectory;
    if (!QueryCurrentDirectory(currentDirectory)) {
        return false;
    }

    if (!path.empty() && IsSeparator(path[0])) {
        if (!HasDrivePrefix(currentDirectory)) {
            return false;
        }
        rootedPath = currentDirectory.substr(0, 2);
        rootedPath += path;
        return true;
    }

    rootedPath = currentDirectory;
    AppendRelativePath(rootedPath, path);
    return true;
}

}  // namespace

std::wstring PathForWin32Io(const std::wstring& absolutePath) {
    if (absolutePath.size() < static_cast<std::size_t>(MAX_PATH) ||
        absolutePath.rfind(L"\\\\?\\", 0) == 0 ||
        absolutePath.rfind(L"\\\\.\\", 0) == 0) {
        return absolutePath;
    }
    if (absolutePath.rfind(L"\\\\", 0) == 0) {
        return std::wstring(L"\\\\?\\UNC\\") + absolutePath.substr(2);
    }
    if (HasDrivePrefix(absolutePath)) {
        return std::wstring(L"\\\\?\\") + absolutePath;
    }
    return absolutePath;
}

bool MakeAbsolutePath(const std::wstring& path, std::wstring& absolutePath) {
    absolutePath.clear();
    if (path.empty()) {
        return false;
    }

    if (QueryFullPath(path, absolutePath)) {
        return true;
    }

    std::wstring rootedPath;
    if (!MakeRootedFallback(path, rootedPath)) {
        return false;
    }
    return QueryFullPath(PathForWin32Io(rootedPath), absolutePath);
}

MediaPathStatus PrepareMediaFilePath(
    const std::wstring& path,
    std::wstring& displayPath,
    std::wstring& ioPath) {
    displayPath.clear();
    ioPath.clear();
    if (path.empty()) {
        return MediaPathStatus::Empty;
    }

    if (!MakeAbsolutePath(path, displayPath)) {
        return MediaPathStatus::CannotResolve;
    }

    ioPath = PathForWin32Io(displayPath);
    const DWORD attributes = GetFileAttributesW(ioPath.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD pathError = GetLastError();
        return pathError == ERROR_FILE_NOT_FOUND || pathError == ERROR_PATH_NOT_FOUND
            ? MediaPathStatus::NotFound
            : MediaPathStatus::Inaccessible;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return MediaPathStatus::Directory;
    }
    return MediaPathStatus::Ready;
}

}  // namespace videoplayer
