#include "../../src/VideoPlayer/App.h"
#include "../../src/VideoPlayer/MediaOpen.h"
#include "../../src/VideoPlayer/PathUtils.h"
#include "../../src/VideoPlayer/PlaybackMath.h"
#include "../../src/VideoPlayer/PreviewCache.h"
#include "../../src/VideoPlayer/PreviewMath.h"
#include "../../src/VideoPlayer/ProgressStrip.h"
#include "../../src/VideoPlayer/PrivacyPolicy.h"
#include "../../src/VideoPlayer/TimeFormatter.h"
#include "../../src/VideoPlayer/ToolbarVisibility.h"
#include "../../src/VideoPlayer/Utf8.h"
#include "../../src/VideoPlayer/VideoGeometry.h"
#include "../../src/VideoPlayer/ZoomState.h"
#include "PlaybackSnapshotTests.h"
#include "PreviewLifecycleTests.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

void RunUiLifecycleTests(int& passed, int& failed);

namespace {

constexpr wchar_t kMediaOpenIntegrationPrefix[] =
    L"VideoPlayer.Tests.SharedMemory:";
constexpr wchar_t kMediaOpenIntegrationSensitivePath[] =
    LR"(C:\Media Files\приватне відео з пробілами\тест.mkv)";

std::wstring MediaOpenIntegrationObjectName(
    const wchar_t* const role,
    const std::wstring_view identifier) {
    return std::wstring(L"Local\\VideoPlayer.Tests.MediaOpen.") +
        role + L"." + std::wstring(identifier);
}

std::wstring BuildMediaOpenIntegrationPayload(
    const std::wstring_view identifier) {
    return std::wstring(kMediaOpenIntegrationPrefix) +
        std::wstring(identifier) + L"|" +
        kMediaOpenIntegrationSensitivePath;
}

int RunMediaOpenIntegrationChild(const std::wstring_view payload) {
    const std::size_t prefixLength =
        std::size(kMediaOpenIntegrationPrefix) - 1;
    const std::size_t separator = payload.find(L'|', prefixLength);
    if (separator == std::wstring_view::npos ||
        payload.substr(0, prefixLength) != kMediaOpenIntegrationPrefix) {
        return 80;
    }

    const std::wstring_view identifier = payload.substr(
        prefixLength, separator - prefixLength);
    const bool exactPayload =
        payload == BuildMediaOpenIntegrationPayload(identifier);
    const bool absentFromCommandLine =
        std::wstring_view(GetCommandLineW()).find(
            kMediaOpenIntegrationSensitivePath) == std::wstring_view::npos;

    const HANDLE done = OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        MediaOpenIntegrationObjectName(L"Done", identifier).c_str());
    const HANDLE success = OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        MediaOpenIntegrationObjectName(L"Success", identifier).c_str());
    if (done == nullptr || success == nullptr) {
        if (done != nullptr) {
            CloseHandle(done);
        }
        if (success != nullptr) {
            CloseHandle(success);
        }
        return 81;
    }

    if (exactPayload && absentFromCommandLine) {
        SetEvent(success);
    }
    SetEvent(done);
    CloseHandle(done);
    CloseHandle(success);
    return exactPayload && absentFromCommandLine ? 0 : 82;
}

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

    void Near(double actual, double expected, double tolerance, std::wstring_view name)
    {
        True(std::abs(actual - expected) <= tolerance, name);
    }

    int Finish() const
    {
        std::wcout << L"Passed: " << passed_ << L", failed: " << failed_ << L'\n';
        return failed_ == 0 ? 0 : 1;
    }

    void AddCounts(const int passed, const int failed) {
        passed_ += passed;
        failed_ += failed;
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

void TestProgressStrip(TestRunner& runner)
{
    using videoplayer::EvaluateProgressStrip;
    using videoplayer::ProgressStripInput;

    ProgressStripInput input{};
    input.fullscreen = true;
    input.toolbarVisible = false;
    input.hasMedia = true;
    input.availableWidth = 1920;
    input.positionMs = 30'000;
    input.durationMs = 60'000;

    auto decision = EvaluateProgressStrip(input);
    runner.True(decision.visible, L"fullscreen hidden-toolbar progress strip is visible");
    runner.Equal(decision.completedWidth, 960, L"progress strip maps midpoint to half width");

    input.fullscreen = false;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"windowed mode hides compact progress strip");
    runner.Equal(decision.completedWidth, 0, L"hidden windowed strip has no completed width");

    input.fullscreen = true;
    input.toolbarVisible = true;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"visible toolbar hides compact progress strip");

    input.toolbarVisible = false;
    input.hasMedia = false;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"no media hides compact progress strip");

    input.hasMedia = true;
    input.availableWidth = 0;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"zero available width hides compact progress strip");

    input.availableWidth = -1;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"negative available width hides compact progress strip");

    input.availableWidth = 1920;
    input.durationMs = 0;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"unknown zero duration hides compact progress strip");

    input.durationMs = -1;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"negative duration hides compact progress strip");

    input.durationMs = 60'000;
    input.positionMs = -1;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"non-positive progress has no completed strip to show");
    runner.Equal(decision.completedWidth, 0, L"negative progress clamps to zero width");

    input.positionMs = 1;
    input.durationMs = (std::numeric_limits<std::int64_t>::max)();
    decision = EvaluateProgressStrip(input);
    runner.Equal(decision.completedWidth, 1, L"positive progress receives a one-pixel minimum");

    input.availableWidth = 1001;
    input.positionMs = 1;
    input.durationMs = 3;
    decision = EvaluateProgressStrip(input);
    runner.Equal(decision.completedWidth, 333, L"progress strip uses exact floor scaling");

    input.positionMs = 2;
    decision = EvaluateProgressStrip(input);
    runner.Equal(decision.completedWidth, 667, L"progress strip preserves fractional scaling");

    input.availableWidth = 1920;
    input.positionMs = 60'000;
    input.durationMs = 60'000;
    decision = EvaluateProgressStrip(input);
    runner.Equal(decision.completedWidth, 1920, L"ended playback fills the progress strip");

    input.positionMs = 90'000;
    decision = EvaluateProgressStrip(input);
    runner.Equal(decision.completedWidth, 1920, L"over-reported playback clamps to full width");

    input.availableWidth = (std::numeric_limits<int>::max)();
    input.positionMs = (std::numeric_limits<std::int64_t>::max)() - 1;
    input.durationMs = (std::numeric_limits<std::int64_t>::max)();
    decision = EvaluateProgressStrip(input);
    runner.Equal(
        decision.completedWidth,
        (std::numeric_limits<int>::max)() - 1,
        L"extreme duration scaling is exact and overflow-safe");
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

