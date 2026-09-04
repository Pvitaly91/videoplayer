#include "../../src/VideoPlayer/App.h"
#include "../../src/VideoPlayer/PathUtils.h"
#include "../../src/VideoPlayer/PlaybackMath.h"
#include "../../src/VideoPlayer/PreviewCache.h"
#include "../../src/VideoPlayer/PreviewMath.h"
#include "../../src/VideoPlayer/PrivacyPolicy.h"
#include "../../src/VideoPlayer/TimeFormatter.h"
#include "../../src/VideoPlayer/ToolbarVisibility.h"
#include "../../src/VideoPlayer/Utf8.h"
#include "../../src/VideoPlayer/VideoGeometry.h"
#include "../../src/VideoPlayer/ZoomState.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

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

    void Near(double actual, double expected, double tolerance, std::wstring_view name)
    {
        True(std::abs(actual - expected) <= tolerance, name);
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

    runner.Equal(
        FormatVideoCropGeometry({480, 300, 960, 480}),
        std::string("960x480+480+300"),
        L"LibVLC crop geometry format");
    runner.True(
        FormatVideoCropGeometry({1, 2, 0, 4}).empty(),
        L"invalid crop has no geometry string");
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
    verifyBlocker(input, L"zoom selection blocks hide", L"zoom blocker is reported");
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
            OFN_HIDEREADONLY | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT),
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
    TestVideoGeometryBasics(runner);
    TestVideoContentFit(runner);
    TestVideoCropMapping(runner);
    TestPreviewMath(runner);
    TestPreviewCache(runner);
    TestToolbarVisibility(runner);
    TestZoomEscapeHierarchy(runner);
    TestPrivacyPolicy(runner);
    return runner.Finish();
}
