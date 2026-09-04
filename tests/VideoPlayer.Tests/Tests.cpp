#include "../../src/VideoPlayer/App.h"
#include "../../src/VideoPlayer/PathUtils.h"
#include "../../src/VideoPlayer/PlaybackMath.h"
#include "../../src/VideoPlayer/TimeFormatter.h"
#include "../../src/VideoPlayer/Utf8.h"

#include <Windows.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

class TestRunner final {
public:
    template <typename Actual, typename Expected>
    void Equal(const Actual& actual, const Expected& expected, std::wstring_view name)
    {
        if (actual == expected) {
            ++passed_;
            return;
        }

        ++failed_;
        std::wcerr << L"FAIL: " << name << L'\n';
    }

    void True(bool value, std::wstring_view name)
    {
        Equal(value, true, name);
    }

    int Finish() const
    {
        std::wcout << L"Passed: " << passed_ << L", failed: " << failed_ << L'\n';
        return failed_ == 0 ? 0 : 1;
    }

private:
    int passed_ = 0;
    int failed_ = 0;
};

void TestTimeFormatting(TestRunner& runner)
{
    using videoplayer::FormatTime;

    runner.Equal(FormatTime(0), std::wstring(L"00:00"), L"00:00");
    runner.Equal(FormatTime(-1), std::wstring(L"00:00"), L"negative time is clamped");
    runner.Equal(FormatTime(65'000), std::wstring(L"01:05"), L"MM:SS");
    runner.Equal(FormatTime(3'661'000), std::wstring(L"1:01:01"), L"H:MM:SS");
}

void TestPlaybackMath(TestRunner& runner)
{
    using videoplayer::ClampTime;
    using videoplayer::ClampVolume;
    using videoplayer::SeekBy;

    runner.Equal(ClampTime(-100, 60'000), std::int64_t{0}, L"clamp time lower bound");
    runner.Equal(ClampTime(70'000, 60'000), std::int64_t{60'000}, L"clamp time upper bound");
    runner.Equal(SeekBy(20'000, -5'000, 60'000), std::int64_t{15'000}, L"seek -5 seconds");
    runner.Equal(SeekBy(20'000, 5'000, 60'000), std::int64_t{25'000}, L"seek +5 seconds");
    runner.Equal(SeekBy(20'000, -30'000, 60'000), std::int64_t{0}, L"seek -30 seconds clamps");
    runner.Equal(SeekBy(40'000, 30'000, 60'000), std::int64_t{60'000}, L"seek +30 seconds clamps");
    runner.Equal(ClampVolume(-1), 0, L"volume lower bound");
    runner.Equal(ClampVolume(55), 55, L"volume unchanged in range");
    runner.Equal(ClampVolume(101), 100, L"volume upper bound");
}

void TestUtf8(TestRunner& runner)
{
    std::string utf8;
    runner.True(
        videoplayer::WideToUtf8(LR"(C:\Відео\український файл.mp4)", utf8),
        L"UTF-16 Ukrainian path conversion succeeds");
    runner.Equal(
        utf8,
        std::string(u8R"(C:\Відео\український файл.mp4)"),
        L"UTF-16 Ukrainian path conversion is exact");

    runner.True(
        videoplayer::WideToUtf8(LR"(C:\Media Files\sample video.mkv)", utf8),
        L"path with spaces conversion succeeds");
    runner.Equal(
        utf8,
        std::string(u8R"(C:\Media Files\sample video.mkv)"),
        L"path with spaces conversion is exact");
}

void TestCommandLineArgument(TestRunner& runner)
{
    wchar_t executable[] = L"VideoPlayer.exe";
    wchar_t filePath[] = LR"(C:\Відео з пробілами\фільм.mkv)";
    wchar_t* arguments[] = {executable, filePath};

    runner.Equal(
        videoplayer::FirstFileArgument(2, arguments),
        std::wstring(arguments[1]),
        L"first command-line file argument");
    runner.True(
        videoplayer::FirstFileArgument(1, arguments).empty(),
        L"missing command-line file argument");
}

void TestLongPaths(TestRunner& runner)
{
    const std::wstring longComponent(280, L'a');
    const std::wstring localPath = L"C:\\" + longComponent + L"\\video.mkv";
    const std::wstring localIoPath = videoplayer::PathForWin32Io(localPath);
    runner.True(
        localIoPath.rfind(L"\\\\?\\C:\\", 0) == 0,
        L"long local path receives extended prefix");

    const std::wstring uncPath =
        L"\\\\server\\share\\" + longComponent + L"\\video.mkv";
    const std::wstring uncIoPath = videoplayer::PathForWin32Io(uncPath);
    runner.True(
        uncIoPath.rfind(L"\\\\?\\UNC\\server\\share\\", 0) == 0,
        L"long UNC path receives extended prefix");

    runner.Equal(
        videoplayer::PathForWin32Io(LR"(C:\Media\video.mkv)"),
        std::wstring(LR"(C:\Media\video.mkv)"),
        L"short path remains unchanged");

    std::wstring absolutePath;
    runner.True(
        videoplayer::MakeAbsolutePath(localPath, absolutePath),
        L"long absolute path resolves without file access");
    runner.True(
        videoplayer::PathForWin32Io(absolutePath).rfind(L"\\\\?\\", 0) == 0,
        L"resolved long path remains usable by Win32 I/O");
}

void TestFileExistence(TestRunner& runner)
{
    wchar_t temporaryDirectory[MAX_PATH]{};
    const DWORD directoryLength = GetTempPathW(MAX_PATH, temporaryDirectory);
    runner.True(
        directoryLength > 0 && directoryLength < MAX_PATH,
        L"temporary directory is available");
    if (directoryLength == 0 || directoryLength >= MAX_PATH) {
        return;
    }

    wchar_t temporaryFile[MAX_PATH]{};
    const UINT created = GetTempFileNameW(temporaryDirectory, L"VPT", 0, temporaryFile);
    runner.True(created != 0, L"temporary file is created");
    if (created == 0) {
        return;
    }

    std::wstring displayPath;
    std::wstring ioPath;
    runner.Equal(
        videoplayer::PrepareMediaFilePath(temporaryFile, displayPath, ioPath),
        videoplayer::MediaPathStatus::Ready,
        L"production path validation accepts an existing file");
    runner.True(!displayPath.empty() && !ioPath.empty(), L"validated paths are returned");
    DeleteFileW(temporaryFile);
    runner.Equal(
        videoplayer::PrepareMediaFilePath(temporaryFile, displayPath, ioPath),
        videoplayer::MediaPathStatus::NotFound,
        L"production path validation rejects a nonexistent file");
    runner.Equal(
        videoplayer::PrepareMediaFilePath(temporaryDirectory, displayPath, ioPath),
        videoplayer::MediaPathStatus::Directory,
        L"production path validation rejects a directory");
}

} // namespace

int wmain()
{
    TestRunner runner;
    TestTimeFormatting(runner);
    TestPlaybackMath(runner);
    TestUtf8(runner);
    TestCommandLineArgument(runner);
    TestLongPaths(runner);
    TestFileExistence(runner);
    return runner.Finish();
}