void TestMediaOpen(TestRunner& runner)
{
    wchar_t executable[] = L"VideoPlayer.exe";
    wchar_t firstPath[] = LR"(C:\Відео з пробілами\фільм.mkv)";
    wchar_t secondPath[] = LR"(D:\Media\друге відео.mp4)";
    wchar_t internalMap[] = L"--media-map=12345";
    wchar_t empty[] = L"";
    wchar_t* arguments[] = {
        executable, firstPath, nullptr, empty, internalMap, secondPath};
    const std::vector<std::wstring> files =
        videoplayer::CollectMediaFileArguments(6, arguments);
    runner.Equal(files.size(), std::size_t{2}, L"all non-empty command-line files collected");
    runner.Equal(
        files[0],
        std::wstring(firstPath),
        L"first command-line video is preserved");
    runner.Equal(
        files[1],
        std::wstring(secondPath),
        L"additional command-line video is preserved");
    runner.True(
        videoplayer::CollectMediaFileArguments(1, arguments).empty(),
        L"missing command-line media arguments");

    std::wstring inheritedPath = L"stale path";
    std::wstring inheritedError = L"stale error";
    runner.Equal(
        videoplayer::ReadInheritedMediaPath(
            1, arguments, inheritedPath, inheritedError),
        videoplayer::InheritedMediaPathStatus::NotRequested,
        L"ordinary command line does not request inherited media");
    runner.True(
        inheritedPath.empty() && inheritedError.empty(),
        L"inherited media outputs are cleared when not requested");

    wchar_t malformedMap[] =
        L"--media-map=184467440737095516160000";
    wchar_t* malformedArguments[] = {executable, malformedMap};
    runner.Equal(
        videoplayer::ReadInheritedMediaPath(
            2, malformedArguments, inheritedPath, inheritedError),
        videoplayer::InheritedMediaPathStatus::Error,
        L"overflowing inherited mapping handle is rejected");
    runner.True(
        inheritedPath.empty() && !inheritedError.empty(),
        L"invalid inherited mapping reports an error without a path");

    wchar_t duplicateMap[] = L"--media-map=67890";
    wchar_t* duplicateArguments[] = {
        executable, internalMap, duplicateMap};
    runner.Equal(
        videoplayer::ReadInheritedMediaPath(
            3, duplicateArguments, inheritedPath, inheritedError),
        videoplayer::InheritedMediaPathStatus::Error,
        L"duplicate inherited mapping arguments are rejected");

    const wchar_t singleSelection[] =
        L"C:\\Відео з пробілами\\один.mp4\0";
    runner.Equal(
        videoplayer::ParseOpenDialogPaths(
            singleSelection, std::size(singleSelection)),
        std::vector<std::wstring>{LR"(C:\Відео з пробілами\один.mp4)"},
        L"single Open dialog selection parses as one full path");

    const wchar_t multipleSelection[] =
        L"D:\\Media\0one.mp4\0два відео.mkv\0";
    runner.Equal(
        videoplayer::ParseOpenDialogPaths(
            multipleSelection, std::size(multipleSelection)),
        std::vector<std::wstring>{
            LR"(D:\Media\one.mp4)",
            LR"(D:\Media\два відео.mkv)"},
        L"Explorer multi-select buffer expands every file path");

    const wchar_t malformedSelection[] = {L'C', L':'};
    runner.True(
        videoplayer::ParseOpenDialogPaths(
            malformedSelection, std::size(malformedSelection)).empty(),
        L"unterminated Open dialog buffer is rejected");

    runner.Equal(
        videoplayer::QuoteWindowsCommandLineArgument(
            LR"(C:\Media Files\clip.mkv)"),
        std::wstring(L"\"C:\\Media Files\\clip.mkv\""),
        L"instance command-line path with spaces is quoted");
    runner.Equal(
        videoplayer::QuoteWindowsCommandLineArgument(
            LR"(C:\Folder With Space\)"),
        std::wstring(L"\"C:\\Folder With Space\\\\\""),
        L"trailing backslash is escaped before closing quote");
    runner.Equal(
        videoplayer::QuoteWindowsCommandLineArgument(L"a\"b"),
        std::wstring(L"\"a\\\"b\""),
        L"embedded quote follows CommandLineToArgvW escaping");
    runner.Equal(
        videoplayer::QuoteWindowsCommandLineArgument(L""),
        std::wstring(L"\"\""),
        L"empty command-line argument remains representable");
    runner.True(
        videoplayer::QuoteWindowsCommandLineArgument(
            std::wstring_view(L"a\0b", 3)).empty(),
        L"embedded null command-line argument is rejected");
}

