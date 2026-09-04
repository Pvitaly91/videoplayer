#include "LibVlcRuntime.h"

#include <memory>
#include <string_view>
#include <vector>

namespace videoplayer {
namespace {

std::wstring ToExtendedPath(const std::wstring& path) {
    if (path.rfind(LR"(\\?\)", 0) == 0 || path.rfind(LR"(\\.\)", 0) == 0 ||
        path.size() < 248) {
        return path;
    }
    if (path.rfind(LR"(\\)", 0) == 0) {
        return LR"(\\?\UNC\)" + path.substr(2);
    }
    return LR"(\\?\)" + path;
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring_view name) {
    if (directory.empty()) {
        return std::wstring(name);
    }
    if (directory.back() == L'\\' || directory.back() == L'/') {
        return directory + std::wstring(name);
    }
    return directory + L'\\' + std::wstring(name);
}

bool GetExecutableDirectory(std::wstring& directory, std::wstring& error) {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD copied =
            ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            error = L"GetModuleFileNameW failed.";
            return false;
        }
        if (static_cast<std::size_t>(copied) < buffer.size()) {
            const std::wstring executable(buffer.data(), copied);
            const std::size_t separator = executable.find_last_of(L"\\/");
            if (separator == std::wstring::npos) {
                error = L"The executable path has no directory.";
                return false;
            }
            directory.assign(executable, 0, separator);
            return true;
        }
        if (buffer.size() >= 32768) {
            error = L"The executable path is too long.";
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool IsRegularFile(const std::wstring& path) {
    const std::wstring extended = ToExtendedPath(path);
    const DWORD attributes = ::GetFileAttributesW(extended.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool IsNonEmptyDirectory(const std::wstring& path) {
    const std::wstring extended = ToExtendedPath(path);
    const DWORD attributes = ::GetFileAttributesW(extended.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return false;
    }

    const std::wstring pattern = JoinPath(extended, L"*");
    WIN32_FIND_DATAW entry{};
    const HANDLE find = ::FindFirstFileW(pattern.c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool hasEntry = false;
    do {
        if (std::wstring_view(entry.cFileName) != L"." &&
            std::wstring_view(entry.cFileName) != L"..") {
            hasEntry = true;
            break;
        }
    } while (::FindNextFileW(find, &entry));

    ::FindClose(find);
    return hasEntry;
}

std::wstring ExportName(const char* name) {
    std::wstring result;
    while (*name != '\0') {
        result.push_back(static_cast<unsigned char>(*name));
        ++name;
    }
    return result;
}

template <typename Function>
bool Resolve(
    const HMODULE module,
    const char* name,
    Function& destination,
    std::wstring& error) {
    const FARPROC address = ::GetProcAddress(module, name);
    if (address == nullptr) {
        error = L"Missing LibVLC export: " + ExportName(name);
        return false;
    }
    destination = reinterpret_cast<Function>(address);
    return true;
}

std::wstring LoaderError(const std::wstring_view prefix) {
    return std::wstring(prefix) + L" (Win32 error " +
           std::to_wstring(::GetLastError()) + L").";
}

bool ClearExternalPluginPath(std::wstring& error) {
    ::SetLastError(ERROR_SUCCESS);
    if (::SetEnvironmentVariableW(L"VLC_PLUGIN_PATH", nullptr) == FALSE &&
        ::GetLastError() != ERROR_ENVVAR_NOT_FOUND) {
        error = LoaderError(L"Could not clear an external VLC_PLUGIN_PATH");
        return false;
    }

    // Official VLC 3 Windows builds use msvcrt's narrow getenv(). If that CRT
    // was already initialized by another DLL, also clear its cached copy.
    const HMODULE msvcrt = ::GetModuleHandleW(L"msvcrt.dll");
    if (msvcrt != nullptr) {
        using PutEnv = int(__cdecl*)(const char*);
        const auto putEnv = reinterpret_cast<PutEnv>(
            ::GetProcAddress(msvcrt, "_putenv"));
        if (putEnv != nullptr && putEnv("VLC_PLUGIN_PATH=") != 0) {
            error = L"Could not clear the CRT VLC_PLUGIN_PATH override.";
            return false;
        }
    }
    return true;
}

struct InstanceReleaser final {
    LibVlcRelease release = nullptr;

    void operator()(libvlc_instance_t* instance) const noexcept {
        if (instance != nullptr && release != nullptr) {
            release(instance);
        }
    }
};

struct PlayerReleaser final {
    LibVlcMediaPlayerRelease release = nullptr;

    void operator()(libvlc_media_player_t* player) const noexcept {
        if (player != nullptr && release != nullptr) {
            release(player);
        }
    }
};

}  // namespace

LibVlcRuntime::~LibVlcRuntime() {
    Unload();
}

bool LibVlcRuntime::Load(std::wstring& error) noexcept {
    error.clear();
    if (IsLoaded()) {
        return true;
    }

    try {
        std::wstring directory;
        if (!GetExecutableDirectory(directory, error)) {
            return false;
        }

        const std::wstring corePath = JoinPath(directory, L"libvlccore.dll");
        const std::wstring vlcPath = JoinPath(directory, L"libvlc.dll");
        const std::wstring pluginsPath = JoinPath(directory, L"plugins");

        if (!ClearExternalPluginPath(error)) {
            return false;
        }

        if (!IsRegularFile(corePath)) {
            error = L"Missing libvlccore.dll beside the executable.";
            return false;
        }
        if (!IsRegularFile(vlcPath)) {
            error = L"Missing libvlc.dll beside the executable.";
            return false;
        }
        if (!IsNonEmptyDirectory(pluginsPath)) {
            error = L"Missing or empty plugins directory beside the executable.";
            return false;
        }

        const std::wstring extendedCorePath = ToExtendedPath(corePath);
        coreModule_ =
            ::LoadLibraryExW(extendedCorePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (coreModule_ == nullptr) {
            error = LoaderError(L"Could not load libvlccore.dll");
            Unload();
            return false;
        }

        const std::wstring extendedVlcPath = ToExtendedPath(vlcPath);
        vlcModule_ =
            ::LoadLibraryExW(extendedVlcPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (vlcModule_ == nullptr) {
            error = LoaderError(L"Could not load libvlc.dll");
            Unload();
            return false;
        }

        // Synchronize msvcrt's cached environment after its first possible
        // load, before libvlc_new() can enumerate plug-ins.
        if (!ClearExternalPluginPath(error)) {
            Unload();
            return false;
        }

#define RESOLVE(member, symbol)                                      \
    if (!Resolve(vlcModule_, #symbol, api_.member, error)) {          \
        Unload();                                                     \
        return false;                                                 \
    }

        RESOLVE(newInstance, libvlc_new)
        RESOLVE(releaseInstance, libvlc_release)
        RESOLVE(getVersion, libvlc_get_version)
        RESOLVE(errorMessage, libvlc_errmsg)
        RESOLVE(mediaNewPath, libvlc_media_new_path)
        RESOLVE(mediaNewCallbacks, libvlc_media_new_callbacks)
        RESOLVE(mediaRelease, libvlc_media_release)
        RESOLVE(mediaPlayerNew, libvlc_media_player_new)
        RESOLVE(mediaPlayerRelease, libvlc_media_player_release)
        RESOLVE(mediaPlayerSetMedia, libvlc_media_player_set_media)
        RESOLVE(mediaPlayerSetHwnd, libvlc_media_player_set_hwnd)
        RESOLVE(videoSetKeyInput, libvlc_video_set_key_input)
        RESOLVE(videoSetMouseInput, libvlc_video_set_mouse_input)
        RESOLVE(mediaPlayerPlay, libvlc_media_player_play)
        RESOLVE(mediaPlayerSetPause, libvlc_media_player_set_pause)
        RESOLVE(mediaPlayerStop, libvlc_media_player_stop)
        RESOLVE(mediaPlayerGetTime, libvlc_media_player_get_time)
        RESOLVE(mediaPlayerSetTime, libvlc_media_player_set_time)
        RESOLVE(mediaPlayerGetLength, libvlc_media_player_get_length)
        RESOLVE(mediaPlayerIsSeekable, libvlc_media_player_is_seekable)
        RESOLVE(mediaPlayerEventManager, libvlc_media_player_event_manager)
        RESOLVE(audioSetVolume, libvlc_audio_set_volume)
        RESOLVE(audioSetMute, libvlc_audio_set_mute)
        RESOLVE(eventAttach, libvlc_event_attach)
        RESOLVE(eventDetach, libvlc_event_detach)

#undef RESOLVE
        return true;
    } catch (...) {
        Unload();
        try {
            error = L"Unexpected error while loading the LibVLC runtime.";
        } catch (...) {
        }
        return false;
    }
}

void LibVlcRuntime::Unload() noexcept {
    api_ = {};
    if (vlcModule_ != nullptr) {
        ::FreeLibrary(vlcModule_);
        vlcModule_ = nullptr;
    }
    if (coreModule_ != nullptr) {
        ::FreeLibrary(coreModule_);
        coreModule_ = nullptr;
    }
}

bool LibVlcRuntime::IsLoaded() const noexcept {
    return vlcModule_ != nullptr;
}

const LibVlcRuntime::Api& LibVlcRuntime::Functions() const noexcept {
    return api_;
}

int RunLibVlcSelfTest(std::wstring& error) noexcept {
    error.clear();
    try {
        LibVlcRuntime runtime;
        if (!runtime.Load(error)) {
            return 1;
        }

        const LibVlcRuntime::Api& api = runtime.Functions();
        const char* const arguments[] = {"--no-video-title-show"};
        std::unique_ptr<libvlc_instance_t, InstanceReleaser> instance(
            api.newInstance(1, arguments),
            InstanceReleaser{api.releaseInstance});
        if (!instance) {
            error = L"libvlc_new failed.";
            return 2;
        }

        const char* version = api.getVersion();
        if (version == nullptr || *version == '\0') {
            error = L"libvlc_get_version returned an empty value.";
            return 3;
        }

        std::unique_ptr<libvlc_media_player_t, PlayerReleaser> player(
            api.mediaPlayerNew(instance.get()),
            PlayerReleaser{api.mediaPlayerRelease});
        if (!player) {
            error = L"libvlc_media_player_new failed.";
            return 4;
        }
        return 0;
    } catch (...) {
        try {
            error = L"Unexpected exception during the LibVLC self-test.";
        } catch (...) {
        }
        return 100;
    }
}

}  // namespace videoplayer
