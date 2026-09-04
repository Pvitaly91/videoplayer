#include "targetver.h"
#include "App.h"

#include "LibVlcRuntime.h"
#include "MainWindow.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <cwchar>
#include <string>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

namespace videoplayer {
namespace {

constexpr wchar_t kAppTitle[] = L"VideoPlayer";

void ShowStartupError(const std::wstring& error) {
    const wchar_t* const text = error.empty()
        ? L"Не вдалося запустити VideoPlayer."
        : error.c_str();
    MessageBoxW(nullptr, text, kAppTitle, MB_OK | MB_ICONERROR);
}

class CommandLineArguments final {
public:
    CommandLineArguments()
        : values_(CommandLineToArgvW(GetCommandLineW(), &count_)) {}

    ~CommandLineArguments() {
        if (values_ != nullptr) {
            LocalFree(values_);
        }
    }

    CommandLineArguments(const CommandLineArguments&) = delete;
    CommandLineArguments& operator=(const CommandLineArguments&) = delete;

    bool IsValid() const noexcept { return values_ != nullptr; }
    int Count() const noexcept { return count_; }
    wchar_t* const* Values() const noexcept { return values_; }

private:
    int count_ = 0;
    wchar_t** values_ = nullptr;
};

class ComApartment final {
public:
    bool Initialize(std::wstring& error) {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(result)) {
            initialized_ = true;
            return true;
        }
        if (result == RPC_E_CHANGED_MODE) {
            return true;
        }
        error = L"Не вдалося ініціалізувати системні компоненти Windows.";
        return false;
    }

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment() = default;

private:
    bool initialized_ = false;
};

}  // namespace

int App::Run(const HINSTANCE instance, PWSTR commandLine, const int showCommand) {
    UNREFERENCED_PARAMETER(commandLine);

    CommandLineArguments arguments;
    if (!arguments.IsValid()) {
        return 2;
    }

    const bool selfTest = arguments.Count() >= 2 &&
        arguments.Values()[1] != nullptr &&
        _wcsicmp(arguments.Values()[1], L"--self-test") == 0;
    if (selfTest) {
        std::wstring ignoredError;
        return RunLibVlcSelfTest(ignoredError);
    }

    ComApartment com;
    std::wstring error;
    if (!com.Initialize(error)) {
        ShowStartupError(error);
        return 3;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    if (InitCommonControlsEx(&commonControls) == FALSE) {
        ShowStartupError(L"Не вдалося ініціалізувати елементи керування Windows.");
        return 4;
    }

    MainWindow window;
    if (!window.Create(instance, error)) {
        ShowStartupError(error);
        return 5;
    }

    window.Show(showCommand);
    const std::wstring initialFile = FirstFileArgument(arguments.Count(), arguments.Values());
    if (!initialFile.empty()) {
        window.OpenFile(initialFile);
    }

    return window.RunMessageLoop();
}

}  // namespace videoplayer

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE previousInstance,
    PWSTR commandLine,
    int showCommand) {
    UNREFERENCED_PARAMETER(previousInstance);
    videoplayer::App app;
    return app.Run(instance, commandLine, showCommand);
}