void TestMediaOpenLaunchIntegration(TestRunner& runner)
{
    const std::wstring identifier =
        std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    const std::wstring doneName =
        MediaOpenIntegrationObjectName(L"Done", identifier);
    const std::wstring successName =
        MediaOpenIntegrationObjectName(L"Success", identifier);
    const HANDLE done = CreateEventW(
        nullptr, TRUE, FALSE, doneName.c_str());
    const HANDLE success = CreateEventW(
        nullptr, TRUE, FALSE, successName.c_str());
    const bool eventsCreated = done != nullptr && success != nullptr;
    runner.True(eventsCreated, L"shared-memory integration events are created");
    if (!eventsCreated) {
        if (done != nullptr) {
            CloseHandle(done);
        }
        if (success != nullptr) {
            CloseHandle(success);
        }
        return;
    }

    std::wstring launchError;
    const bool launched = videoplayer::LaunchMediaInNewInstance(
        BuildMediaOpenIntegrationPayload(identifier), launchError);
    runner.True(
        launched && launchError.empty(),
        L"shared-memory integration child launches without error");

    const DWORD completion = launched
        ? WaitForSingleObject(done, 10'000)
        : WAIT_FAILED;
    runner.Equal(
        completion,
        static_cast<DWORD>(WAIT_OBJECT_0),
        L"shared-memory integration child completes within timeout");
    runner.Equal(
        WaitForSingleObject(success, 0),
        static_cast<DWORD>(WAIT_OBJECT_0),
        L"exact Unicode path round-trips outside the child command line");

    CloseHandle(done);
    CloseHandle(success);
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

void TestVideoGeometryBasics(TestRunner& runner)
{
    using namespace videoplayer;

    runner.Equal(ScaleLogicalPixels(32, 96), 32, L"logical pixels at 96 DPI");
    runner.Equal(ScaleLogicalPixels(32, 144), 48, L"logical pixels at 144 DPI");
    runner.Equal(ScaleLogicalPixels(100, 120), 125, L"logical pixels at 120 DPI");
    runner.Equal(ScaleLogicalPixels(32, 0), 32, L"zero DPI falls back to 96");
    runner.Equal(ScaleLogicalPixels(0, 144), 0, L"zero logical pixels remain zero");
    runner.Equal(ScaleLogicalPixels(-1, 144), 0, L"negative logical pixels clamp to zero");
    runner.Equal(
        ScaleLogicalPixels((std::numeric_limits<int>::max)(),
            (std::numeric_limits<unsigned>::max)()),
        (std::numeric_limits<int>::max)(),
        L"logical pixel scaling saturates");

    const GeometryRect reverse = NormalizeRectangle({80, 90}, {20, 10});
    runner.Equal(reverse, GeometryRect{20, 10, 80, 90}, L"reverse drag normalizes");
    runner.Equal(reverse.Width(), 60, L"normalized rectangle width");
    runner.Equal(reverse.Height(), 80, L"normalized rectangle height");
    runner.True(!reverse.IsEmpty(), L"normalized rectangle is nonempty");
    runner.True(GeometryRect{1, 1, 1, 2}.IsEmpty(), L"zero-width rectangle is empty");
    runner.Equal(
        IntersectRectangles({0, 0, 100, 100}, {50, -20, 120, 70}),
        GeometryRect{50, 0, 100, 70},
        L"rectangle intersection clips every edge");
    runner.Equal(
        IntersectRectangles({0, 0, 10, 10}, {10, 0, 20, 10}),
        GeometryRect{},
        L"touching rectangles have empty intersection");
    runner.Equal(
        OffsetRectangle({10, 20, 30, 40}, -5, 7),
        GeometryRect{5, 27, 25, 47},
        L"rectangle offset");
}

void TestVideoContentFit(TestRunner& runner)
{
    using namespace videoplayer;

    runner.Equal(
        ComputeVideoContentRect({1920, 1080}, {0, 0, 800, 600}),
        GeometryRect{0, 75, 800, 525},
        L"widescreen video is letterboxed in 4:3 viewport");
    runner.Equal(
        ComputeVideoContentRect({640, 480}, {0, 0, 1920, 1080}),
        GeometryRect{240, 0, 1680, 1080},
        L"4:3 video is pillarboxed in widescreen viewport");
    runner.Equal(
        ComputeVideoContentRect({1920, 1080}, {10, 20, 650, 380}),
        GeometryRect{10, 20, 650, 380},
        L"matching aspect ratio fills offset viewport");
    runner.Equal(
        ComputeVideoContentRect({1080, 1920}, {0, 0, 1000, 500}),
        GeometryRect{359, 0, 640, 500},
        L"portrait video remains centered");
    runner.Equal(
        ComputeVideoContentRect({640, 480}, {100, 50, 900, 650}),
        GeometryRect{100, 50, 900, 650},
        L"fit preserves viewport origin");
    runner.Equal(
        ComputeVideoContentRect({0, 1080}, {0, 0, 800, 600}),
        GeometryRect{},
        L"zero source width rejects fit");
    runner.Equal(
        ComputeVideoContentRect({1920, 1080}, {0, 0, 0, 600}),
        GeometryRect{},
        L"empty viewport rejects fit");
}

void TestVideoCropMapping(TestRunner& runner)
{
    using namespace videoplayer;
    const VideoDimensions video{1920, 1080};
    const GeometryRect viewport{0, 0, 800, 600};

    const VideoCropMapping full =
        MapSelectionToVideoCrop(video, viewport, {0, 75, 800, 525}, 32);
    runner.True(full.valid, L"full content selection maps");
    runner.Equal(full.contentRect, GeometryRect{0, 75, 800, 525}, L"mapping reports content rect");
    runner.Equal(full.clippedSelection, full.contentRect, L"full selection remains unclipped");
    runner.Equal(full.crop, VideoCrop{0, 0, 1920, 1080}, L"full selection maps to full source");
    runner.Near(full.normalizedSelection.left, 0.0, 0.000001, L"full normalized left");
    runner.Near(full.normalizedSelection.bottom, 1.0, 0.000001, L"full normalized bottom");

    const VideoCropMapping reverse =
        MapSelectionToVideoCrop(video, viewport, {600, 400, 200, 200}, 32);
    runner.True(reverse.valid, L"reverse selection maps");
    runner.Equal(
        reverse.clippedSelection,
        GeometryRect{200, 200, 600, 400},
        L"reverse selection normalizes before mapping");
    runner.Equal(
        reverse.crop,
        VideoCrop{480, 300, 960, 480},
        L"reverse selection maps to expected source crop");
    runner.Near(reverse.normalizedSelection.left, 0.25, 0.000001, L"normalized crop left");
    runner.Near(reverse.normalizedSelection.right, 0.75, 0.000001, L"normalized crop right");

    const VideoCropMapping clipped =
        MapSelectionToVideoCrop(video, viewport, {-100, -100, 400, 300}, 32);
    runner.True(clipped.valid, L"partially outside selection is clipped");
    runner.Equal(
        clipped.clippedSelection,
        GeometryRect{0, 75, 400, 300},
        L"selection clips to fitted video rather than viewport");
    runner.Equal(
        clipped.crop,
        VideoCrop{0, 0, 960, 540},
        L"clipped selection maps to source bounds");

    const VideoCropMapping letterboxOnly =
        MapSelectionToVideoCrop(video, viewport, {0, 0, 800, 74}, 1);
    runner.True(!letterboxOnly.valid, L"selection wholly in letterbox is rejected");
    runner.Equal(letterboxOnly.clippedSelection, GeometryRect{}, L"letterbox selection has empty clip");

    const VideoCropMapping tooSmall =
        MapSelectionToVideoCrop(video, viewport, {10, 80, 41, 112}, 32);
    runner.True(!tooSmall.valid, L"selection below minimum width is rejected");
    const VideoCropMapping minimum =
        MapSelectionToVideoCrop(video, viewport, {10, 80, 42, 112}, 32);
    runner.True(minimum.valid, L"selection exactly at minimum size is accepted");
    runner.True(
        minimum.crop.x + minimum.crop.width <= video.width &&
            minimum.crop.y + minimum.crop.height <= video.height,
        L"minimum crop remains within decoded bounds");
    runner.True(
        (minimum.crop.x % 2U) == 0U && (minimum.crop.y % 2U) == 0U &&
            (minimum.crop.width % 2U) == 0U && (minimum.crop.height % 2U) == 0U,
        L"ordinary crop coordinates and extents are even aligned");

    const VideoCropMapping clippedToBounds =
        MapSelectionToVideoCrop(video, viewport, {-1000, -1000, 2000, 2000}, 32);
    runner.Equal(
        clippedToBounds.crop,
        VideoCrop{0, 0, 1920, 1080},
        L"oversized selection clamps to decoded source");

    const VideoCropMapping oddFinalPixel = MapSelectionToVideoCrop(
        {5, 5}, {0, 0, 5, 5}, {4, 4, 5, 5}, 1);
    runner.True(oddFinalPixel.valid, L"final pixel in odd-sized source remains selectable");
    runner.Equal(
        oddFinalPixel.crop,
        VideoCrop{4, 4, 1, 1},
        L"odd source edge stays bounded when even alignment is impossible");
    const VideoCropMapping oddAligned = MapSelectionToVideoCrop(
        {5, 5}, {0, 0, 5, 5}, {1, 1, 4, 4}, 1);
    runner.Equal(
        oddAligned.crop,
        VideoCrop{0, 0, 4, 4},
        L"odd source crop expands to even boundaries when possible");

    const VideoCropMapping formerToolbarArea = MapSelectionToVideoCrop(
        video,
        {0, 0, 1920, 1080},
        {0, 992, 1920, 1080},
        32);
    runner.True(
        formerToolbarArea.valid,
        L"video formerly under fullscreen toolbar remains selectable");
    runner.Equal(
        formerToolbarArea.crop,
        VideoCrop{0, 992, 1920, 88},
        L"selection reaches decoded pixels behind the hidden toolbar");

    runner.Equal(
        FormatLibVlc3CropGeometry(reverse.crop),
        std::string("1440x780+480+300"),
        L"mapped crop uses LibVLC 3 absolute right and bottom edges");
    runner.Equal(
        FormatLibVlc3CropGeometry({0, 0, 960, 480}),
        std::string("960x480+0+0"),
        L"zero-origin crop keeps its extents as endpoints");
    runner.Equal(
        FormatLibVlc3CropGeometry({1280, 720, 640, 360}),
        std::string("1920x1080+1280+720"),
        L"crop ending at source right and bottom preserves the edge");
    runner.True(
        FormatLibVlc3CropGeometry({1, 2, 0, 4}).empty(),
        L"invalid crop has no geometry string");
    runner.True(
        FormatLibVlc3CropGeometry(
            {(std::numeric_limits<unsigned>::max)(), 0, 1, 1}).empty(),
        L"overflowing LibVLC crop endpoint is rejected");
}

void TestPreviewMath(TestRunner& runner)
{
    using namespace videoplayer;

    runner.Equal(QuantizePreviewTimestamp(-10, 10'000), std::int64_t{0}, L"preview time clamps low");
    runner.Equal(QuantizePreviewTimestamp(20'000, 10'000), std::int64_t{10'000}, L"preview time clamps high");
    runner.Equal(QuantizePreviewTimestamp(374, 10'000), std::int64_t{0}, L"preview quantum below midpoint");
    runner.Equal(QuantizePreviewTimestamp(375, 10'000), std::int64_t{750}, L"preview quantum midpoint rounds up");
    runner.Equal(QuantizePreviewTimestamp(1'124, 10'000), std::int64_t{750}, L"preview quantum upper lower-half edge");
    runner.Equal(QuantizePreviewTimestamp(1'125, 10'000), std::int64_t{1'500}, L"preview quantum upper midpoint");
    runner.Equal(QuantizePreviewTimestamp(1'000, 1'000), std::int64_t{1'000}, L"duration endpoint is preserved");
    runner.Equal(QuantizePreviewTimestamp(500, 10'000, 1'000), std::int64_t{1'000}, L"custom quantum midpoint");
    runner.Equal(QuantizePreviewTimestamp(500, 0), std::int64_t{0}, L"invalid duration rejects quantization");
    runner.Equal(QuantizePreviewTimestamp(500, 1'000, 0), std::int64_t{0}, L"invalid quantum rejects quantization");

    const RECT channel{10, 2, 110, 12};
    runner.Equal(PreviewTimeFromChannelX(10, channel, 10'000), std::int64_t{0}, L"channel left edge maps to zero");
    runner.Equal(PreviewTimeFromChannelX(60, channel, 10'000), std::int64_t{5'000}, L"channel midpoint maps to half duration");
    runner.Equal(PreviewTimeFromChannelX(110, channel, 10'000), std::int64_t{10'000}, L"channel right edge maps to duration");
    runner.Equal(PreviewTimeFromChannelX(-500, channel, 10'000), std::int64_t{0}, L"mouse left of channel clamps");
    runner.Equal(PreviewTimeFromChannelX(500, channel, 10'000), std::int64_t{10'000}, L"mouse right of channel clamps");
    runner.Equal(PreviewTimeFromChannelX(60, channel, 0), std::int64_t{0}, L"zero duration rejects channel mapping");
    runner.Equal(
        PreviewTimeFromChannelX(60, RECT{10, 0, 10, 10}, 10'000),
        std::int64_t{0},
        L"empty channel rejects mapping");

    const RECT pointerRange = TrackbarPointerRange(
        RECT{8, 2, 592, 12},
        RECT{295, 0, 306, 20});
    runner.Equal(pointerRange.left, 13L, L"trackbar range starts at minimum thumb center");
    runner.Equal(pointerRange.right, 586L, L"trackbar range ends at maximum thumb center");
    runner.Equal(
        TrackbarPositionFromPointerX(13, pointerRange, 0, 10'000),
        0,
        L"track click at start maps to minimum");
    runner.Equal(
        TrackbarPositionFromPointerX(154, pointerRange, 0, 10'000),
        2'461,
        L"track click maps to its exact pixel-derived position");
    runner.Equal(
        TrackbarPositionFromPointerX(442, pointerRange, 0, 10'000),
        7'487,
        L"track click near three quarters maps without a page step");
    runner.Equal(
        TrackbarPositionFromPointerX(1'000, pointerRange, 0, 10'000),
        10'000,
        L"track click past end clamps to maximum");
    runner.Equal(
        TrackbarPositionFromPointerX(50, RECT{}, -10, 10),
        -10,
        L"empty pointer range returns slider minimum");

    const SIZE widescreen = FitPreviewSize(1920, 1080);
    runner.Equal(widescreen.cx, 320L, L"16:9 preview maximum width");
    runner.Equal(widescreen.cy, 180L, L"16:9 preview maximum height");
    const SIZE fourByThree = FitPreviewSize(640, 480);
    runner.Equal(fourByThree.cx, 240L, L"4:3 preview fitted width");
    runner.Equal(fourByThree.cy, 180L, L"4:3 preview fitted height");
    const SIZE portrait = FitPreviewSize(1080, 1920);
    runner.Equal(portrait.cx, 101L, L"portrait preview fitted width");
    runner.Equal(portrait.cy, 180L, L"portrait preview maximum height");
    const SIZE ultrawide = FitPreviewSize(3840, 1080);
    runner.Equal(ultrawide.cx, 320L, L"ultrawide preview maximum width");
    runner.Equal(ultrawide.cy, 90L, L"ultrawide preview fitted height");
    const SIZE square = FitPreviewSize(100, 100);
    runner.Equal(square.cx, 180L, L"square preview fitted width");
    runner.Equal(square.cy, 180L, L"square preview fitted height");
    const SIZE invalid = FitPreviewSize(0, 1080);
    runner.Equal(invalid.cx, 0L, L"invalid preview width returns empty size");
    runner.Equal(invalid.cy, 0L, L"invalid preview height returns empty size");
    const SIZE custom = FitPreviewSize(1920, 1080, 160, 90);
    runner.Equal(custom.cx, 160L, L"custom preview maximum width");
    runner.Equal(custom.cy, 90L, L"custom preview maximum height");

    runner.Equal(ScalePreviewPixels(320, 96), 320, L"preview pixels at 96 DPI");
    runner.Equal(ScalePreviewPixels(320, 144), 480, L"preview pixels at 144 DPI");
    runner.Equal(ScalePreviewPixels(100, 120), 125, L"preview pixels at 120 DPI");
    runner.Equal(ScalePreviewPixels(320, 0), 320, L"preview pixels invalid DPI fallback");

    const RECT monitor{0, 0, 1920, 1080};
    const RECT centered = PlacePreviewPopup({960, 900}, {320, 180}, monitor, 12);
    runner.Equal(centered.left, 800L, L"preview popup centers on anchor");
    runner.Equal(centered.top, 708L, L"preview popup sits above anchor");
    runner.Equal(centered.right, 1120L, L"preview popup width is preserved");
    runner.Equal(centered.bottom, 888L, L"preview popup gap is preserved");
    const RECT leftClamped = PlacePreviewPopup({10, 900}, {320, 180}, monitor, 12);
    runner.Equal(leftClamped.left, 0L, L"preview popup clamps at monitor left");
    const RECT rightClamped = PlacePreviewPopup({1910, 900}, {320, 180}, monitor, 12);
    runner.Equal(rightClamped.right, 1920L, L"preview popup clamps at monitor right");
    const RECT movedBelow = PlacePreviewPopup({960, 50}, {320, 180}, monitor, 12);
    runner.Equal(movedBelow.top, 62L, L"preview popup moves below anchor at monitor top");
    const RECT negativeMonitor = PlacePreviewPopup(
        {-160, 900}, {320, 180}, {-1920, 0, 0, 1080}, 12);
    runner.True(
        negativeMonitor.left == -320 && negativeMonitor.top == 708 &&
            negativeMonitor.right == 0 && negativeMonitor.bottom == 888,
        L"preview popup clamps on negative-coordinate monitor");
    const RECT oversized = PlacePreviewPopup(
        {200, 200}, {500, 500}, {100, 100, 300, 250}, 10);
    runner.True(
        oversized.left == 100 && oversized.top == 100 &&
            oversized.right == 300 && oversized.bottom == 250,
        L"oversized preview popup clamps to monitor dimensions");
}

videoplayer::PreviewFramePtr MakePreviewFrame(
    const std::uint32_t generation,
    const std::int64_t timestampMs,
    const std::uint8_t fill,
    const unsigned width = 2,
    const unsigned height = 2,
    const unsigned pitch = 0)
{
    auto frame = std::make_shared<videoplayer::PreviewFrame>();
    frame->mediaGeneration = generation;
    frame->timestampMs = timestampMs;
    frame->width = width;
    frame->height = height;
    frame->pitch = pitch == 0 ? width * 4U : pitch;
    frame->pixels.assign(
        static_cast<std::size_t>(frame->pitch) * height,
        fill);
    return frame;
}

void TestPreviewCache(TestRunner& runner)
{
    using namespace videoplayer;
    using FrameElement =
        std::remove_reference_t<decltype(*std::declval<PreviewFramePtr>())>;
    runner.True(std::is_const_v<FrameElement>, L"cached frame pointers expose immutable frames");

    PreviewCache cache(3);
    runner.Equal(cache.Capacity(), std::size_t{3}, L"preview cache reports configured capacity");
    runner.Equal(cache.Size(), std::size_t{0}, L"preview cache starts empty");
    cache.ClearForMedia(7);
    runner.Equal(cache.MediaGeneration(), std::uint32_t{7}, L"preview cache records media generation");

    const PreviewFramePtr first = MakePreviewFrame(7, 0, 1);
    const PreviewFramePtr second = MakePreviewFrame(7, 750, 2);
    const PreviewFramePtr third = MakePreviewFrame(7, 1'500, 3);
    cache.Put(first);
    cache.Put(second);
    cache.Put(third);
    runner.Equal(cache.Size(), std::size_t{3}, L"preview cache fills to capacity");
    runner.True(cache.Find(7, 0) == first, L"preview cache returns immutable frame pointer");
    runner.Equal(cache.Find(7, 0)->pixels.front(), std::uint8_t{1}, L"preview frame pixels remain in RAM");
    runner.True(!cache.Find(8, 0), L"preview cache rejects wrong generation lookup");
    runner.True(!cache.Find(7, 999), L"preview cache misses unknown timestamp");

    // Looking up the first frame promotes it. The second is therefore the LRU
    // entry and must be evicted when the fourth frame is inserted.
    const PreviewFramePtr fourth = MakePreviewFrame(7, 2'250, 4);
    cache.Put(fourth);
    runner.Equal(cache.Size(), std::size_t{3}, L"preview cache remains capped");
    runner.True(!cache.Find(7, 750), L"least-recently-used preview is evicted");
    runner.True(cache.Find(7, 0) == first, L"recently accessed preview survives eviction");
    runner.True(cache.Find(7, 2'250) == fourth, L"newest preview is retained");

    const PreviewFramePtr replacement = MakePreviewFrame(7, 1'500, 99);
    cache.Put(replacement);
    runner.Equal(cache.Size(), std::size_t{3}, L"replacement does not grow preview cache");
    runner.True(cache.Find(7, 1'500) == replacement, L"duplicate timestamp replaces cached frame");
    runner.Equal(
        cache.Find(7, 1'500)->pixels.front(),
        std::uint8_t{99},
        L"replacement pixels are visible through immutable frame");

    cache.Put(MakePreviewFrame(8, 3'000, 8));
    runner.Equal(cache.Size(), std::size_t{3}, L"wrong-generation insert is ignored");
    cache.Put(MakePreviewFrame(7, 3'000, 8, 2, 2, 12));
    runner.Equal(cache.Size(), std::size_t{3}, L"padded preview pitch is rejected");
    cache.Put(MakePreviewFrame(7, 3'000, 8, 321, 1));
    runner.Equal(cache.Size(), std::size_t{3}, L"oversized preview frame is rejected");
    cache.Put({});
    runner.Equal(cache.Size(), std::size_t{3}, L"null preview frame is ignored");

    cache.ClearForMedia(8);
    runner.Equal(cache.Size(), std::size_t{0}, L"new media generation clears previews");
    runner.Equal(cache.MediaGeneration(), std::uint32_t{8}, L"new generation replaces old generation");
    runner.True(!cache.Find(7, 0), L"old generation cannot leak after media change");
    const PreviewFramePtr newMedia = MakePreviewFrame(8, 0, 8);
    cache.Put(newMedia);
    runner.True(cache.Find(8, 0) == newMedia, L"new generation accepts previews");
    cache.Clear();
    runner.Equal(cache.Size(), std::size_t{0}, L"explicit cache clear releases previews");
    runner.Equal(cache.MediaGeneration(), std::uint32_t{0}, L"explicit cache clear resets generation");

    PreviewCache defaultCache;
    defaultCache.ClearForMedia(1);
    for (std::int64_t index = 0; index < 25; ++index) {
        defaultCache.Put(MakePreviewFrame(1, index * 750, static_cast<std::uint8_t>(index)));
    }
    runner.Equal(defaultCache.Capacity(), std::size_t{24}, L"default preview cache capacity is 24");
    runner.Equal(defaultCache.Size(), std::size_t{24}, L"default preview cache never exceeds 24 frames");
    runner.True(!defaultCache.Find(1, 0), L"25th preview evicts oldest default-cache entry");
    runner.True(defaultCache.Find(1, 24 * 750) != nullptr, L"25th preview remains cached");

    PreviewCache disabled(0);
    disabled.ClearForMedia(4);
    disabled.Put(MakePreviewFrame(4, 0, 1));
    runner.Equal(disabled.Size(), std::size_t{0}, L"zero-capacity preview cache stores nothing");
}

videoplayer::ToolbarVisibilityInput FullscreenPlayingToolbarInput()
{
    videoplayer::ToolbarVisibilityInput input{};
    input.wasFullscreen = true;
    input.fullscreen = true;
    input.playing = true;
    input.currentlyVisible = true;
    input.clientHeight = 1'000;
    input.dpi = 96;
    input.nowMs = 1'000;
    input.lastInteractionMs = 0;
    return input;
}

void TestToolbarVisibility(TestRunner& runner)
{
    using namespace videoplayer;

    runner.Equal(ToolbarActivationHeight(96), 100, L"toolbar activation zone at 96 DPI");
    runner.Equal(ToolbarActivationHeight(144), 150, L"toolbar activation zone at 144 DPI");
    runner.Equal(ToolbarActivationHeight(0), 100, L"toolbar activation zone invalid DPI fallback");

    ToolbarVisibilityInput input = FullscreenPlayingToolbarInput();
    input.fullscreen = false;
    input.currentlyVisible = false;
    ToolbarVisibilityDecision decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"windowed toolbar is always visible");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"windowed hidden toolbar receives show action");

    input = FullscreenPlayingToolbarInput();
    input.wasFullscreen = false;
    input.currentlyVisible = false;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"fullscreen transition reveals toolbar");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"fullscreen transition emits show action");

    input = FullscreenPlayingToolbarInput();
    input.playing = false;
    input.currentlyVisible = false;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"paused stopped opening error and ended states keep toolbar visible");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"non-playing state shows toolbar");

    input = FullscreenPlayingToolbarInput();
    input.nowMs = 1'199;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"toolbar remains visible before 1200 ms");
    runner.Equal(decision.action, ToolbarVisibilityAction::None, L"no action before hide boundary");
    input.nowMs = 1'200;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldBeVisible, L"toolbar hides at exact 1200 ms boundary");
    runner.Equal(decision.action, ToolbarVisibilityAction::Hide, L"exact hide boundary emits hide action");

    input = FullscreenPlayingToolbarInput();
    input.nowMs = 100;
    input.lastInteractionMs = 200;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"clock rollback cannot hide toolbar");

    input = FullscreenPlayingToolbarInput();
    input.currentlyVisible = false;
    input.cursorInsideClient = true;
    input.cursorY = 900;
    input.cursorMoved = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.inBottomActivationZone, L"activation-zone top edge is inclusive");
    runner.True(decision.shouldBeVisible, L"mouse movement in bottom zone reveals toolbar");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"bottom-zone movement emits show action");
    input.cursorMoved = false;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldBeVisible, L"stationary cursor in bottom zone does not reveal hidden toolbar");
    input.cursorMoved = true;
    input.cursorY = 899;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.inBottomActivationZone, L"pixel above activation zone is outside");
    runner.True(!decision.shouldBeVisible, L"movement outside bottom zone does not reveal toolbar");
    input.cursorY = 1'000;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.inBottomActivationZone, L"client bottom edge is outside half-open zone");

    input = FullscreenPlayingToolbarInput();
    input.currentlyVisible = false;
    input.cursorInsideClient = true;
    input.cursorMoved = true;
    input.dpi = 144;
    input.cursorY = 850;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.inBottomActivationZone, L"DPI-scaled activation top edge reveals toolbar");

    input = FullscreenPlayingToolbarInput();
    input.nowMs = 5'000;
    input.cursorMoved = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"fresh cursor movement blocks hide");

    const auto verifyBlocker = [&runner](
                                   ToolbarVisibilityInput blocked,
                                   std::wstring_view visibleName,
                                   std::wstring_view blockerName) {
        blocked.nowMs = 5'000;
        blocked.lastInteractionMs = 0;
        const ToolbarVisibilityDecision blockedDecision =
            videoplayer::EvaluateToolbarVisibility(blocked);
        runner.True(blockedDecision.shouldBeVisible, visibleName);
        runner.True(blockedDecision.interactionBlocksHide, blockerName);
    };

    input = FullscreenPlayingToolbarInput();
    input.pointerOverToolbar = true;
    verifyBlocker(input, L"pointer over toolbar blocks hide", L"pointer blocker is reported");
    input = FullscreenPlayingToolbarInput();
    input.seekDragging = true;
    verifyBlocker(input, L"seek drag blocks hide", L"seek blocker is reported");
    input = FullscreenPlayingToolbarInput();
    input.volumeDragging = true;
    verifyBlocker(input, L"volume drag blocks hide", L"volume blocker is reported");
    input = FullscreenPlayingToolbarInput();
    input.previewVisible = true;
    verifyBlocker(input, L"preview popup blocks hide", L"preview blocker is reported");
    input = FullscreenPlayingToolbarInput();
    input.zoomSelecting = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldBeVisible, L"zoom selection hides fullscreen toolbar");
    runner.Equal(decision.action, ToolbarVisibilityAction::Hide, L"zoom selection emits hide action");
    runner.True(!decision.interactionBlocksHide, L"zoom selection is a force-hide mode");
    input.currentlyVisible = false;
    input.playing = false;
    input.cursorMoved = true;
    input.pointerOverToolbar = true;
    input.controlHasFocus = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldBeVisible, L"paused zoom selection keeps toolbar hidden");
    runner.Equal(decision.action, ToolbarVisibilityAction::None, L"hidden zoom toolbar stays hidden");

    input = FullscreenPlayingToolbarInput();
    input.fullscreen = false;
    input.zoomSelecting = true;
    input.currentlyVisible = false;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"windowed zoom keeps non-overlapping toolbar visible");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"windowed zoom restores toolbar");
    input = FullscreenPlayingToolbarInput();
    input.controlHasFocus = true;
    verifyBlocker(input, L"focused control blocks hide", L"focus blocker is reported");

    input = FullscreenPlayingToolbarInput();
    input.currentlyVisible = false;
    input.previewVisible = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"active blocker reveals an already hidden toolbar");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"hidden blocked toolbar emits show action");
}

void TestZoomEscapeHierarchy(TestRunner& runner)
{
    using namespace videoplayer;

    ZoomEscapeDecision decision = EvaluateZoomEscape(ZoomState::Selecting, true);
    runner.Equal(decision.action, ZoomEscapeAction::CancelSelection, L"Escape cancels selection first");
    runner.Equal(decision.nextState, ZoomState::None, L"cancel selection returns to none");
    runner.True(!decision.exitFullscreen, L"cancel selection stays fullscreen");

    decision = EvaluateZoomEscape(ZoomState::Selecting, false);
    runner.Equal(decision.action, ZoomEscapeAction::CancelSelection, L"windowed Escape cancels selection");
    runner.True(!decision.exitFullscreen, L"windowed selection cancel does not exit fullscreen");

    decision = EvaluateZoomEscape(ZoomState::Applied, true);
    runner.Equal(decision.action, ZoomEscapeAction::ResetCrop, L"Escape resets applied crop before fullscreen");
    runner.Equal(decision.nextState, ZoomState::None, L"crop reset returns to none");
    runner.True(!decision.exitFullscreen, L"crop reset stays fullscreen");

    decision = EvaluateZoomEscape(ZoomState::Applied, false);
    runner.Equal(decision.action, ZoomEscapeAction::ResetCrop, L"windowed Escape resets applied crop");
    runner.True(!decision.exitFullscreen, L"windowed crop reset does not request fullscreen exit");

    decision = EvaluateZoomEscape(ZoomState::None, true);
    runner.Equal(decision.action, ZoomEscapeAction::ExitFullscreen, L"Escape exits fullscreen only with no zoom state");
    runner.True(decision.exitFullscreen, L"fullscreen exit flag is set");
    decision = EvaluateZoomEscape(ZoomState::None, false);
    runner.Equal(decision.action, ZoomEscapeAction::None, L"Escape is idle when windowed with no zoom");
    runner.True(!decision.exitFullscreen, L"idle Escape has no fullscreen exit flag");

    const ZoomEscapeDecision firstEscape = EvaluateZoomEscape(ZoomState::Applied, true);
    const ZoomEscapeDecision secondEscape = EvaluateZoomEscape(firstEscape.nextState, true);
    runner.Equal(firstEscape.action, ZoomEscapeAction::ResetCrop, L"first Escape of two-step flow resets crop");
    runner.Equal(secondEscape.action, ZoomEscapeAction::ExitFullscreen, L"second Escape of two-step flow exits fullscreen");
}

void TestPrivacyPolicy(TestRunner& runner)
{
    using namespace videoplayer;

    runner.Equal(kPrivateLibVlcArguments.size(), std::size_t{3}, L"private LibVLC option count is exact");
    runner.Equal(
        std::string_view(kPrivateLibVlcArguments[0]),
        std::string_view("--ignore-config"),
        L"private LibVLC ignores configuration");
    runner.Equal(
        std::string_view(kPrivateLibVlcArguments[1]),
        std::string_view("--no-media-library"),
        L"private LibVLC disables media library");
    runner.Equal(
        std::string_view(kPrivateLibVlcArguments[2]),
        std::string_view("--no-video-title-show"),
        L"private LibVLC disables video title overlay");
    runner.Equal(
        kPrivateOpenDialogFlags,
        static_cast<DWORD>(
            OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
            OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT |
            OFN_ALLOWMULTISELECT),
        L"private file dialog flag set is exact");
    runner.True(
        (kPrivateOpenDialogFlags & OFN_DONTADDTORECENT) != 0,
        L"file dialog explicitly avoids Windows recent documents");
    runner.True(
        (kPrivateOpenDialogFlags & OFN_NOCHANGEDIR) != 0,
        L"file dialog does not persistently change working directory");
    runner.True(
        (kPrivateOpenDialogFlags & OFN_FILEMUSTEXIST) != 0,
        L"private file dialog requires existing media");
    runner.True(
        (kPrivateOpenDialogFlags & OFN_ALLOWMULTISELECT) != 0,
        L"file dialog accepts several videos in one selection");
}

} // namespace

int wmain(const int argc, wchar_t* const* const argv)
{
    std::wstring inheritedMediaPath;
    std::wstring inheritedMediaError;
    const videoplayer::InheritedMediaPathStatus inheritedStatus =
        videoplayer::ReadInheritedMediaPath(
            argc, argv, inheritedMediaPath, inheritedMediaError);
    if (inheritedStatus == videoplayer::InheritedMediaPathStatus::Error) {
        return 83;
    }
    if (inheritedStatus == videoplayer::InheritedMediaPathStatus::Ready) {
        return RunMediaOpenIntegrationChild(inheritedMediaPath);
    }

    TestRunner runner;
    TestTimeFormatting(runner);
    TestPlaybackMath(runner);
    TestProgressStrip(runner);
    TestUtf8(runner);
    TestMediaOpen(runner);
    TestMediaOpenLaunchIntegration(runner);
    TestLongPaths(runner);
    TestFileExistence(runner);
    TestVideoGeometryBasics(runner);
    TestVideoContentFit(runner);
    TestVideoCropMapping(runner);
    TestPreviewMath(runner);
    TestPreviewCache(runner);
    TestToolbarVisibility(runner);
    TestZoomEscapeHierarchy(runner);
    TestPrivacyPolicy(runner);
    RunPlaybackSnapshotTests(runner);
    RunPreviewLifecycleTests([&runner](bool success, const wchar_t* name) {
        runner.True(success, name);
    });
    int uiPassed = 0;
    int uiFailed = 0;
    RunUiLifecycleTests(uiPassed, uiFailed);
    runner.AddCounts(uiPassed, uiFailed);
    return runner.Finish();
}
