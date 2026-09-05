#include "targetver.h"
#include "MediaOpen.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

namespace videoplayer {
namespace {

constexpr std::size_t kMaximumWindowsCommandLineLength = 32'766;
constexpr std::size_t kMaximumExecutablePathCapacity = 32'768;
constexpr std::size_t kMaximumTransferredPathCharacters = 32'767;
constexpr wchar_t kMediaMapArgumentPrefix[] = L"--media-map=";
constexpr std::uint32_t kMediaMapProtocolMagic = 0x314D4D56;  // "VMM1"
constexpr std::uint32_t kMediaMapProtocolVersion = 1;

struct MediaMapHeader final {
    std::uint32_t magic = kMediaMapProtocolMagic;
    std::uint32_t version = kMediaMapProtocolVersion;
    std::uint32_t characterWidth = sizeof(wchar_t);
    std::uint32_t characterCount = 0;
};

static_assert(sizeof(MediaMapHeader) == 16);
static_assert(sizeof(wchar_t) == 2, "Windows media mapping protocol requires UTF-16 wchar_t");

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    HANDLE Get() const noexcept { return handle_; }
    HANDLE* Put() noexcept {
        Reset();
        return &handle_;
    }

    void Reset(const HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

class MappedView final {
public:
    MappedView() = default;
    explicit MappedView(void* const view) noexcept : view_(view) {}

    ~MappedView() {
        Reset();
    }

    MappedView(const MappedView&) = delete;
    MappedView& operator=(const MappedView&) = delete;

    MappedView(MappedView&& other) noexcept : view_(other.view_) {
        other.view_ = nullptr;
    }

    MappedView& operator=(MappedView&& other) noexcept {
        if (this != &other) {
            Reset();
            view_ = other.view_;
            other.view_ = nullptr;
        }
        return *this;
    }

    void* Get() const noexcept { return view_; }

    void Reset(void* const view = nullptr) noexcept {
        if (view_ != nullptr) {
            UnmapViewOfFile(view_);
        }
        view_ = view;
    }

private:
    void* view_ = nullptr;
};

class ScopedAttributeList final {
public:
    ~ScopedAttributeList() {
        if (list_ != nullptr) {
            DeleteProcThreadAttributeList(list_);
        }
        if (storage_ != nullptr) {
            HeapFree(GetProcessHeap(), 0, storage_);
        }
    }

    ScopedAttributeList(const ScopedAttributeList&) = delete;
    ScopedAttributeList& operator=(const ScopedAttributeList&) = delete;
    ScopedAttributeList() = default;

    bool Initialize(const HANDLE inheritedHandle, DWORD& windowsError) {
        SIZE_T storageSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &storageSize);
        if (storageSize == 0) {
            windowsError = GetLastError();
            return false;
        }

        storage_ = HeapAlloc(GetProcessHeap(), 0, storageSize);
        if (storage_ == nullptr) {
            windowsError = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }

        list_ = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage_);
        if (InitializeProcThreadAttributeList(list_, 1, 0, &storageSize) == FALSE) {
            windowsError = GetLastError();
            list_ = nullptr;
            return false;
        }

        inheritedHandle_ = inheritedHandle;
        if (UpdateProcThreadAttribute(
                list_,
                0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                &inheritedHandle_,
                sizeof(inheritedHandle_),
                nullptr,
                nullptr) == FALSE) {
            windowsError = GetLastError();
            return false;
        }

        windowsError = ERROR_SUCCESS;
        return true;
    }

    PPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept { return list_; }

private:
    void* storage_ = nullptr;
    PPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    HANDLE inheritedHandle_ = nullptr;
};

bool ReadBoundedString(
    const wchar_t*& cursor,
    const wchar_t* const end,
    std::wstring& value) {
    const wchar_t* const terminator = std::find(cursor, end, L'\0');
    if (terminator == end) {
        return false;
    }
    value.assign(cursor, terminator);
    cursor = terminator + 1;
    return true;
}

std::wstring JoinDialogDirectoryAndName(
    const std::wstring& directory,
    const std::wstring& name) {
    if (directory.empty() || name.empty()) {
        return {};
    }

    std::wstring path = directory;
    if (path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    path.append(name);
    return path;
}

void SetLaunchError(
    const wchar_t* const message,
    const DWORD windowsError,
    std::wstring& error) {
    error = message;
    if (windowsError != ERROR_SUCCESS) {
        error.append(L" Код Windows: ");
        error.append(std::to_wstring(windowsError));
        error.push_back(L'.');
    }
}

bool IsMediaMapArgument(const std::wstring_view argument) noexcept {
    return argument.size() >= std::size(kMediaMapArgumentPrefix) - 1 &&
        argument.compare(
            0,
            std::size(kMediaMapArgumentPrefix) - 1,
            kMediaMapArgumentPrefix) == 0;
}

bool ParseHandleValue(
    const std::wstring_view text,
    std::uintptr_t& value) noexcept {
    if (text.empty()) {
        return false;
    }

    std::uintptr_t parsed = 0;
    constexpr std::uintptr_t maximum =
        (std::numeric_limits<std::uintptr_t>::max)();
    for (const wchar_t character : text) {
        if (character < L'0' || character > L'9') {
            return false;
        }
        const std::uintptr_t digit =
            static_cast<std::uintptr_t>(character - L'0');
        if (parsed > (maximum - digit) / 10) {
            return false;
        }
        parsed = parsed * 10 + digit;
    }

    if (parsed == 0 || parsed == maximum) {
        return false;
    }
    value = parsed;
    return true;
}

bool IsValidMediaMapHeader(const MediaMapHeader& header) noexcept {
    return header.magic == kMediaMapProtocolMagic &&
        header.version == kMediaMapProtocolVersion &&
        header.characterWidth == sizeof(wchar_t) &&
        header.characterCount != 0 &&
        header.characterCount <= kMaximumTransferredPathCharacters;
}

bool CurrentExecutablePath(std::wstring& executablePath, std::wstring& error) {
    std::vector<wchar_t> buffer(512, L'\0');
    for (;;) {
        SetLastError(ERROR_SUCCESS);
        const DWORD copied = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            SetLaunchError(
                L"Не вдалося визначити шлях до VideoPlayer.exe.",
                GetLastError(),
                error);
            return false;
        }
        if (static_cast<std::size_t>(copied) < buffer.size()) {
            executablePath.assign(buffer.data(), copied);
            if (executablePath.empty()) {
                SetLaunchError(
                    L"Не вдалося визначити шлях до VideoPlayer.exe.",
                    ERROR_INVALID_NAME,
                    error);
                return false;
            }
            return true;
        }
        if (buffer.size() >= kMaximumExecutablePathCapacity) {
            SetLaunchError(
                L"Шлях до VideoPlayer.exe надто довгий.",
                ERROR_FILENAME_EXCED_RANGE,
                error);
            return false;
        }
        buffer.resize((std::min)(
            buffer.size() * 2,
            kMaximumExecutablePathCapacity));
    }
}

}  // namespace

std::vector<std::wstring> CollectMediaFileArguments(
    const int argc,
    wchar_t* const* const argv) {
    std::vector<std::wstring> paths;
    if (argc <= 1 || argv == nullptr) {
        return paths;
    }

    paths.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && argv[index][0] != L'\0' &&
            !IsMediaMapArgument(argv[index])) {
            paths.emplace_back(argv[index]);
        }
    }
    return paths;
}

std::vector<std::wstring> ParseOpenDialogPaths(
    const wchar_t* const buffer,
    const std::size_t capacity) {
    std::vector<std::wstring> paths;
    if (buffer == nullptr || capacity < 2) {
        return paths;
    }

    const wchar_t* cursor = buffer;
    const wchar_t* const end = buffer + capacity;
    std::wstring first;
    if (!ReadBoundedString(cursor, end, first) || first.empty() || cursor == end) {
        return {};
    }

    if (*cursor == L'\0') {
        paths.emplace_back(std::move(first));
        return paths;
    }

    const std::wstring directory = std::move(first);
    for (;;) {
        std::wstring name;
        if (!ReadBoundedString(cursor, end, name)) {
            return {};
        }
        if (name.empty()) {
            return paths;
        }

        std::wstring path = JoinDialogDirectoryAndName(directory, name);
        if (path.empty()) {
            return {};
        }
        paths.emplace_back(std::move(path));
    }
}

std::wstring QuoteWindowsCommandLineArgument(const std::wstring_view argument) {
    if (argument.find(L'\0') != std::wstring_view::npos) {
        return {};
    }

    std::wstring quoted;
    if (argument.size() > (quoted.max_size() - 2) / 2) {
        return {};
    }
    quoted.reserve(argument.size() * 2 + 2);
    quoted.push_back(L'"');

    std::size_t backslashCount = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashCount;
            continue;
        }

        if (character == L'"') {
            quoted.append(backslashCount * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashCount, L'\\');
            quoted.push_back(character);
        }
        backslashCount = 0;
    }

    quoted.append(backslashCount * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

InheritedMediaPathStatus ReadInheritedMediaPath(
    const int argc,
    wchar_t* const* const argv,
    std::wstring& mediaPath,
    std::wstring& error) {
    mediaPath.clear();
    error.clear();

    std::wstring_view handleText;
    bool found = false;
    if (argc > 1 && argv != nullptr) {
        for (int index = 1; index < argc; ++index) {
            if (argv[index] == nullptr) {
                continue;
            }
            const std::wstring_view argument(argv[index]);
            if (!IsMediaMapArgument(argument)) {
                continue;
            }
            if (found) {
                SetLaunchError(
                    L"Не вдалося відкрити відео: дубльоване службове сховище.",
                    ERROR_INVALID_PARAMETER,
                    error);
                return InheritedMediaPathStatus::Error;
            }
            found = true;
            handleText = argument.substr(std::size(kMediaMapArgumentPrefix) - 1);
        }
    }

    if (!found) {
        return InheritedMediaPathStatus::NotRequested;
    }

    std::uintptr_t handleValue = 0;
    if (!ParseHandleValue(handleText, handleValue)) {
        SetLaunchError(
            L"Не вдалося відкрити відео: некоректне службове сховище.",
            ERROR_INVALID_HANDLE,
            error);
        return InheritedMediaPathStatus::Error;
    }

    UniqueHandle mediaMap(reinterpret_cast<HANDLE>(handleValue));
    MappedView headerView(MapViewOfFile(
        mediaMap.Get(),
        FILE_MAP_READ,
        0,
        0,
        sizeof(MediaMapHeader)));
    if (headerView.Get() == nullptr) {
        SetLaunchError(
            L"Не вдалося відкрити відео: службове сховище недоступне.",
            GetLastError(),
            error);
        return InheritedMediaPathStatus::Error;
    }

    MediaMapHeader header{};
    std::memcpy(&header, headerView.Get(), sizeof(header));
    if (!IsValidMediaMapHeader(header)) {
        SetLaunchError(
            L"Не вдалося відкрити відео: службові дані пошкоджені.",
            ERROR_INVALID_DATA,
            error);
        return InheritedMediaPathStatus::Error;
    }

    const std::size_t pathBytes =
        static_cast<std::size_t>(header.characterCount) * sizeof(wchar_t);
    const std::size_t mappingBytes = sizeof(MediaMapHeader) + pathBytes;
    headerView.Reset();

    MappedView completeView(MapViewOfFile(
        mediaMap.Get(), FILE_MAP_READ, 0, 0, mappingBytes));
    if (completeView.Get() == nullptr) {
        SetLaunchError(
            L"Не вдалося повністю отримати шлях до відео зі службового сховища.",
            GetLastError(),
            error);
        return InheritedMediaPathStatus::Error;
    }

    MediaMapHeader confirmedHeader{};
    std::memcpy(&confirmedHeader, completeView.Get(), sizeof(confirmedHeader));
    if (!IsValidMediaMapHeader(confirmedHeader) ||
        std::memcmp(&confirmedHeader, &header, sizeof(header)) != 0) {
        SetLaunchError(
            L"Не вдалося відкрити відео: службові дані змінено або пошкоджено.",
            ERROR_INVALID_DATA,
            error);
        return InheritedMediaPathStatus::Error;
    }

    std::wstring receivedPath(header.characterCount, L'\0');
    const auto* const mappedBytes =
        static_cast<const unsigned char*>(completeView.Get());
    std::memcpy(
        receivedPath.data(),
        mappedBytes + sizeof(MediaMapHeader),
        pathBytes);
    if (receivedPath.find(L'\0') != std::wstring::npos) {
        SetLaunchError(
            L"Не вдалося відкрити відео: шлях у службових даних некоректний.",
            ERROR_INVALID_DATA,
            error);
        return InheritedMediaPathStatus::Error;
    }

    mediaPath = std::move(receivedPath);
    return InheritedMediaPathStatus::Ready;
}

bool LaunchMediaInNewInstance(
    const std::wstring& mediaPath,
    std::wstring& error) {
    error.clear();
    if (mediaPath.empty() || mediaPath.find(L'\0') != std::wstring::npos ||
        mediaPath.size() > kMaximumTransferredPathCharacters) {
        SetLaunchError(
            L"Не вдалося запустити окреме вікно: шлях до відео некоректний.",
            ERROR_INVALID_PARAMETER,
            error);
        return false;
    }

    std::wstring executablePath;
    if (!CurrentExecutablePath(executablePath, error)) {
        return false;
    }

    const std::wstring quotedExecutable =
        QuoteWindowsCommandLineArgument(executablePath);
    if (quotedExecutable.empty()) {
        SetLaunchError(
            L"Не вдалося запустити окреме вікно: командний рядок надто довгий.",
            ERROR_FILENAME_EXCED_RANGE,
            error);
        return false;
    }

    const std::size_t pathBytes = mediaPath.size() * sizeof(wchar_t);
    const std::size_t mappingBytes = sizeof(MediaMapHeader) + pathBytes;
    SECURITY_ATTRIBUTES mapSecurity{};
    mapSecurity.nLength = sizeof(mapSecurity);
    mapSecurity.bInheritHandle = TRUE;

    UniqueHandle mediaMap(CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &mapSecurity,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(mappingBytes),
        nullptr));
    if (mediaMap.Get() == nullptr) {
        SetLaunchError(
            L"Не вдалося створити захищене службове сховище для нового вікна.",
            GetLastError(),
            error);
        return false;
    }

    MappedView writableView(MapViewOfFile(
        mediaMap.Get(), FILE_MAP_WRITE, 0, 0, mappingBytes));
    if (writableView.Get() == nullptr) {
        SetLaunchError(
            L"Не вдалося підготувати службове сховище для нового вікна.",
            GetLastError(),
            error);
        return false;
    }

    MediaMapHeader header{};
    header.characterCount = static_cast<std::uint32_t>(mediaPath.size());
    std::memcpy(writableView.Get(), &header, sizeof(header));
    auto* const mappedBytes = static_cast<unsigned char*>(writableView.Get());
    std::memcpy(
        mappedBytes + sizeof(MediaMapHeader), mediaPath.data(), pathBytes);
    writableView.Reset();

    ScopedAttributeList attributes;
    DWORD windowsError = ERROR_SUCCESS;
    if (!attributes.Initialize(mediaMap.Get(), windowsError)) {
        SetLaunchError(
            L"Не вдалося обмежити успадкування системних ресурсів.",
            windowsError,
            error);
        return false;
    }

    const std::uintptr_t mapHandleValue =
        reinterpret_cast<std::uintptr_t>(mediaMap.Get());
    std::wstring commandLine = quotedExecutable;
    commandLine.append(L" ");
    commandLine.append(kMediaMapArgumentPrefix);
    commandLine.append(std::to_wstring(
        static_cast<unsigned long long>(mapHandleValue)));
    if (commandLine.size() >= kMaximumWindowsCommandLineLength) {
        SetLaunchError(
            L"Не вдалося запустити окреме вікно: командний рядок надто довгий.",
            ERROR_FILENAME_EXCED_RANGE,
            error);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.Get();
    UniqueHandle processHandle;
    UniqueHandle threadHandle;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            executablePath.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process) == FALSE) {
        SetLaunchError(
            L"Не вдалося запустити окреме вікно VideoPlayer.",
            GetLastError(),
            error);
        return false;
    }
    processHandle.Reset(process.hProcess);
    threadHandle.Reset(process.hThread);
    return true;
}

}  // namespace videoplayer
