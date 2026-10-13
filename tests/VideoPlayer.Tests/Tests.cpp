#include "../../src/VideoPlayer/App.h"
#include "../../src/VideoPlayer/ExportGeometry.h"
#include "../../src/VideoPlayer/ExportJob.h"
#include "../../src/VideoPlayer/ExportPlanner.h"
#include "../../src/VideoPlayer/ExportProcess.h"
#include "../../src/VideoPlayer/RemoveSegment.h"
#include "../../src/VideoPlayer/MediaProbe.h"
#include "../../src/VideoPlayer/MediaOpen.h"
#include "../../src/VideoPlayer/PathUtils.h"
#include "../../src/VideoPlayer/PlaybackLoop.h"
#include "../../src/VideoPlayer/PlaybackMath.h"
#include "../../src/VideoPlayer/PreviewCache.h"
#include "../../src/VideoPlayer/PreviewMath.h"
#include "../../src/VideoPlayer/ProgressStrip.h"
#include "../../src/VideoPlayer/IntervalTimeline.h"
#include "../../src/VideoPlayer/PrivacyPolicy.h"
#include "../../src/VideoPlayer/Resource.h"
#include "../../src/VideoPlayer/TimeFormatter.h"
#include "../../src/VideoPlayer/ToolbarVisibility.h"
#include "../../src/VideoPlayer/Utf8.h"
#include "../../src/VideoPlayer/VideoDisplayGeometry.h"
#include "../../src/VideoPlayer/VideoGeometry.h"
#include "../../src/VideoPlayer/ZoomState.h"
#include "PlaybackSnapshotTests.h"
#include "PreviewLifecycleTests.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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

bool IconHasBrandColors(const HICON icon) {
    ICONINFO info{};
    if (!GetIconInfo(icon, &info)) return false;
    BITMAP color{};
    bool foundOrange = false;
    bool foundWhite = false;
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(color), &color) == static_cast<int>(sizeof(color)) &&
        color.bmWidth > 0 && color.bmHeight > 0) {
        BITMAPINFO bitmap{};
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = color.bmWidth;
        bitmap.bmiHeader.biHeight = -color.bmHeight;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint8_t> pixels(
            static_cast<std::size_t>(color.bmWidth) * color.bmHeight * 4);
        const HDC dc = GetDC(nullptr);
        if (dc && GetDIBits(dc, info.hbmColor, 0, static_cast<UINT>(color.bmHeight),
            pixels.data(), &bitmap, DIB_RGB_COLORS) == color.bmHeight) {
            for (std::size_t offset = 0; offset < pixels.size(); offset += 4) {
                const auto blue = pixels[offset];
                const auto green = pixels[offset + 1];
                const auto red = pixels[offset + 2];
                foundOrange = foundOrange || (red > 180 && green > 65 &&
                    green < 210 && blue < 110);
                foundWhite = foundWhite || (red > 225 && green > 225 && blue > 225);
            }
        }
        if (dc) ReleaseDC(nullptr, dc);
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    return foundOrange && foundWhite;
}

void CheckApplicationIconResources(TestRunner& runner, const HMODULE module,
    const std::wstring_view context) {
    const auto name = [&](const std::wstring_view detail) {
        return std::wstring(context) + L": " + std::wstring(detail);
    };
    const HRSRC group = FindResourceW(module, MAKEINTRESOURCEW(IDI_VIDEOPLAYER),
        RT_GROUP_ICON);
    runner.True(group != nullptr, name(L"application icon group is embedded"));
    if (!group) return;
    const DWORD bytes = SizeofResource(module, group);
    const HGLOBAL loaded = LoadResource(module, group);
    const auto* data = static_cast<const std::uint8_t*>(LockResource(loaded));
    runner.True(data != nullptr && bytes >= 6, name(L"icon group is readable"));
    if (!data || bytes < 6) return;
    const auto word = [&](const std::size_t offset) {
        return static_cast<unsigned>(data[offset]) |
            (static_cast<unsigned>(data[offset + 1]) << 8);
    };
    const auto dword = [&](const std::size_t offset) {
        return static_cast<DWORD>(word(offset)) |
            (static_cast<DWORD>(word(offset + 2)) << 16);
    };
    constexpr std::array<int, 9> expectedSizes{16, 20, 24, 32, 40, 48, 64, 128, 256};
    const unsigned count = word(4);
    runner.True(word(0) == 0 && word(2) == 1 && count == expectedSizes.size() &&
        bytes >= 6 + 14 * count, name(L"icon group has exactly nine valid entries"));
    if (bytes < 6 + 14 * count) return;
    for (const int expectedSize : expectedSizes) {
        bool embedded = false;
        for (unsigned index = 0; index < count; ++index) {
            const std::size_t entry = 6 + 14 * index;
            const int width = data[entry] == 0 ? 256 : data[entry];
            const int height = data[entry + 1] == 0 ? 256 : data[entry + 1];
            if (width != expectedSize || height != expectedSize) continue;
            const HRSRC image = FindResourceW(module,
                MAKEINTRESOURCEW(word(entry + 12)), RT_ICON);
            embedded = image != nullptr && dword(entry + 8) > 0 &&
                SizeofResource(module, image) == dword(entry + 8);
        }
        const std::wstring sizeName = std::to_wstring(expectedSize) + L"px";
        runner.True(embedded, name(sizeName + L" image resource is embedded"));
        const HICON icon = static_cast<HICON>(LoadImageW(module,
            MAKEINTRESOURCEW(IDI_VIDEOPLAYER), IMAGE_ICON, expectedSize, expectedSize,
            LR_DEFAULTCOLOR));
        runner.True(icon != nullptr, name(sizeName + L" icon can be loaded"));
        ICONINFO info{};
        BITMAP color{};
        BITMAP mask{};
        const bool information = icon && GetIconInfo(icon, &info);
        runner.True(information && info.fIcon && info.hbmColor && info.hbmMask &&
            GetObjectW(info.hbmColor, sizeof(color), &color) == static_cast<int>(sizeof(color)) &&
            GetObjectW(info.hbmMask, sizeof(mask), &mask) == static_cast<int>(sizeof(mask)) &&
            color.bmWidth == expectedSize && color.bmHeight == expectedSize &&
            color.bmBitsPixel == 32 && mask.bmWidth == expectedSize &&
            mask.bmHeight == expectedSize && mask.bmBitsPixel == 1,
            name(sizeName + L" icon color and transparency mask have correct dimensions"));
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        if (expectedSize == 32 || expectedSize == 256) {
            runner.True(icon && IconHasBrandColors(icon),
                name(sizeName + L" icon contains the orange frame and white play symbol"));
        }
        if (icon) DestroyIcon(icon);
    }
}

void TestApplicationIconResources(TestRunner& runner) {
    CheckApplicationIconResources(runner, GetModuleHandleW(nullptr), L"test EXE");
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    runner.True(length > 0 && length < path.size(), L"application icon test resolves build directory");
    if (length == 0 || length >= path.size()) return;
    const std::wstring executablePath(path.data(), length);
    const auto separator = executablePath.find_last_of(L"\\/");
    runner.True(separator != std::wstring::npos, L"application icon test finds sibling player EXE");
    if (separator == std::wstring::npos) return;
    const std::wstring playerPath = executablePath.substr(0, separator + 1) + L"VideoPlayer.exe";
    // Data-file loading does not execute application code or resolve its DLLs.
    const HMODULE player = LoadLibraryExW(playerPath.c_str(), nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    runner.True(player != nullptr, L"built player EXE can be inspected as a resource data file");
    if (!player) return;
    CheckApplicationIconResources(runner, player, L"player EXE");
    FreeLibrary(player);
}

bool HasArgument(const std::vector<std::wstring>& arguments, const std::wstring_view value) {
    for (const auto& argument : arguments) {
        if (argument == value) return true;
    }
    return false;
}

void TestVideoExport(TestRunner& runner) {
    using namespace videoplayer;
    const ExportSourceRegion full = ComputeExportSourceRegion(
        {1920, 1080}, {0, 0, 1920, 1080}, {100, 50, 1700, 950}, {0, 0, 1920, 1080});
    runner.True(full.valid && full.aligned == VideoCrop{0, 0, 1920, 1080},
        L"export geometry maps complete fitted presentation");

    const ExportSourceRegion clipped = ComputeExportSourceRegion(
        {1920, 1080}, {200, 100, 800, 400}, {-100, 0, 1500, 800}, {0, 0, 1000, 800});
    runner.True(clipped.valid, L"export geometry accepts clipped zoom presentation");
    runner.Equal(clipped.aligned, VideoCrop{250, 100, 500, 400},
        L"export geometry maps pan and viewport clipping into source");
    runner.Near(clipped.x, 250.0, 0.001, L"export geometry keeps rational source x");
    runner.True(!ComputeExportSourceRegion(
        {1920, 1080}, {0, 0, 1920, 1080}, {0, 0, 0, 100}, {0, 0, 100, 100}).valid,
        L"export geometry rejects empty presentation");
    const ExportSourceRegion oddSource = ComputeExportSourceRegion(
        {1919, 1079}, {0, 0, 1919, 1079}, {0, 0, 1919, 1079}, {0, 0, 1919, 1079});
    runner.True(oddSource.valid && oddSource.aligned.width == 1918 &&
        oddSource.aligned.height == 1078,
        L"export geometry keeps subsampled crop extents encoder-aligned at odd bounds");
    runner.Equal(ComputeExportPresentationRect(
            {0, 0, 720, 576}, {0, 0, 1000, 1000}, {16, 15}, 0),
        GeometryRect{0, 125, 1000, 875},
        L"export presentation honors non-square SAR without black bars");
    const GeometryRect rotatedPresentation = ComputeExportPresentationRect(
        {0, 0, 1920, 1080}, {0, 0, 1000, 1000}, {1, 1}, 90);
    runner.True(rotatedPresentation.Height() == 1000 &&
        rotatedPresentation.Width() >= 562 && rotatedPresentation.Width() <= 563,
        L"export presentation swaps aspect for metadata rotation");
    const ExportSourceRegion rotated = ComputeExportSourceRegionTransformed(
        {1920, 1080}, {0, 0, 1920, 1080}, {0, 0, 1080, 1920},
        {0, 0, 540, 1920}, 90);
    runner.True(rotated.valid && rotated.aligned == VideoCrop{0, 540, 1920, 540},
        L"export geometry maps rotated display clipping back to coded pixels");

    const std::string flat =
        "streams.stream.0.index=0\n"
        "streams.stream.0.codec_type=\"video\"\n"
        "streams.stream.0.codec_name=\"mjpeg\"\n"
        "streams.stream.0.disposition.attached_pic=1\n"
        "streams.stream.1.index=2\n"
        "streams.stream.1.codec_type=\"video\"\n"
        "streams.stream.1.codec_name=\"h264\"\n"
        "streams.stream.1.profile=\"High\"\n"
        "streams.stream.1.width=1920\n"
        "streams.stream.1.height=1080\n"
        "streams.stream.1.bit_rate=2000000\n"
        "streams.stream.1.sample_aspect_ratio=\"1:1\"\n"
        "streams.stream.1.avg_frame_rate=\"30000/1001\"\n"
        "streams.stream.1.time_base=\"1/90000\"\n"
        "streams.stream.1.pix_fmt=\"yuv420p10le\"\n"
        "streams.stream.1.side_data_list.side_data.0.rotation=90\n"
        "streams.stream.2.index=3\n"
        "streams.stream.2.codec_type=\"audio\"\n"
        "streams.stream.2.codec_name=\"aac\"\n"
        "format.format_name=\"mov,mp4,m4a,3gp,3g2,mj2\"\n"
        "format.duration=12.500000\n"
        "chapters.chapter.0.id=0\n";
    MediaProbeResult probe = ParseFfprobeFlat(flat);
    probe.availableEncoders = {L"libx264", L"libx265", L"mpeg4",
        L"libvpx-vp9", L"libaom-av1", L"aac", L"libopus"};
    runner.True(probe.valid && probe.MainVideo() != nullptr,
        L"ffprobe flat parser selects a main video");
    runner.Equal(probe.MainVideo()->index, 2, L"ffprobe parser ignores attached cover art");
    runner.Equal(probe.MainVideo()->averageFrameRate.numerator,
        static_cast<std::int64_t>(30000), L"ffprobe parser preserves rational fps");
    runner.Equal(probe.MainVideo()->averageFrameRate.denominator,
        static_cast<std::int64_t>(1001), L"ffprobe parser preserves fps denominator");
    runner.Equal(probe.MainVideo()->bitDepth, 10U, L"ffprobe parser derives pixel bit depth");
    runner.Equal(probe.MainVideo()->rotationDegrees, 90, L"ffprobe parser reads display matrix rotation");
    runner.True(probe.HasAudio() && probe.chapterCount == 1,
        L"ffprobe parser inventories audio and chapters");

    ExportSnapshot snapshot{};
    snapshot.sourcePath = L"C:\\media\\source.mp4";
    snapshot.durationMs = 12500;
    snapshot.hasSegment = true;
    snapshot.segmentStartMs = 1000;
    snapshot.segmentEndMs = 4000;
    snapshot.playerVideo = {1920, 1080};
    snapshot.playerCrop = {100, 100, 800, 400};
    ExportOptions options{};
    runner.True(options.resolution == ExportResolution::NativeCrop,
        L"visible-zoom export defaults to a physically cropped frame");
    options.destinationPath = L"C:\\out\\result.mp4";
    ExportPlan plan = BuildExportPlan(snapshot, probe, options, clipped);
    runner.True(plan.valid && plan.streamCopy && HasArgument(plan.ffmpegArguments, L"copy"),
        L"whole full-frame export uses stream copy");
    runner.True(HasArgument(plan.ffmpegArguments, L"0:2") && HasArgument(plan.ffmpegArguments, L"0:a?"),
        L"export planner explicitly maps viewed video and optional audio");
    runner.True(!HasArgument(plan.ffmpegArguments, L"-r"),
        L"export planner never invents a 30 fps rate");

    options.fragment = ExportFragment::Segment;
    const std::vector<std::int64_t> copyBoundaries{0, 1000, 2000, 3000, 4000, 12000};
    plan = BuildExportPlan(snapshot, probe, options, clipped, copyBoundaries);
    runner.True(plan.valid && plan.streamCopy && plan.keyframeAligned && plan.videoEncoder.empty() &&
        HasArgument(plan.ffmpegArguments, L"copy") && !HasArgument(plan.ffmpegArguments, L"libx264") &&
        !HasArgument(plan.ffmpegArguments, L"-c:a"),
        L"A-B without zoom copies both codecs even with legacy Exact method");
    runner.True(HasArgument(plan.ffmpegArguments, L"1.000") &&
        HasArgument(plan.ffmpegArguments, L"3.000"),
        L"aligned copy A-B plan carries start and duration");
    runner.True(std::find(plan.ffmpegArguments.begin(), plan.ffmpegArguments.end(), L"-ss") <
        std::find(plan.ffmpegArguments.begin(), plan.ffmpegArguments.end(), L"-i"),
        L"A-B copy uses input keyframe seeking");
    snapshot.segmentStartMs = 1500;
    snapshot.segmentEndMs = 3500;
    plan = BuildExportPlan(snapshot, probe, options, clipped, {4000, 0, 1000, 4000});
    runner.True(plan.valid && plan.startMs == 1000 && plan.endMs == 4000 &&
        plan.summary.find(L"1.500–3.500") != std::wstring::npos &&
        plan.summary.find(L"1.000–4.000") != std::wstring::npos,
        L"copy A-B expands to safe boundaries and reports requested and actual interval");
    runner.True(!BuildExportPlan(snapshot, probe, options, clipped).valid,
        L"missing keyframe proof never silently triggers A-B reencoding");
    MediaProbeResult noEncoders = probe;
    noEncoders.availableEncoders.clear();
    plan = BuildExportPlan(snapshot, noEncoders, options, clipped, copyBoundaries);
    runner.True(plan.valid && plan.streamCopy && !plan.requiresCodecFallbackConfirmation,
        L"A-B copy needs no installed source video or audio encoder");
    snapshot.segmentStartMs = 1000;
    snapshot.segmentEndMs = 4000;

    options.image = ExportImage::VisibleZoom;
    options.resolution = ExportResolution::SourceCanvas;
    plan = BuildExportPlan(snapshot, probe, options, clipped);
    runner.True(plan.valid && HasArgument(plan.ffmpegArguments, L"-vf"),
        L"visible zoom export emits a crop filter");
    runner.True(HasArgument(plan.ffmpegArguments, L"-noautorotate"),
        L"cropped export disables FFmpeg autorotation before coded crop");
    runner.True(HasArgument(plan.ffmpegArguments, L"crop=500:400:250:100,scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black"),
        L"source-canvas export pads an aspect-safe crop");
    runner.True(plan.outputWidth == 1920 && plan.outputHeight == 1080 &&
        plan.summary.find(L"black padding") != std::wstring::npos,
        L"source-canvas plan explicitly reports retained canvas and padding");

    options.resolution = ExportResolution::NativeCrop;
    plan = BuildExportPlan(snapshot, probe, options, clipped);
    runner.True(plan.valid && plan.outputWidth == 500 && plan.outputHeight == 400 &&
        plan.summary.find(L"фізичний crop") != std::wstring::npos,
        L"native zoom export physically changes encoded frame dimensions");
    runner.True(plan.targetVideoBitRate > 0 && plan.targetVideoBitRate < probe.MainVideo()->bitRate,
        L"native crop scales target bitrate with encoded pixel area");

    options.image = ExportImage::FullFrame;
    options.method = ExportMethod::FastStreamCopy;
    plan = BuildExportPlan(snapshot, probe, options, clipped);
    runner.True(!plan.valid && !plan.error.empty(),
        L"unsafe arbitrary fast A-B copy is rejected without keyframe proof");

    options.fragment = ExportFragment::WholeVideo;
    runner.Equal(ExportMuxerForPath(L"C:\\x\\out.MKV"), std::wstring(L"matroska"),
        L"export muxer is explicit and extension case-insensitive");
    runner.True(ExportMuxerForPath(L"out.avi").empty(),
        L"unsupported output container is rejected");

    auto SetMainCodec = [](MediaProbeResult& value, const std::wstring& codec) {
        for (auto& stream : value.streams) {
            if (stream.codecType == L"video" && !stream.attachedPicture) {
                stream.codecName = codec;
                stream.bitDepth = 8;
                stream.pixelFormat = L"yuv420p";
                return;
            }
        }
    };
    for (const auto& encoderCase : std::array<std::pair<const wchar_t*, const wchar_t*>, 3>{{
             {L"hevc", L"libx265"}, {L"mpeg4", L"mpeg4"}, {L"vp9", L"libvpx-vp9"}}}) {
        MediaProbeResult codecProbe = probe;
        SetMainCodec(codecProbe, encoderCase.first);
        options = {};
        options.fragment = ExportFragment::Segment;
        options.image = ExportImage::VisibleZoom;
        options.audio = ExportAudio::None;
        options.destinationPath = encoderCase.first == std::wstring(L"vp9")
            ? L"C:\\out\\result.webm" : L"C:\\out\\result.mp4";
        plan = BuildExportPlan(snapshot, codecProbe, options, clipped);
        runner.True(plan.valid && plan.videoEncoder == encoderCase.second,
            std::wstring(L"planner selects available source-family encoder for ") + encoderCase.first);
    }

    MediaProbeResult unsupportedCodec = probe;
    SetMainCodec(unsupportedCodec, L"prores");
    options = {};
    options.fragment = ExportFragment::Segment;
    options.image = ExportImage::VisibleZoom;
    options.audio = ExportAudio::None;
    options.destinationPath = L"C:\\out\\fallback.mp4";
    plan = BuildExportPlan(snapshot, unsupportedCodec, options, clipped);
    runner.True(!plan.valid && plan.requiresCodecFallbackConfirmation,
        L"unknown source codec requires explicit H.264 fallback confirmation");
    options.confirmedCodecFallback = true;
    plan = BuildExportPlan(snapshot, unsupportedCodec, options, clipped);
    runner.True(plan.valid && plan.videoEncoder == L"libx264" &&
        plan.expectedVideoCodec == L"h264",
        L"confirmed unknown-codec fallback is explicit H.264 in MP4");

    MediaProbeResult hdrFallback = unsupportedCodec;
    for (auto& stream : hdrFallback.streams) {
        if (stream.codecType == L"video" && !stream.attachedPicture) {
            stream.bitDepth = 10;
            stream.pixelFormat = L"yuv420p10le";
            stream.colorTransfer = L"smpte2084";
        }
    }
    plan = BuildExportPlan(snapshot, hdrFallback, options, clipped);
    runner.True(!plan.valid && !plan.error.empty(),
        L"HDR/10-bit fallback refuses an implicit SDR/8-bit downgrade");

    options.confirmedCodecFallback = false;
    options.destinationPath = L"C:\\out\\incompatible.webm";
    plan = BuildExportPlan(snapshot, probe, options, clipped);
    runner.True(!plan.valid && !plan.error.empty(),
        L"planner rejects an incompatible H.264 WebM container");
}

void TestRealVideoExportIntegration(TestRunner& runner) {
    using namespace videoplayer;
    const std::wstring tools = ExportToolsDirectory();
    const std::wstring ffmpeg = tools + L"\\ffmpeg.exe";
    const std::wstring ffprobe = tools + L"\\ffprobe.exe";
    if (GetFileAttributesW(ffmpeg.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(ffprobe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        runner.True(false, L"real export integration has bundled ffmpeg and ffprobe");
        return;
    }
    wchar_t temporaryRoot[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temporaryRoot)), temporaryRoot) == 0) {
        runner.True(false, L"real export integration obtains temp directory");
        return;
    }
    const std::wstring directory = std::wstring(temporaryRoot) +
        L"VideoPlayer-експорт-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring source = directory + L"\\джерело з пробілами.mp4";
    std::vector<std::wstring> fixtureArgs{
        L"-hide_banner", L"-loglevel", L"error", L"-y",
        L"-f", L"lavfi", L"-i", L"testsrc2=size=320x180:rate=30000/1001",
        L"-f", L"lavfi", L"-i", L"sine=frequency=880:sample_rate=48000",
        L"-t", L"4", L"-c:v", L"libx264", L"-g", L"30",
        L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", L"-shortest",
        L"-f", L"mp4", source};
    ExportProcessResult fixture = RunHiddenExportProcess(ffmpeg, fixtureArgs);
    runner.True(fixture.started && fixture.exitCode == 0,
        L"real export integration generates H.264/AAC fixture");
    MediaProbeResult probe = ProbeMediaFile(ffprobe, source);
    runner.True(probe.valid && probe.HasAudio(),
        L"real export integration probes source streams");
    if (!probe.valid) {
        DeleteFileW(source.c_str());
        RemoveDirectoryW(directory.c_str());
        return;
    }
    std::wstring copyKeyframeError;
    const auto copyKeyframes = ProbeVideoKeyframes(ffprobe, source, probe, nullptr, copyKeyframeError);
    runner.True(copyKeyframeError.empty() && !copyKeyframes.empty(),
        L"real A-B export scans source keyframe packets without decoding");
    ExportSnapshot snapshot{};
    snapshot.sourcePath = source;
    snapshot.sourceIdentity = CaptureExportFileIdentity(source);
    snapshot.durationMs = static_cast<std::int64_t>(probe.durationSeconds * 1000.0 + 0.5);
    snapshot.hasSegment = true;
    snapshot.segmentStartMs = 700;
    snapshot.segmentEndMs = 2600;
    snapshot.playerVideo = {320, 180};
    snapshot.playerCrop = {40, 20, 200, 120};
    snapshot.presentationScreen = {-40, 0, 360, 240};
    snapshot.viewportScreen = {0, 0, 320, 180};
    const ExportSourceRegion region = ComputeExportSourceRegion(
        snapshot.playerVideo, snapshot.playerCrop,
        snapshot.presentationScreen, snapshot.viewportScreen);

    const std::wstring changedSource = directory + L"\\changed-after-snapshot.mp4";
    CopyFileW(source.c_str(), changedSource.c_str(), FALSE);
    ExportSnapshot changedSnapshot = snapshot;
    changedSnapshot.sourcePath = changedSource;
    changedSnapshot.sourceIdentity = CaptureExportFileIdentity(changedSource);
    HANDLE changedFile = CreateFileW(changedSource.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (changedFile != INVALID_HANDLE_VALUE) {
        FILETIME changedTime{};
        GetSystemTimeAsFileTime(&changedTime);
        ULARGE_INTEGER later{};
        later.LowPart = changedTime.dwLowDateTime;
        later.HighPart = changedTime.dwHighDateTime;
        later.QuadPart += 10ULL * 10000000ULL;
        changedTime.dwLowDateTime = later.LowPart;
        changedTime.dwHighDateTime = later.HighPart;
        SetFileTime(changedFile, nullptr, nullptr, &changedTime);
        CloseHandle(changedFile);
    }
    ExportOptions changedOptions{};
    changedOptions.destinationPath = directory + L"\\changed-output.mp4";
    const ExportPlan changedPlan = BuildExportPlan(
        changedSnapshot, probe, changedOptions, region);
    std::atomic_bool changedCancellation{false};
    const ExportJobResult changedResult = RunExportJob(ffmpeg, ffprobe,
        changedSnapshot, changedOptions, changedPlan, false, changedCancellation);
    runner.True(!changedResult.success && !changedResult.diagnostics.empty(),
        L"export refuses a source changed after the immutable dialog snapshot");
    DeleteFileW(changedOptions.destinationPath.c_str());
    DeleteFileW(changedSource.c_str());

    struct Combination final {
        ExportFragment fragment;
        ExportImage image;
        ExportResolution resolution;
        ExportAudio audio;
        const wchar_t* name;
    };
    const std::array<Combination, 4> combinations{{
        {ExportFragment::WholeVideo, ExportImage::FullFrame,
            ExportResolution::SourceCanvas, ExportAudio::Original, L"whole-full"},
        {ExportFragment::Segment, ExportImage::FullFrame,
            ExportResolution::SourceCanvas, ExportAudio::None, L"segment-full"},
        {ExportFragment::WholeVideo, ExportImage::VisibleZoom,
            ExportResolution::NativeCrop, ExportAudio::None, L"whole-crop"},
        {ExportFragment::Segment, ExportImage::VisibleZoom,
            ExportResolution::SourceCanvas, ExportAudio::Original, L"segment-crop"},
    }};
    for (const auto& combination : combinations) {
        ExportOptions options{};
        options.fragment = combination.fragment;
        options.image = combination.image;
        options.resolution = combination.resolution;
        options.audio = combination.audio;
        options.destinationPath = directory + L"\\" + combination.name + L".mp4";
        const ExportPlan plan = BuildExportPlan(snapshot, probe, options, region, copyKeyframes);
        std::atomic_bool cancellation{false};
        const ExportJobResult exported = RunExportJob(ffmpeg, ffprobe, snapshot,
            options, plan, false, cancellation);
        const MediaProbeResult output = ProbeMediaFile(ffprobe, options.destinationPath);
        const MediaStreamInfo* outputVideo = output.MainVideo();
        const bool audioExpected = combination.audio == ExportAudio::Original;
        runner.True(exported.success && output.valid && output.HasAudio() == audioExpected,
            std::wstring(L"real export pipeline validates ") + combination.name);
        if (exported.success && outputVideo != nullptr) {
            std::wcout << L"REAL EXPORT " << combination.name << L": "
                << outputVideo->codecName << L" " << outputVideo->width << L"x"
                << outputVideo->height << L", " << output.durationSeconds
                << L" s, audio=" << (output.HasAudio() ? L"yes" : L"no") << L'\n';
        }
        DeleteFileW(options.destinationPath.c_str());
    }

    // Audio is optional even when "Original" is selected: the source or A-B
    // interval can contain no samples. Still reject loss of real source audio.
    struct CopyPacket final { double pts = 0.0; std::string hash; };
    const auto copyPackets = [&](const std::wstring& file, const wchar_t* selector) {
        std::vector<CopyPacket> packets;
        const auto analysis = RunHiddenExportProcess(ffprobe,
            {L"-v", L"error", L"-select_streams", selector, L"-show_packets",
             L"-show_entries", L"packet=pts_time,data_hash", L"-show_data_hash", L"sha256",
             L"-of", L"compact=p=0:nk=0", file}, nullptr,
            [&](const std::string& line) {
                const auto pts = line.find("pts_time=");
                const auto hash = line.find("data_hash=");
                if (pts == std::string::npos || hash == std::string::npos) return;
                std::string value = line.substr(hash + 10);
                const auto delimiter = value.find_first_of("|\r");
                if (delimiter != std::string::npos) value.resize(delimiter);
                packets.push_back({std::strtod(line.c_str() + pts + 9, nullptr), value});
            });
        if (!analysis.started || analysis.exitCode != 0) packets.clear();
        return packets;
    };
    const auto originalCopyVideo = copyPackets(source, L"v:0");
    const auto originalCopyAudio = copyPackets(source, L"a:0");
    for (const auto* extension : {L".mp4", L".mkv"}) {
        ExportSnapshot copySnapshot = snapshot;
        copySnapshot.segmentStartMs = 1500;
        copySnapshot.segmentEndMs = 2500;
        ExportOptions copyOptions{};
        copyOptions.fragment = ExportFragment::Segment;
        copyOptions.destinationPath = directory + L"\\lossless-segment" + extension;
        const auto copyPlan = BuildExportPlan(copySnapshot, probe, copyOptions, region, copyKeyframes);
        // This source is 30000/1001 fps: 30-frame GOPs last 1001 ms.
        runner.True(copyPlan.valid && copyPlan.streamCopy && copyPlan.startMs == 1001 &&
            copyPlan.endMs == 3003 && copyPlan.expectedAudioCodecs == std::vector<std::wstring>{L"aac"},
            L"real no-zoom A-B keeps source codecs and safe keyframe bounds");
        std::atomic_bool copyCancellation{false};
        const auto copied = RunExportJob(ffmpeg, ffprobe, copySnapshot,
            copyOptions, copyPlan, false, copyCancellation);
        if (!copied.success) std::wcerr << L"A-B COPY FAILURE: " << copied.diagnostics << L'\n';
        const auto copiedProbe = ProbeMediaFile(ffprobe, copyOptions.destinationPath);
        runner.True(copied.success && copiedProbe.valid && copiedProbe.HasAudio() &&
            std::abs(copiedProbe.durationSeconds - 2.0) < 0.05,
            L"real MP4/MKV no-zoom A-B copies and validates source interval");
        std::vector<std::string> expectedHashes, actualHashes;
        for (const auto& packet : originalCopyVideo) {
            if (packet.pts * 1000.0 >= copyPlan.startMs - 0.5 &&
                packet.pts * 1000.0 < copyPlan.endMs - 0.5) expectedHashes.push_back(packet.hash);
        }
        for (const auto& packet : copyPackets(copyOptions.destinationPath, L"v:0")) actualHashes.push_back(packet.hash);
        runner.True(expectedHashes.size() == 60 && actualHashes == expectedHashes,
            L"A-B export video packets are byte-identical to source in encoded order");
        const auto copiedAudio = copyPackets(copyOptions.destinationPath, L"a:0");
        runner.True(!copiedAudio.empty() && std::all_of(copiedAudio.begin(), copiedAudio.end(),
            [&](const CopyPacket& packet) {
                return std::any_of(originalCopyAudio.begin(), originalCopyAudio.end(),
                    [&](const CopyPacket& original) { return packet.hash == original.hash; });
            }), L"A-B export audio packets are copied without AAC reencoding");
        runner.True(CaptureExportFileIdentity(source).size == snapshot.sourceIdentity.size &&
            CaptureExportFileIdentity(source).modifiedTime == snapshot.sourceIdentity.modifiedTime,
            L"lossless A-B export leaves the source untouched");
        DeleteFileW(copyOptions.destinationPath.c_str());
    }

    const auto SameFileIdentity = [](const ExportFileIdentity& before,
                                     const ExportFileIdentity& after) {
        return before.valid && after.valid && before.volume == after.volume &&
            before.fileIndex == after.fileIndex && before.size == after.size &&
            before.modifiedTime == after.modifiedTime;
    };
    const std::wstring videoOnlySource = directory + L"\\video-only-source.mp4";
    const ExportProcessResult videoOnlyFixture = RunHiddenExportProcess(ffmpeg,
        {L"-hide_banner", L"-loglevel", L"error", L"-y", L"-i", source,
         L"-map", L"0:v:0", L"-c", L"copy", L"-an", L"-f", L"mp4",
         videoOnlySource});
    const MediaProbeResult videoOnlyProbe = ProbeMediaFile(ffprobe, videoOnlySource);
    runner.True(videoOnlyFixture.started && videoOnlyFixture.exitCode == 0 &&
        videoOnlyProbe.valid && !videoOnlyProbe.HasAudio(),
        L"real audio regression creates a genuinely video-only source");
    if (videoOnlyProbe.valid && !videoOnlyProbe.HasAudio()) {
        ExportSnapshot videoOnlySnapshot = snapshot;
        videoOnlySnapshot.sourcePath = videoOnlySource;
        videoOnlySnapshot.sourceIdentity = CaptureExportFileIdentity(videoOnlySource);
        videoOnlySnapshot.durationMs = static_cast<std::int64_t>(
            videoOnlyProbe.durationSeconds * 1000.0 + 0.5);
        ExportOptions videoOnlyOptions{};
        videoOnlyOptions.fragment = ExportFragment::Segment;
        videoOnlyOptions.image = ExportImage::FullFrame;
        videoOnlyOptions.audio = ExportAudio::Original;
        videoOnlyOptions.destinationPath = directory + L"\\video-only-segment.mp4";
        ExportPlan videoOnlyPlan = BuildExportPlan(
            videoOnlySnapshot, videoOnlyProbe, videoOnlyOptions, region, copyKeyframes);
        runner.True(videoOnlyPlan.valid && !videoOnlyPlan.outputHasAudio &&
            videoOnlyPlan.expectedAudioCodecs.empty() &&
            HasArgument(videoOnlyPlan.ffmpegArguments, L"-an") &&
            videoOnlyPlan.summary.find(L"Аудіо: відсутнє") != std::wstring::npos,
            L"video-only A-B plan disables audio and reports its absence");
        std::atomic_bool audioCancellation{false};
        const ExportJobResult videoOnlyExport = RunExportJob(ffmpeg, ffprobe,
            videoOnlySnapshot, videoOnlyOptions, videoOnlyPlan, false, audioCancellation);
        const MediaProbeResult videoOnlyOutput = ProbeMediaFile(
            ffprobe, videoOnlyOptions.destinationPath);
        runner.True(videoOnlyExport.success && videoOnlyOutput.valid &&
            !videoOnlyOutput.HasAudio(),
            L"video-only full-frame A-B export succeeds with Original audio selected");
        DeleteFileW(videoOnlyOptions.destinationPath.c_str());

        videoOnlyOptions.destinationPath = directory + L"\\phantom-audio-segment.mp4";
        videoOnlyPlan = BuildExportPlan(
            videoOnlySnapshot, videoOnlyProbe, videoOnlyOptions, region, copyKeyframes);
        videoOnlyPlan.outputHasAudio = true;
        videoOnlyPlan.expectedAudioCodecs = {L"aac"};
        const ExportJobResult phantomAudioExport = RunExportJob(ffmpeg, ffprobe,
            videoOnlySnapshot, videoOnlyOptions, videoOnlyPlan, false, audioCancellation);
        const MediaProbeResult phantomAudioOutput = ProbeMediaFile(
            ffprobe, videoOnlyOptions.destinationPath);
        runner.True(phantomAudioExport.success && phantomAudioOutput.valid &&
            !phantomAudioOutput.HasAudio(),
            L"fresh source probe accepts absent audio despite a phantom audio expectation");
        runner.True(SameFileIdentity(videoOnlySnapshot.sourceIdentity,
            CaptureExportFileIdentity(videoOnlySource)),
            L"video-only audio fallback preserves the original source");
        DeleteFileW(videoOnlyOptions.destinationPath.c_str());
    }

    const std::wstring prefixAudioSource = directory + L"\\short-audio-source.mp4";
    const ExportProcessResult prefixAudioFixture = RunHiddenExportProcess(ffmpeg,
        {L"-hide_banner", L"-loglevel", L"error", L"-y",
         L"-f", L"lavfi", L"-i", L"testsrc2=size=320x180:rate=30:duration=4",
         L"-f", L"lavfi", L"-i", L"sine=frequency=880:sample_rate=48000:duration=1",
         L"-map", L"0:v:0", L"-map", L"1:a:0", L"-t", L"4",
         L"-c:v", L"libx264", L"-preset", L"ultrafast", L"-g", L"15", L"-pix_fmt", L"yuv420p",
         L"-c:a", L"aac", L"-f", L"mp4", prefixAudioSource});
    const MediaProbeResult prefixAudioProbe = ProbeMediaFile(ffprobe, prefixAudioSource);
    runner.True(prefixAudioFixture.started && prefixAudioFixture.exitCode == 0 &&
        prefixAudioProbe.valid && prefixAudioProbe.HasAudio() &&
        prefixAudioProbe.durationSeconds > 3.9,
        L"real audio regression creates four seconds of video with one second of audio");
    if (prefixAudioProbe.valid && prefixAudioProbe.HasAudio()) {
        std::wstring prefixKeyframeError;
        const auto prefixKeyframes = ProbeVideoKeyframes(ffprobe, prefixAudioSource,
            prefixAudioProbe, nullptr, prefixKeyframeError);
        ExportSnapshot prefixAudioSnapshot = snapshot;
        prefixAudioSnapshot.sourcePath = prefixAudioSource;
        prefixAudioSnapshot.sourceIdentity = CaptureExportFileIdentity(prefixAudioSource);
        prefixAudioSnapshot.durationMs = static_cast<std::int64_t>(
            prefixAudioProbe.durationSeconds * 1000.0 + 0.5);
        prefixAudioSnapshot.segmentStartMs = 2000;
        prefixAudioSnapshot.segmentEndMs = 3500;
        ExportOptions prefixAudioOptions{};
        prefixAudioOptions.fragment = ExportFragment::Segment;
        prefixAudioOptions.image = ExportImage::FullFrame;
        prefixAudioOptions.audio = ExportAudio::Original;
        prefixAudioOptions.destinationPath = directory + L"\\after-audio-segment.mp4";
        ExportPlan prefixAudioPlan = BuildExportPlan(
            prefixAudioSnapshot, prefixAudioProbe, prefixAudioOptions, region, prefixKeyframes);
        runner.True(prefixAudioPlan.valid && prefixAudioPlan.outputHasAudio &&
            !prefixAudioPlan.expectedAudioCodecs.empty(),
            L"source audio is initially expected for an A-B interval after it ends");
        std::atomic_bool audioCancellation{false};
        const ExportJobResult afterAudioExport = RunExportJob(ffmpeg, ffprobe,
            prefixAudioSnapshot, prefixAudioOptions, prefixAudioPlan, false, audioCancellation);
        const MediaProbeResult afterAudioOutput = ProbeMediaFile(
            ffprobe, prefixAudioOptions.destinationPath);
        runner.True(afterAudioExport.success && afterAudioOutput.valid &&
            !afterAudioOutput.HasAudio(),
            L"A-B export accepts no audio when the selected interval has no decoded samples");
        DeleteFileW(prefixAudioOptions.destinationPath.c_str());

        prefixAudioSnapshot.segmentStartMs = 100;
        prefixAudioSnapshot.segmentEndMs = 900;
        prefixAudioOptions.destinationPath = directory + L"\\with-audio-segment.mp4";
        prefixAudioPlan = BuildExportPlan(
            prefixAudioSnapshot, prefixAudioProbe, prefixAudioOptions, region, prefixKeyframes);
        const ExportJobResult withAudioExport = RunExportJob(ffmpeg, ffprobe,
            prefixAudioSnapshot, prefixAudioOptions, prefixAudioPlan, false, audioCancellation);
        const MediaProbeResult withAudioOutput = ProbeMediaFile(
            ffprobe, prefixAudioOptions.destinationPath);
        runner.True(withAudioExport.success && withAudioOutput.valid &&
            withAudioOutput.HasAudio(),
            L"A-B export preserves real audio samples inside the selected interval");
        DeleteFileW(prefixAudioOptions.destinationPath.c_str());

        prefixAudioOptions.destinationPath = directory + L"\\protected-audio-destination.mp4";
        const bool copiedDestination = CopyFileW(source.c_str(),
            prefixAudioOptions.destinationPath.c_str(), FALSE) != FALSE;
        const ExportFileIdentity destinationBefore = CaptureExportFileIdentity(
            prefixAudioOptions.destinationPath);
        prefixAudioPlan = BuildExportPlan(
            prefixAudioSnapshot, prefixAudioProbe, prefixAudioOptions, region, prefixKeyframes);
        prefixAudioPlan.ffmpegArguments.push_back(L"-an");
        const ExportJobResult droppedAudioExport = RunExportJob(ffmpeg, ffprobe,
            prefixAudioSnapshot, prefixAudioOptions, prefixAudioPlan, true, audioCancellation);
        runner.True(!droppedAudioExport.success && !droppedAudioExport.cancelled &&
            droppedAudioExport.diagnostics.find(L"Перевірка контейнера") != std::wstring::npos,
            L"audio fallback rejects an output that dropped genuine A-B audio");
        runner.True(copiedDestination && SameFileIdentity(destinationBefore,
            CaptureExportFileIdentity(prefixAudioOptions.destinationPath)) &&
            SameFileIdentity(prefixAudioSnapshot.sourceIdentity,
                CaptureExportFileIdentity(prefixAudioSource)),
            L"rejected genuine-audio loss preserves both source and existing destination");
        runner.True(!droppedAudioExport.partialPath.empty() &&
            GetFileAttributesW(droppedAudioExport.partialPath.c_str()) == INVALID_FILE_ATTRIBUTES,
            L"rejected genuine-audio loss removes its temporary partial output");
        DeleteFileW(prefixAudioOptions.destinationPath.c_str());
    }
    DeleteFileW(prefixAudioSource.c_str());
    DeleteFileW(videoOnlySource.c_str());

    ExportOptions protectionOptions{};
    protectionOptions.destinationPath = source;
    ExportPlan protectionPlan = BuildExportPlan(snapshot, probe, protectionOptions, region);
    std::atomic_bool protectionCancellation{false};
    ExportJobResult protectedSource = RunExportJob(ffmpeg, ffprobe, snapshot,
        protectionOptions, protectionPlan, true, protectionCancellation);
    runner.True(!protectedSource.success && !protectedSource.diagnostics.empty(),
        L"export refuses to overwrite its source path");

    const std::wstring hardLink = directory + L"\\source-hard-link.mp4";
    if (CreateHardLinkW(hardLink.c_str(), source.c_str(), nullptr) != FALSE) {
        protectionOptions.destinationPath = hardLink;
        protectionPlan = BuildExportPlan(snapshot, probe, protectionOptions, region);
        protectionCancellation.store(false);
        ExportJobResult protectedHardLink = RunExportJob(ffmpeg, ffprobe, snapshot,
            protectionOptions, protectionPlan, true, protectionCancellation);
        runner.True(!protectedHardLink.success,
            L"export refuses to overwrite a hard link to source");
        DeleteFileW(hardLink.c_str());
    } else {
        runner.True(false, L"export source protection creates hard-link fixture");
    }

    const std::wstring existing = directory + L"\\existing.mp4";
    CopyFileW(source.c_str(), existing.c_str(), FALSE);
    protectionOptions.destinationPath = existing;
    protectionPlan = BuildExportPlan(snapshot, probe, protectionOptions, region, copyKeyframes);
    protectionCancellation.store(false);
    ExportJobResult unconfirmedOverwrite = RunExportJob(ffmpeg, ffprobe, snapshot,
        protectionOptions, protectionPlan, false, protectionCancellation);
    runner.True(!unconfirmedOverwrite.success,
        L"export refuses an existing destination without overwrite confirmation");

    bool raced = false;
    protectionCancellation.store(false);
    ExportJobResult racedDestination = RunExportJob(ffmpeg, ffprobe, snapshot,
        protectionOptions, protectionPlan, true, protectionCancellation,
        [&](int, const std::wstring&) {
            if (!raced) {
                raced = true;
                DeleteFileW(existing.c_str());
                CopyFileW(source.c_str(), existing.c_str(), FALSE);
            }
        });
    runner.True(!racedDestination.success && raced &&
        GetFileAttributesW(existing.c_str()) != INVALID_FILE_ATTRIBUTES,
        L"export detects destination replacement race before finalization");
    DeleteFileW(existing.c_str());

    protectionOptions.destinationPath = directory + L"\\cancelled.mp4";
    protectionOptions.fragment = ExportFragment::Segment;
    protectionPlan = BuildExportPlan(snapshot, probe, protectionOptions, region, copyKeyframes);
    protectionCancellation.store(false);
    ExportJobResult cancelledJob = RunExportJob(ffmpeg, ffprobe, snapshot,
        protectionOptions, protectionPlan, false, protectionCancellation,
        [&](int, const std::wstring&) { protectionCancellation.store(true); });
    WIN32_FIND_DATAW partialData{};
    HANDLE partialSearch = FindFirstFileW((protectionOptions.destinationPath + L".*.part").c_str(), &partialData);
    const bool partialAbsent = partialSearch == INVALID_HANDLE_VALUE;
    if (partialSearch != INVALID_HANDLE_VALUE) FindClose(partialSearch);
    runner.True(cancelledJob.cancelled && partialAbsent,
        L"cancelled export removes its unique partial file");

    std::atomic_bool cancelImmediately{true};
    ExportProcessResult cancelled = RunHiddenExportProcess(ffmpeg,
        {L"-hide_banner", L"-loglevel", L"error", L"-f", L"lavfi", L"-i",
         L"testsrc2=size=1920x1080:rate=60", L"-t", L"60", L"-f", L"null", L"NUL"},
        &cancelImmediately);
    runner.True(cancelled.started && cancelled.cancelled,
        L"real export process cancellation targets its own job");

    const std::wstring vfrBase = directory + L"\\vfr-sar-base.mp4";
    const std::wstring vfrRotated = directory + L"\\vfr-sar-rotated.mp4";
    ExportProcessResult vfrCreated = RunHiddenExportProcess(ffmpeg,
        {L"-hide_banner", L"-loglevel", L"error", L"-y",
         L"-f", L"lavfi", L"-i", L"testsrc2=size=160x120:rate=30", L"-t", L"3",
         L"-vf", L"select='not(mod(n,2))+not(mod(n,5))',setsar=4/3",
         L"-fps_mode", L"vfr", L"-c:v", L"libx264", L"-pix_fmt", L"yuv420p",
         L"-an", L"-f", L"mp4", vfrBase});
    ExportProcessResult rotationAdded = RunHiddenExportProcess(ffmpeg,
        {L"-hide_banner", L"-loglevel", L"error", L"-y", L"-display_rotation",
         L"90", L"-i", vfrBase, L"-c", L"copy", L"-f", L"mp4", vfrRotated});
    MediaProbeResult transformedProbe = ProbeMediaFile(ffprobe, vfrRotated);
    const MediaStreamInfo* transformedVideo = transformedProbe.MainVideo();
    runner.True(vfrCreated.exitCode == 0 && rotationAdded.exitCode == 0 &&
        transformedVideo != nullptr && !transformedProbe.HasAudio() &&
        transformedVideo->sampleAspectRatio.numerator == 4 &&
        transformedVideo->sampleAspectRatio.denominator == 3 &&
        transformedVideo->averageFrameRate.numerator * transformedVideo->realFrameRate.denominator !=
            transformedVideo->realFrameRate.numerator * transformedVideo->averageFrameRate.denominator &&
        transformedVideo->rotationDegrees == 90,
        L"real ffprobe preserves VFR, no-audio, non-square SAR and rotation metadata");

    if (transformedVideo != nullptr) {
        ExportSnapshot transformedSnapshot{};
        transformedSnapshot.sourcePath = vfrRotated;
        transformedSnapshot.sourceIdentity = CaptureExportFileIdentity(vfrRotated);
        transformedSnapshot.durationMs = static_cast<std::int64_t>(
            transformedProbe.durationSeconds * 1000.0 + 0.5);
        transformedSnapshot.playerVideo = {160, 120};
        transformedSnapshot.playerCrop = {0, 0, 160, 120};
        transformedSnapshot.zoomApplied = true;
        const ExportSourceRegion transformedRegion = ComputeExportSourceRegionTransformed(
            {160, 120}, {0, 0, 160, 120}, {0, 0, 120, 160}, {0, 0, 60, 160}, 90);
        ExportOptions transformedOptions{};
        transformedOptions.image = ExportImage::VisibleZoom;
        transformedOptions.resolution = ExportResolution::NativeCrop;
        transformedOptions.audio = ExportAudio::None;
        transformedOptions.destinationPath = directory + L"\\vfr-sar-rotated-crop.mp4";
        const ExportPlan transformedPlan = BuildExportPlan(transformedSnapshot,
            transformedProbe, transformedOptions, transformedRegion);
        std::atomic_bool transformedCancellation{false};
        const ExportJobResult transformedOutput = RunExportJob(ffmpeg, ffprobe,
            transformedSnapshot, transformedOptions, transformedPlan, false,
            transformedCancellation);
        const MediaProbeResult transformedOutputProbe = ProbeMediaFile(
            ffprobe, transformedOptions.destinationPath);
        const MediaStreamInfo* transformedOutputVideo = transformedOutputProbe.MainVideo();
        runner.True(transformedOutput.success && transformedOutputVideo != nullptr &&
            transformedOutputVideo->rotationDegrees == 90 &&
            transformedOutputVideo->sampleAspectRatio.numerator == 4 &&
            transformedOutputVideo->sampleAspectRatio.denominator == 3,
            L"real cropped export keeps SAR and rotation without double-rotating pixels");
        if (transformedOutput.success && transformedOutputVideo != nullptr) {
            std::wcout << L"REAL EXPORT vfr-sar-rotated-crop: "
                << transformedOutputVideo->codecName << L" "
                << transformedOutputVideo->width << L"x" << transformedOutputVideo->height
                << L", SAR " << transformedOutputVideo->sampleAspectRatio.numerator << L":"
                << transformedOutputVideo->sampleAspectRatio.denominator << L", rotation="
                << transformedOutputVideo->rotationDegrees << L", "
                << transformedOutputProbe.durationSeconds << L" s\n";
        }
        DeleteFileW(transformedOptions.destinationPath.c_str());
    }

    const std::wstring corrupt = directory + L"\\corrupt.mp4";
    CopyFileW(source.c_str(), corrupt.c_str(), FALSE);
    HANDLE corruptFile = CreateFileW(corrupt.c_str(), GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (corruptFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(corruptFile, 64, nullptr, FILE_BEGIN);
        SetEndOfFile(corruptFile);
        CloseHandle(corruptFile);
    }
    runner.True(!ProbeMediaFile(ffprobe, corrupt).valid,
        L"real ffprobe rejects a corrupted/truncated source");
    DeleteFileW(corrupt.c_str());

    std::vector<std::wstring> longDirectories;
    std::wstring longDirectory = directory;
    for (int index = 0; index < 7; ++index) {
        longDirectory += L"\\довгий-сегмент-експорту-0123456789-" + std::to_wstring(index);
        CreateDirectoryW(PathForWin32Io(longDirectory).c_str(), nullptr);
        longDirectories.push_back(longDirectory);
    }
    const std::wstring longSource = longDirectory + L"\\довге джерело.mp4";
    const std::wstring longOutput = longDirectory + L"\\довгий результат.mp4";
    const bool longCopied = CopyFileW(PathForWin32Io(source).c_str(),
        PathForWin32Io(longSource).c_str(), FALSE) != FALSE;
    const MediaProbeResult longProbe = ProbeMediaFile(ffprobe, longSource);
    ExportSnapshot longSnapshot = snapshot;
    longSnapshot.sourcePath = longSource;
    longSnapshot.sourceIdentity = CaptureExportFileIdentity(longSource);
    ExportOptions longOptions{};
    longOptions.destinationPath = longOutput;
    const ExportPlan longPlan = BuildExportPlan(longSnapshot, longProbe, longOptions, region);
    std::atomic_bool longCancellation{false};
    const ExportJobResult longExport = RunExportJob(ffmpeg, ffprobe, longSnapshot,
        longOptions, longPlan, false, longCancellation);
    runner.True(longSource.size() > MAX_PATH && longCopied && longProbe.valid && longExport.success,
        L"real export supports extended Unicode paths longer than MAX_PATH");
    DeleteFileW(PathForWin32Io(longOutput).c_str());
    DeleteFileW(PathForWin32Io(longSource).c_str());
    for (auto item = longDirectories.rbegin(); item != longDirectories.rend(); ++item) {
        RemoveDirectoryW(PathForWin32Io(*item).c_str());
    }

    if (source.size() > 2 && source[1] == L':') {
        const std::wstring uncSource = L"\\\\localhost\\" + source.substr(0, 1) +
            L"$" + source.substr(2);
        if (GetFileAttributesW(uncSource.c_str()) != INVALID_FILE_ATTRIBUTES) {
            runner.True(ProbeMediaFile(ffprobe, uncSource).valid,
                L"real ffprobe supports an available UNC source");
        }
    }
    DeleteFileW(vfrBase.c_str());
    DeleteFileW(vfrRotated.c_str());
    DeleteFileW(source.c_str());
    RemoveDirectoryW(directory.c_str());
}

void TestRemovalPlans(TestRunner& runner) {
    using namespace videoplayer;
    MediaProbeResult probe{};
    probe.valid = true;
    probe.durationSeconds = 10.0;
    MediaStreamInfo video{};
    video.index = 0;
    video.codecType = L"video";
    video.codecName = L"h264";
    probe.streams.push_back(video);
    const std::vector<std::int64_t> keyframes{
        0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000};
    ExportSnapshot snapshot{};
    snapshot.removalIntervals = {{6200, 6800}, {1200, 1800},
        {2100, 2400}, {1100, 1700}};
    auto plan = BuildRemovalPlan(snapshot, probe, keyframes, L"result.mp4");
    runner.True(plan.valid && plan.requestedIntervals.size() == 4 &&
        plan.actualIntervals.size() == 2 && plan.retainedIntervals.size() == 3 &&
        plan.actualIntervals[0].startMs == 1000 && plan.actualIntervals[0].endMs == 3000 &&
        plan.actualIntervals[1].startMs == 6000 && plan.actualIntervals[1].endMs == 7000 &&
        plan.retainedIntervals[0].startMs == 0 && plan.retainedIntervals[0].endMs == 1000 &&
        plan.retainedIntervals[1].startMs == 3000 && plan.retainedIntervals[1].endMs == 6000 &&
        plan.retainedIntervals[2].startMs == 7000 && plan.retainedIntervals[2].endMs == 10000 &&
        plan.expectedDurationMs == 7000,
        L"multi removal sorts and merges aligned duplicate/overlapping/touching cuts");
    snapshot.removalIntervals = {{1200, 1600}, {1750, 1900}};
    plan = BuildRemovalPlan(snapshot, probe, keyframes, L"result.mkv");
    runner.True(plan.valid && plan.actualIntervals.size() == 1 &&
        plan.actualIntervals[0].startMs == 1000 && plan.actualIntervals[0].endMs == 2000 &&
        plan.expectedDurationMs == 9000,
        L"separated requested cuts merging only after keyframe alignment are counted once");
    snapshot.removalIntervals = {{0, 1000}, {9000, 10000}};
    plan = BuildRemovalPlan(snapshot, probe, keyframes, L"result.mp4");
    runner.True(plan.valid && plan.retainedIntervals.size() == 1 &&
        plan.retainedIntervals[0].startMs == 1000 &&
        plan.retainedIntervals[0].endMs == 9000 && plan.expectedDurationMs == 8000,
        L"multi removal excludes empty retained prefix and suffix");
    snapshot.removalIntervals = {{0, 4900}, {5100, 10000}};
    runner.True(!BuildRemovalPlan(snapshot, probe, keyframes, L"result.mp4").valid,
        L"aligned multi-cut union covering entire source is rejected");
    for (const VideoInterval invalid : std::vector<VideoInterval>{
            {-1, 1000}, {1000, 1000}, {2000, 1000}, {10000, 10100}, {1000, 10501}}) {
        snapshot.removalIntervals = {{1000, 2000}, invalid};
        runner.True(!BuildRemovalPlan(snapshot, probe, keyframes, L"result.mp4").valid,
            L"one invalid queued cut rejects the entire operation");
    }
    snapshot.removalIntervals.clear();
    runner.True(!BuildRemovalPlan(snapshot, probe, keyframes, L"result.mp4").valid,
        L"empty removal selection is rejected");
    snapshot.hasSegment = true;
    snapshot.segmentStartMs = 1000;
    snapshot.segmentEndMs = 2000;
    plan = BuildRemovalPlan(snapshot, probe, {9000, 2000, 0, 1000, 1000}, L"result.mp4");
    runner.True(plan.valid && plan.actualIntervals.size() == 1 &&
        plan.actualIntervals[0].startMs == 1000 && plan.actualIntervals[0].endMs == 2000 &&
        plan.expectedDurationMs == 9000,
        L"legacy A-B fallback preserves exact keyframe bounds with unsorted duplicate keyframes");
}

void TestRemovalWithoutReencode(TestRunner& runner) {
    using namespace videoplayer;
    const std::wstring tools = ExportToolsDirectory();
    const std::wstring ffmpeg = tools + L"\\ffmpeg.exe";
    const std::wstring ffprobe = tools + L"\\ffprobe.exe";
    wchar_t temporaryRoot[MAX_PATH]{};
    if (GetTempPathW(static_cast<DWORD>(std::size(temporaryRoot)), temporaryRoot) == 0) {
        runner.True(false, L"removal test obtains temporary directory");
        return;
    }
    const std::wstring directory = std::wstring(temporaryRoot) +
        L"VideoPlayer-removal-" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(directory.c_str(), nullptr);
    const std::wstring source = directory + L"\\джерело.mp4";
    const std::wstring destination = directory + L"\\результат з 'quote'.mp4";
    const auto fixture = RunHiddenExportProcess(ffmpeg,
        {L"-v", L"error", L"-y", L"-f", L"lavfi",
         L"-i", L"testsrc2=size=320x180:rate=30",
         L"-f", L"lavfi", L"-i", L"sine=frequency=440:sample_rate=48000",
         L"-t", L"18", L"-c:v", L"libx264", L"-g", L"30",
         L"-keyint_min", L"30", L"-sc_threshold", L"0",
         L"-pix_fmt", L"yuv420p", L"-c:a", L"aac", L"-shortest",
         L"-f", L"mp4", source});
    runner.True(fixture.started && fixture.exitCode == 0,
        L"removal fixture generated");
    const MediaProbeResult probe = ProbeMediaFile(ffprobe, source);
    runner.True(probe.valid && probe.HasAudio(), L"removal fixture probed");
    if (!probe.valid) {
        DeleteFileW(source.c_str());
        RemoveDirectoryW(directory.c_str());
        return;
    }
    ExportSnapshot snapshot{};
    snapshot.sourcePath = source;
    snapshot.sourceIdentity = CaptureExportFileIdentity(source);
    snapshot.durationMs = 18000;
    snapshot.hasSegment = true;
    snapshot.segmentStartMs = 1400;
    snapshot.segmentEndMs = 2400;
    std::atomic_bool cancellation{false};
    std::wstring keyframeError;
    const auto keyframes = ProbeVideoKeyframes(ffprobe, source, probe,
        &cancellation, keyframeError);
    runner.True(keyframeError.empty() && keyframes.size() >= 5,
        L"removal scans keyframe packet timestamps");
    const RemovalPlan plan = BuildRemovalPlan(snapshot, probe, keyframes, destination);
    runner.True(plan.valid && plan.actualIntervals.size() == 1 &&
        plan.actualIntervals[0].startMs >= 990 &&
        plan.actualIntervals[0].startMs <= 1010 &&
        plan.actualIntervals[0].endMs >= 2990 &&
        plan.actualIntervals[0].endMs <= 3010,
        L"removal rounds A down and B up to keyframes");
    if (plan.valid) {
        const ExportJobResult result = RunRemovalJob(ffmpeg, ffprobe,
            snapshot, probe, plan, destination, false, false, cancellation);
        const MediaProbeResult output = ProbeMediaFile(ffprobe, destination);
        const ExportFileIdentity sourceAfter = CaptureExportFileIdentity(source);
        runner.True(result.success && output.valid &&
            output.MainVideo() != nullptr &&
            output.MainVideo()->codecName == probe.MainVideo()->codecName &&
            output.HasAudio() &&
            std::abs(output.durationSeconds - 16.0) < 0.75 &&
            sourceAfter.fileIndex == snapshot.sourceIdentity.fileIndex &&
            sourceAfter.size == snapshot.sourceIdentity.size,
            L"removal produces stream-copy result and preserves source");
        if (!result.success) {
            std::wcerr << L"REMOVAL FAILURE: " << result.diagnostics << L"\n";
        }
        const ExportJobResult denied = RunRemovalJob(ffmpeg, ffprobe,
            snapshot, probe, plan, source, false, false, cancellation);
        runner.True(!denied.success && !denied.diagnostics.empty(),
            L"removal rejects source-as-new-file");
        ExportSnapshot fromBeginning = snapshot;
        fromBeginning.segmentStartMs = 0;
        fromBeginning.segmentEndMs = 1100;
        const std::wstring onePiecePath = directory + L"\\одна частина.mp4";
        const RemovalPlan onePiecePlan =
            BuildRemovalPlan(fromBeginning, probe, keyframes, onePiecePath);
        const ExportJobResult onePiece = RunRemovalJob(ffmpeg, ffprobe,
            fromBeginning, probe, onePiecePlan, onePiecePath,
            false, false, cancellation);
        runner.True(onePiece.success &&
            ProbeMediaFile(ffprobe, onePiecePath).valid,
            L"removal handles a single retained suffix");
        DeleteFileW(onePiecePath.c_str());

        ExportSnapshot multiple = snapshot;
        // Reverse order ensures the real job consumes the planner's sorted
        // complement rather than the order in which the user added cuts.
        for (int index = 7; index >= 0; --index) {
            multiple.removalIntervals.push_back(
                {index * 2000 + 1200, index * 2000 + 1700});
        }
        const std::wstring multiPath = directory + L"\\кілька відрізків з 'quote'.mp4";
        const RemovalPlan multiPlan = BuildRemovalPlan(multiple, probe, keyframes, multiPath);
        runner.True(multiPlan.valid && multiPlan.actualIntervals.size() == 8 &&
            multiPlan.retainedIntervals.size() == 9 && multiPlan.expectedDurationMs == 10000,
            L"eight requested cuts produce nine ordered retained pieces");
        const ExportJobResult multiResult = RunRemovalJob(ffmpeg, ffprobe,
            multiple, probe, multiPlan, multiPath, false, false, cancellation);
        const MediaProbeResult multiOutput = ProbeMediaFile(ffprobe, multiPath);
        runner.True(multiResult.success && multiOutput.valid &&
            multiOutput.MainVideo() != nullptr &&
            multiOutput.MainVideo()->codecName == probe.MainVideo()->codecName &&
            multiOutput.HasAudio() && std::abs(multiOutput.durationSeconds - 10.0) < 0.05 &&
            std::abs(multiOutput.MainVideo()->durationSeconds - 10.0) < 0.01 &&
            CaptureExportFileIdentity(source).fileIndex == snapshot.sourceIdentity.fileIndex &&
            CaptureExportFileIdentity(source).size == snapshot.sourceIdentity.size,
            L"many-cut stream copy preserves audio/codecs/source without cumulative duration drift");
        if (!multiResult.success) {
            std::wcerr << L"MULTI REMOVAL FAILURE: " << multiResult.diagnostics << L"\n";
        }
        struct HashedPacket final { double pts = 0.0; std::string hash; };
        const auto packets = [&](const std::wstring& path, const wchar_t* selector) {
            std::vector<HashedPacket> parsed;
            const auto process = RunHiddenExportProcess(ffprobe,
                {L"-v", L"error", L"-select_streams", selector, L"-show_packets",
                 L"-show_entries", L"packet=pts_time,data_hash", L"-show_data_hash", L"sha256",
                 L"-of", L"compact=p=0:nk=0", path});
            if (!process.started || process.exitCode != 0) return parsed;
            std::size_t cursor = 0;
            while (cursor < process.standardOutput.size()) {
                const auto end = process.standardOutput.find('\n', cursor);
                const std::string line = process.standardOutput.substr(cursor,
                    end == std::string::npos ? std::string::npos : end - cursor);
                const auto pts = line.find("pts_time=");
                const auto hash = line.find("data_hash=");
                if (pts != std::string::npos && hash != std::string::npos) {
                    std::string value = line.substr(hash + 10);
                    const auto terminator = value.find_first_of("|\r");
                    if (terminator != std::string::npos) value.resize(terminator);
                    parsed.push_back({std::strtod(line.c_str() + pts + 9, nullptr), value});
                }
                if (end == std::string::npos) break;
                cursor = end + 1;
            }
            return parsed;
        };
        const auto sourceVideoPackets = packets(source, L"v:0");
        const auto outputVideoPackets = packets(multiPath, L"v:0");
        std::vector<std::string> expectedVideoHashes, outputVideoHashes;
        for (const auto& packet : sourceVideoPackets) {
            for (const auto& interval : multiPlan.retainedIntervals) {
                const double milliseconds = packet.pts * 1000.0;
                if (milliseconds >= interval.startMs - 0.001 &&
                    milliseconds < interval.endMs - 0.001) {
                    expectedVideoHashes.push_back(packet.hash);
                    break;
                }
            }
        }
        for (const auto& packet : outputVideoPackets) outputVideoHashes.push_back(packet.hash);
        runner.True(expectedVideoHashes.size() == 300 &&
            outputVideoHashes == expectedVideoHashes,
            L"multi-cut output keeps exactly the requested retained encoded video packets in order");
        const auto sourceAudioPackets = packets(source, L"a:0");
        const auto outputAudioPackets = packets(multiPath, L"a:0");
        const bool unchangedAudio = !sourceAudioPackets.empty() && !outputAudioPackets.empty() &&
            std::all_of(outputAudioPackets.begin(), outputAudioPackets.end(),
                [&](const HashedPacket& packet) {
                    return std::any_of(sourceAudioPackets.begin(), sourceAudioPackets.end(),
                        [&](const HashedPacket& original) { return packet.hash == original.hash; });
                });
        runner.True(unchangedAudio,
            L"all retained encoded audio payloads are byte-identical to source packets");
        const auto multiDecoded = RunHiddenExportProcess(ffmpeg,
            {L"-v", L"error", L"-xerror", L"-i", multiPath,
             L"-map", L"0:v:0", L"-map", L"0:a?", L"-f", L"null", L"NUL"});
        runner.True(multiDecoded.started && multiDecoded.exitCode == 0 &&
            multiDecoded.standardError.empty(),
            L"many-cut result decodes cleanly without overlapping timestamp errors");

        for (const auto& muxer : std::vector<std::wstring>{L"matroska", L"webm", L"mp4"}) {
            const bool webm = muxer == L"webm";
            const bool mkv = muxer == L"matroska";
            const std::wstring extension = mkv ? L".mkv" : webm ? L".webm" : L".mp4";
            const std::wstring fixturePath = directory + L"\\джерело перевірки" + extension;
            const std::wstring copiedPath = directory + L"\\результат перевірки" + extension;
            std::vector<std::wstring> fixtureArgs;
            if (webm) {
                fixtureArgs = {L"-v", L"error", L"-y", L"-f", L"lavfi",
                    L"-i", L"testsrc2=size=320x180:rate=30", L"-f", L"lavfi",
                    L"-i", L"sine=frequency=440:sample_rate=48000", L"-t", L"8",
                    L"-c:v", L"libvpx-vp9", L"-deadline", L"realtime", L"-cpu-used", L"8",
                    L"-g", L"30", L"-pix_fmt", L"yuv420p", L"-c:a", L"libopus",
                    L"-shortest", L"-f", muxer, fixturePath};
            } else {
                fixtureArgs = {L"-v", L"error", L"-y", L"-i", source,
                    L"-map", mkv ? L"0" : L"0:v:0", L"-c", L"copy", L"-f", muxer, fixturePath};
            }
            const auto generated = RunHiddenExportProcess(ffmpeg, fixtureArgs);
            const auto containerProbe = ProbeMediaFile(ffprobe, fixturePath);
            runner.True(generated.started && generated.exitCode == 0 && containerProbe.valid &&
                (!mkv || (containerProbe.MainVideo() != nullptr &&
                    containerProbe.MainVideo()->startSeconds - containerProbe.startSeconds > 0.01)),
                L"MKV offset/WebM Opus/video-only regression fixture generated");
            ExportSnapshot containerSnapshot{};
            containerSnapshot.sourcePath = fixturePath;
            containerSnapshot.sourceIdentity = CaptureExportFileIdentity(fixturePath);
            containerSnapshot.removalIntervals = {{1400, 1600}, {3400, 3600}};
            std::wstring scanError;
            const auto boundaries = ProbeVideoKeyframes(ffprobe, fixturePath,
                containerProbe, &cancellation, scanError);
            const auto containerPlan = BuildRemovalPlan(containerSnapshot,
                containerProbe, boundaries, copiedPath);
            const auto copied = RunRemovalJob(ffmpeg, ffprobe, containerSnapshot,
                containerProbe, containerPlan, copiedPath, false, false, cancellation);
            const auto copiedProbe = ProbeMediaFile(ffprobe, copiedPath);
            runner.True(scanError.empty() && containerPlan.valid && copied.success &&
                copiedProbe.valid && copiedProbe.MainVideo() != nullptr &&
                containerProbe.MainVideo() != nullptr &&
                copiedProbe.MainVideo()->codecName == containerProbe.MainVideo()->codecName &&
                copiedProbe.HasAudio() == (mkv || webm) &&
                std::abs(copiedProbe.durationSeconds - containerPlan.expectedDurationMs / 1000.0) < 0.05 &&
                CaptureExportFileIdentity(fixturePath).fileIndex == containerSnapshot.sourceIdentity.fileIndex &&
                CaptureExportFileIdentity(fixturePath).size == containerSnapshot.sourceIdentity.size,
                L"MKV/WebM/video-only multi-cut copying preserves source and duration");
            if (!copied.success) std::wcerr << L"CONTAINER REMOVAL FAILURE: " << copied.diagnostics << L"\n";
            std::vector<std::string> expectedHashes, actualHashes;
            if (containerProbe.MainVideo() != nullptr) {
                for (const auto& packet : packets(fixturePath, L"v:0")) {
                    const double ms = (packet.pts - containerProbe.MainVideo()->startSeconds) * 1000.0;
                    for (const auto& interval : containerPlan.retainedIntervals) {
                        if (ms >= interval.startMs - 0.001 && ms < interval.endMs - 0.001) {
                            expectedHashes.push_back(packet.hash);
                            break;
                        }
                    }
                }
            }
            for (const auto& packet : packets(copiedPath, L"v:0")) actualHashes.push_back(packet.hash);
            runner.True(expectedHashes.size() == (webm ? 180U : 480U) && actualHashes == expectedHashes,
                L"MKV/WebM/video-only cuts retain exact ordered encoded video packets");
            const auto decodedContainer = RunHiddenExportProcess(ffmpeg,
                {L"-v", L"error", L"-xerror", L"-i", copiedPath,
                 L"-map", L"0:v:0", L"-map", L"0:a?", L"-f", L"null", L"NUL"});
            runner.True(decodedContainer.started && decodedContainer.exitCode == 0 &&
                decodedContainer.standardError.empty(),
                L"MKV/WebM/video-only multi-cut result decodes without timestamp errors");
            DeleteFileW(copiedPath.c_str());
            DeleteFileW(fixturePath.c_str());
        }
        std::atomic_bool alreadyCancelled{true};
        const std::wstring cancelledPath = directory + L"\\скасовано.mp4";
        const auto cancelled = RunRemovalJob(ffmpeg, ffprobe, multiple,
            probe, multiPlan, cancelledPath, false, false, alreadyCancelled);
        runner.True(cancelled.cancelled && !cancelled.success &&
            GetFileAttributesW(cancelledPath.c_str()) == INVALID_FILE_ATTRIBUTES,
            L"cancelled multi-cut operation creates no final file");
        const RemovalPlan overwritePlan =
            BuildRemovalPlan(multiple, probe, keyframes, source);
        const ExportJobResult prepared = RunRemovalJob(ffmpeg, ffprobe,
            multiple, probe, overwritePlan, source, true, true, cancellation);
        runner.True(prepared.success && !prepared.partialPath.empty() &&
            GetFileAttributesW(prepared.partialPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
            CaptureExportFileIdentity(source).fileIndex == snapshot.sourceIdentity.fileIndex,
            L"source overwrite remains staged until UI commits verified result");
        if (prepared.success) {
            const bool committed = MoveFileExW(prepared.partialPath.c_str(),
                source.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
            const MediaProbeResult replaced = ProbeMediaFile(ffprobe, source);
            runner.True(committed && replaced.valid &&
                std::abs(replaced.durationSeconds - 10.0) < 0.05,
                L"verified removal can atomically replace source");
        } else {
            std::wcerr << L"REMOVAL PREPARE FAILURE: " << prepared.diagnostics << L"\n";
        }
        DeleteFileW(multiPath.c_str());
    }
    DeleteFileW(destination.c_str());
    DeleteFileW(source.c_str());
    RemoveDirectoryW(directory.c_str());
}

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

void TestPlaybackLoop(TestRunner& runner)
{
    using namespace videoplayer;
    constexpr std::int64_t duration = 10'000;
    const LoopAction none{};
    const auto snapshot = [](
        const std::uint32_t generation,
        const PlaybackState state,
        const std::int64_t positionMs,
        const std::int64_t durationMs,
        const bool seekable) {
        return PlaybackSnapshot{generation, state, positionMs, durationMs, seekable};
    };

    PlaybackLoop markers;
    runner.Equal(markers.Mode(), LoopMode::Off, L"loop starts disabled");
    runner.Equal(markers.StartMs(), std::int64_t{-1}, L"loop A marker starts unset");
    runner.Equal(markers.EndMs(), std::int64_t{-1}, L"loop B marker starts unset");
    runner.True(!markers.SetStart(0, 0), L"A marker rejects unknown duration");
    runner.True(markers.SetStart(-250, duration), L"A marker accepts and clamps negative time");
    runner.Equal(markers.StartMs(), std::int64_t{0}, L"A marker clamps to media start");
    runner.True(!markers.SetEnd(499, duration), L"B marker enforces 500 ms minimum");
    runner.True(markers.SetEnd(500, duration), L"B marker accepts exact minimum segment");
    runner.True(markers.HasSegment(duration) && markers.Mode() == LoopMode::Segment,
        L"valid B marker enables segment loop");
    runner.True(!markers.SetEnd(duration + 1, duration), L"B marker rejects past-duration time");
    runner.Equal(markers.EndMs(), std::int64_t{500}, L"invalid B edit is atomic");
    runner.True(markers.ToggleSegment(duration) && markers.Mode() == LoopMode::Off,
        L"valid segment toggles off without deleting markers");
    runner.True(markers.ToggleSegment(duration) && markers.Mode() == LoopMode::Segment,
        L"stored valid segment toggles back on");
    runner.True(markers.ToggleSegment(0) && markers.Mode() == LoopMode::Off,
        L"active segment can always be switched off during a temporary unknown duration");
    runner.True(markers.ToggleSegment(duration) && markers.Mode() == LoopMode::Segment,
        L"switching segment off retains valid endpoints for later enabling");
    markers.ToggleWholeVideo();
    runner.Equal(markers.Mode(), LoopMode::WholeVideo, L"whole-video mode replaces segment mode");
    markers.ClearSegment();
    runner.True(markers.Mode() == LoopMode::WholeVideo && markers.StartMs() == -1 &&
        markers.EndMs() == -1, L"clearing markers disables only segment mode");
    runner.True(!markers.ToggleSegment(duration) && markers.Mode() == LoopMode::WholeVideo,
        L"invalid segment toggle leaves whole-video mode unchanged");
    markers.ToggleWholeVideo();
    runner.Equal(markers.Mode(), LoopMode::Off, L"whole-video mode toggles off");
    runner.True(markers.SetStart((std::numeric_limits<std::int64_t>::max)(), duration),
        L"A marker accepts a huge position with bounded clamping");
    runner.Equal(markers.StartMs(), duration - 1, L"A marker remains strictly before duration");
    runner.Equal(markers.EndMs(), std::int64_t{-1}, L"editing A clears B");
    markers.Reset();
    runner.True(markers.Mode() == LoopMode::Off && markers.StartMs() == -1 &&
        markers.EndMs() == -1, L"loop reset clears mode and both markers");

    PlaybackLoop whole;
    whole.ToggleWholeVideo();
    runner.Equal(whole.Update(
        snapshot(1, PlaybackState::Playing, 1000, duration, true), 0), none,
        L"first generation bind preserves configured whole-video mode");
    const PlaybackSnapshot unknownEnded =
        snapshot(1, PlaybackState::Ended, 0, 0, false);
    runner.Equal(whole.Update(unknownEnded, 100),
        LoopAction{LoopActionKind::Play, 0},
        L"whole-video loop restarts Ended without duration or seekability");
    runner.Equal(whole.Update(unknownEnded, 200), none,
        L"stale Ended snapshot cannot repeat whole-video Play");
    runner.Equal(whole.Update(
        snapshot(1, PlaybackState::Opening, 0, 0, false), 300), none,
        L"whole-video restart survives Opening acknowledgement gap");
    runner.Equal(whole.Update(
        snapshot(1, PlaybackState::Stopped, 0, 0, false), 400), none,
        L"whole-video restart survives transient Stopped state");
    runner.Equal(whole.Update(
        snapshot(1, PlaybackState::Playing, 0, 0, false), 500), none,
        L"Playing acknowledges whole-video restart");
    runner.Equal(whole.Update(unknownEnded, 600),
        LoopAction{LoopActionKind::Play, 0},
        L"a later Ended transition can restart again after acknowledgement");
    whole.ClearSegment();
    runner.Equal(whole.Update(unknownEnded, 700), none,
        L"clearing unrelated markers preserves whole-video one-shot guard");
    runner.Equal(whole.Update(
        snapshot(1, PlaybackState::Paused, 0, 0, false), 800), none,
        L"Paused never autonomously resumes playback");
    runner.Equal(whole.Update(unknownEnded, 900),
        LoopAction{LoopActionKind::Play, 0},
        L"Paused cancels the old pending restart");
    whole.CancelPending();
    runner.Equal(whole.Update(
        snapshot(1, PlaybackState::Stopped, 0, 0, false), 1000), none,
        L"manual-stop pending cancellation prevents autonomous Play");

    PlaybackLoop segment;
    runner.True(segment.SetStart(1000, duration) && segment.SetEnd(3000, duration),
        L"configure segment loop for stale-snapshot tests");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Playing, 1500, duration, true), 0), none,
        L"first generation bind preserves configured segment");
    const PlaybackSnapshot outside =
        snapshot(2, PlaybackState::Playing, 3000, duration, true);
    runner.Equal(segment.Update(outside, 10),
        LoopAction{LoopActionKind::Seek, 1000},
        L"segment end emits one seek to A");
    runner.Equal(segment.Update(outside, 20), none,
        L"stale outside snapshot cannot emit a seek storm");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Ended, duration, duration, false), 30),
        LoopAction{LoopActionKind::Play, 0},
        L"Ended replaces an unacknowledged segment seek with one restart Play");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Opening, 3000, duration, false), 40), none,
        L"pending segment seek survives transient Opening");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Stopped, 0, duration, false), 50), none,
        L"pending segment seek survives transient Stopped");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Playing, 1000, duration, true), 60),
        LoopAction{LoopActionKind::Seek, 1000},
        L"Playing and seekable after restart defers the segment seek");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Playing, 1000, duration, true), 70), none,
        L"first in-range snapshot starts deferred-restart seek acknowledgement");
    runner.Equal(segment.Update(
        snapshot(2, PlaybackState::Playing, 1100, duration, true), 80), none,
        L"forward in-range progress acknowledges deferred-restart seek");
    runner.Equal(segment.Update(outside, 100),
        LoopAction{LoopActionKind::Seek, 1000},
        L"later segment boundary can emit a new seek after acknowledgement");
    runner.Equal(segment.Update(outside, 5099), none,
        L"pending segment seek remains gated at 4999 ms");
    runner.Equal(segment.Update(outside, 5100), none,
        L"unacknowledged segment seek times out at 5000 ms");
    runner.True(segment.Mode() == LoopMode::Off && segment.StartMs() == 1000 &&
        segment.EndMs() == 3000, L"pending timeout disables mode without deleting markers");
    runner.Equal(segment.Update(outside, 5200), none,
        L"timed-out segment never retries automatically");

    PlaybackLoop restart;
    runner.True(restart.SetStart(2000, duration) && restart.SetEnd(4000, duration),
        L"configure segment loop for asynchronous restart");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 2500, duration, true), 0), none,
        L"in-range segment playback is idle");
    const PlaybackSnapshot segmentEnded =
        snapshot(3, PlaybackState::Ended, duration, duration, false);
    runner.Equal(restart.Update(segmentEnded, 100),
        LoopAction{LoopActionKind::Play, 0},
        L"Ended segment requests Play even after seekability has disappeared");
    runner.Equal(restart.Update(segmentEnded, 150), none,
        L"stale Ended snapshot cannot repeat segment Play");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Opening, 0, duration, false), 200), none,
        L"segment restart waits through Opening");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Stopped, 0, duration, false), 250), none,
        L"segment restart waits through transient Stopped");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 0, duration, false), 300), none,
        L"segment restart waits for seekability after Playing");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 0, duration, true), 350),
        LoopAction{LoopActionKind::Seek, 2000},
        L"Playing and seekable restart defers exactly one seek to A");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 2000, duration, true), 400), none,
        L"first A snapshot does not prematurely release deferred seek guard");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 0, duration, true), 450), none,
        L"stale restart position cannot repeat deferred seek");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 2000, duration, true), 500), none,
        L"A after restart regression begins a fresh acknowledgement");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 2100, duration, true), 550), none,
        L"A then forward progress acknowledges without a duplicate seek");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 4000, duration, true), 600),
        LoopAction{LoopActionKind::Seek, 2000},
        L"segment B boundary repeats after the prior cycle acknowledged");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Paused, 4000, duration, true), 650), none,
        L"Paused cancels a pending segment seek");
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Playing, 4000, duration, true), 700),
        LoopAction{LoopActionKind::Seek, 2000},
        L"resume outside segment starts a fresh seek after pause cancellation");
    restart.CancelPending();
    runner.Equal(restart.Update(
        snapshot(3, PlaybackState::Stopped, 0, duration, true), 750), none,
        L"explicit pending cancellation keeps manual Stop stopped");

    PlaybackLoop restartTimeout;
    runner.True(restartTimeout.SetStart(2000, duration) &&
        restartTimeout.SetEnd(4000, duration),
        L"configure deferred-restart acknowledgement timeout");
    restartTimeout.Update(
        snapshot(31, PlaybackState::Playing, 2500, duration, true), 0);
    restartTimeout.Update(
        snapshot(31, PlaybackState::Ended, duration, duration, false), 100);
    runner.Equal(restartTimeout.Update(
        snapshot(31, PlaybackState::Playing, 0, duration, true), 200),
        LoopAction{LoopActionKind::Seek, 2000},
        L"restart timeout scenario emits its deferred seek");
    runner.Equal(restartTimeout.Update(
        snapshot(31, PlaybackState::Playing, 2000, duration, true), 300), none,
        L"constant A starts but cannot complete progress acknowledgement");
    runner.Equal(restartTimeout.Update(
        snapshot(31, PlaybackState::Playing, 2000, duration, true), 5200), none,
        L"constant A remains action-free at the bounded timeout");
    runner.True(restartTimeout.Mode() == LoopMode::Off &&
        restartTimeout.StartMs() == 2000 && restartTimeout.EndMs() == 4000,
        L"unacknowledged restart seek disables at 5000 ms without losing markers");

    PlaybackLoop gated;
    runner.True(gated.SetStart(1000, duration) && gated.SetEnd(3000, duration),
        L"configure gated segment loop");
    runner.Equal(gated.Update(
        snapshot(4, PlaybackState::Playing, 3000, 0, true), 0), none,
        L"segment loop waits for a known duration");
    runner.Equal(gated.Update(
        snapshot(4, PlaybackState::Playing, 3000, duration, false), 10), none,
        L"segment loop waits for seekability");
    runner.Equal(gated.Update(
        snapshot(4, PlaybackState::Playing, 3000, duration, true), 20, true), none,
        L"suspended seek dragging suppresses loop action");
    runner.Equal(gated.Update(
        snapshot(4, PlaybackState::Playing, 3000, duration, true), 30),
        LoopAction{LoopActionKind::Seek, 1000},
        L"segment enforcement resumes after seek dragging");

    PlaybackLoop failed;
    runner.True(failed.SetStart(1000, duration) && failed.SetEnd(3000, duration),
        L"configure loop before playback error");
    failed.Update(snapshot(5, PlaybackState::Playing, 3000, duration, true), 0);
    runner.Equal(failed.Update(
        snapshot(5, PlaybackState::Error, 3000, duration, false), 10), none,
        L"playback error emits no loop action");
    runner.True(failed.Mode() == LoopMode::Off && failed.StartMs() == -1 &&
        failed.EndMs() == -1,
        L"playback error resets loop configuration and pending action");

    PlaybackLoop generation;
    runner.True(generation.SetStart(1000, duration) && generation.SetEnd(3000, duration),
        L"configure segment before any generation is bound");
    runner.Equal(generation.Update(
        snapshot(20, PlaybackState::Playing, 3000, duration, true), 0),
        LoopAction{LoopActionKind::Seek, 1000},
        L"first Update binds generation without discarding configuration");
    runner.Equal(generation.Update(
        snapshot(21, PlaybackState::Playing, 3000, duration, true), 10), none,
        L"changed media generation emits no stale loop action");
    runner.True(generation.Mode() == LoopMode::Off && generation.StartMs() == -1 &&
        generation.EndMs() == -1,
        L"changed generation resets loop mode, markers and pending action");

    PlaybackLoop first;
    PlaybackLoop second;
    first.ToggleWholeVideo();
    second.SetStart(1000, duration);
    second.SetEnd(3000, duration);
    first.Update(snapshot(30, PlaybackState::Playing, 0, duration, true), 0);
    second.Update(snapshot(30, PlaybackState::Playing, 1500, duration, true), 0);
    runner.Equal(first.Update(
        snapshot(30, PlaybackState::Ended, duration, duration, false), 100),
        LoopAction{LoopActionKind::Play, 0},
        L"first loop instance independently requests whole-video Play");
    runner.Equal(second.Update(
        snapshot(30, PlaybackState::Playing, 3000, duration, true), 100),
        LoopAction{LoopActionKind::Seek, 1000},
        L"second loop instance independently requests segment Seek");
    runner.True(first.Mode() == LoopMode::WholeVideo &&
        second.Mode() == LoopMode::Segment,
        L"independent loop controllers do not share mode or pending state");
}

void TestProgressStrip(TestRunner& runner)
{
    using videoplayer::EvaluateProgressStrip;
    using videoplayer::ProgressStripInput;

    ProgressStripInput input{};
    input.compactMode = true;
    input.toolbarVisible = false;
    input.hasMedia = true;
    input.availableWidth = 1920;
    input.positionMs = 30'000;
    input.durationMs = 60'000;

    auto decision = EvaluateProgressStrip(input);
    runner.True(decision.visible, L"fullscreen hidden-toolbar progress strip is visible");
    runner.Equal(decision.completedWidth, 960, L"progress strip maps midpoint to half width");

    input.compactMode = false;
    decision = EvaluateProgressStrip(input);
    runner.True(!decision.visible, L"ordinary windowed mode hides compact progress strip");
    runner.Equal(decision.completedWidth, 0, L"non-compact windowed strip has no completed width");

    input.compactMode = true;
    decision = EvaluateProgressStrip(input);
    runner.True(decision.visible, L"manually hidden windowed toolbar enables compact progress strip");
    runner.Equal(decision.completedWidth, 960, L"windowed compact progress uses the fullscreen mapping");

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

void TestIntervalTimeline(TestRunner& runner)
{
    using videoplayer::IntervalTimelineRect;
    using videoplayer::IsInsideSavedInterval;
    using videoplayer::OverlapsSavedInterval;
    using videoplayer::VideoInterval;

    const std::vector<VideoInterval> saved{{1000, 2000}, {4000, 5000}};
    runner.True(IsInsideSavedInterval(1500, saved), L"saved range contains interior position");
    runner.True(IsInsideSavedInterval(4500, saved), L"second saved range contains interior position");
    runner.True(!IsInsideSavedInterval(1000, saved), L"saved start endpoint remains selectable");
    runner.True(!IsInsideSavedInterval(2000, saved), L"saved end endpoint remains selectable");
    runner.True(!IsInsideSavedInterval(3000, saved), L"gap between saved ranges remains selectable");
    runner.True(!IsInsideSavedInterval(-1, saved), L"negative position is not inside a saved range");
    runner.True(!IsInsideSavedInterval(1500, {}), L"empty saved range list permits any position");
    const std::vector<VideoInterval> invalid{{-10, 20}, {10, 10}, {30, 20}};
    runner.True(!IsInsideSavedInterval(15, invalid), L"malformed saved ranges do not occupy positions");
    runner.True(!OverlapsSavedInterval({0, 40}, invalid), L"malformed saved ranges do not overlap");
    runner.True(OverlapsSavedInterval({1100, 1900}, saved), L"candidate within saved range overlaps");
    runner.True(OverlapsSavedInterval({0, 6000}, saved), L"candidate enclosing saved ranges overlaps");
    runner.True(OverlapsSavedInterval({1000, 2000}, saved), L"exact duplicate saved range overlaps");
    runner.True(OverlapsSavedInterval({500, 1500}, saved), L"candidate crossing saved start overlaps");
    runner.True(OverlapsSavedInterval({1500, 2500}, saved), L"candidate crossing saved end overlaps");
    runner.True(OverlapsSavedInterval({2000, 4500}, saved), L"candidate crossing a later saved range overlaps");
    runner.True(!OverlapsSavedInterval({0, 1000}, saved), L"candidate touching saved start is allowed");
    runner.True(!OverlapsSavedInterval({2000, 4000}, saved), L"candidate touching two saved endpoints is allowed");
    runner.True(!OverlapsSavedInterval({5000, 6000}, saved), L"candidate touching saved end is allowed");
    runner.True(!OverlapsSavedInterval({2500, 3500}, saved), L"candidate in saved range gap is allowed");
    runner.True(!OverlapsSavedInterval({1000, 2000}, {}), L"empty saved list cannot overlap");
    runner.True(!OverlapsSavedInterval({-1, 1500}, saved), L"negative candidate is malformed");
    runner.True(!OverlapsSavedInterval({1500, 1500}, saved), L"zero-length candidate cannot overlap");
    runner.True(!OverlapsSavedInterval({1800, 1200}, saved), L"reversed candidate cannot overlap");

    const auto expectRect = [&runner](const RECT& actual, const RECT& expected,
        const wchar_t* name) {
        runner.True(actual.left == expected.left && actual.top == expected.top &&
            actual.right == expected.right && actual.bottom == expected.bottom, name);
    };
    const RECT timeline{100, 7, 1100, 13};
    expectRect(IntervalTimelineRect({1000, 2000}, 10000, timeline),
        {200, 7, 300, 13}, L"saved interval maps to exact timeline pixel bounds");
    expectRect(IntervalTimelineRect({0, 10000}, 10000, timeline), timeline,
        L"whole-video interval fills timeline exactly");
    expectRect(IntervalTimelineRect({-1000, 2000}, 10000, timeline),
        {100, 7, 300, 13}, L"interval clips before video start");
    expectRect(IntervalTimelineRect({8000, 12000}, 10000, timeline),
        {900, 7, 1100, 13}, L"interval clips beyond video end");
    expectRect(IntervalTimelineRect({-1000, 12000}, 10000, timeline), timeline,
        L"interval clips at both video endpoints");
    expectRect(IntervalTimelineRect({-2000, -1}, 10000, timeline), {},
        L"interval wholly before video is empty");
    expectRect(IntervalTimelineRect({10000, 12000}, 10000, timeline), {},
        L"interval starting at video end is empty");
    expectRect(IntervalTimelineRect({12000, 14000}, 10000, timeline), {},
        L"interval wholly after video is empty");
    expectRect(IntervalTimelineRect({1, 1}, 10000, timeline), {},
        L"zero-length interval cannot be rendered");
    expectRect(IntervalTimelineRect({2, 1}, 10000, timeline), {},
        L"reversed interval cannot be rendered");
    expectRect(IntervalTimelineRect({0, 1}, 0, timeline), {},
        L"unknown duration cannot be rendered");
    expectRect(IntervalTimelineRect({0, 1}, -1, timeline), {},
        L"negative duration cannot be rendered");
    expectRect(IntervalTimelineRect({0, 1}, 1, {5, 0, 5, 1}), {},
        L"zero-width timeline cannot be rendered");
    expectRect(IntervalTimelineRect({0, 1}, 1, {6, 0, 5, 1}), {},
        L"reversed-width timeline cannot be rendered");
    expectRect(IntervalTimelineRect({0, 1}, 1, {0, 5, 10, 5}), {},
        L"zero-height timeline cannot be rendered");
    expectRect(IntervalTimelineRect({0, 1}, 1, {0, 6, 10, 5}), {},
        L"reversed-height timeline cannot be rendered");
    expectRect(IntervalTimelineRect({1, 2}, 3, {0, 0, 1001, 2}),
        {333, 0, 668, 2}, L"odd-width fractional mapping uses floor start and ceil end");
    expectRect(IntervalTimelineRect({1, 2}, 10000, timeline),
        {100, 7, 101, 13}, L"subpixel interval retains one visible pixel");
    expectRect(IntervalTimelineRect({9998, 9999}, 10000, timeline),
        {1099, 7, 1100, 13}, L"subpixel interval at video end stays within timeline");
    expectRect(IntervalTimelineRect({1000, 2000}, 10000, {4, 8, 5, 9}),
        {4, 8, 5, 9}, L"one-pixel timeline displays any positive interval");
    constexpr std::int64_t day = 24LL * 60 * 60 * 1000;
    expectRect(IntervalTimelineRect({3 * day, 4 * day}, 10 * day, timeline),
        {400, 7, 500, 13}, L"multiday timeline uses exact millisecond ratios");
    const std::int64_t maxTime = (std::numeric_limits<std::int64_t>::max)();
    const LONG maxCoord = (std::numeric_limits<LONG>::max)();
    const LONG minCoord = (std::numeric_limits<LONG>::min)();
    const RECT hugeTimeline{0, 0, maxCoord, 1};
    expectRect(IntervalTimelineRect({maxTime - 1, maxTime}, maxTime, hugeTimeline),
        {maxCoord - 1, 0, maxCoord, 1},
        L"INT64 maximum duration maps end subpixel without overflow or rounding");
    expectRect(IntervalTimelineRect({1, 2}, maxTime, hugeTimeline),
        {0, 0, 1, 1}, L"INT64 maximum duration maps start subpixel exactly");
    expectRect(IntervalTimelineRect({0, maxTime}, maxTime, hugeTimeline), hugeTimeline,
        L"INT64 maximum duration whole interval fills maximum valid width");
    const RECT negativeTimeline{minCoord, minCoord, -1, maxCoord};
    expectRect(IntervalTimelineRect({0, maxTime}, maxTime, negativeTimeline), negativeTimeline,
        L"extreme negative timeline coordinates and height do not overflow");
    expectRect(IntervalTimelineRect({0, 1}, 1, {minCoord, 0, maxCoord, 1}), {},
        L"timeline wider than INT_MAX is rejected without subtraction overflow");
    expectRect(IntervalTimelineRect({(std::numeric_limits<std::int64_t>::min)(), maxTime},
        maxTime, timeline), timeline, L"extreme signed interval clips without duration subtraction");
    const std::vector<VideoInterval> extremeSaved{{maxTime - 10, maxTime}};
    runner.True(IsInsideSavedInterval(maxTime - 1, extremeSaved),
        L"saved membership handles INT64 maximum end");
    runner.True(OverlapsSavedInterval({maxTime - 20, maxTime - 1}, extremeSaved),
        L"saved overlap handles INT64 maximum without subtraction");
    runner.True(saved.size() == 2 && saved[0].startMs == 1000 &&
        saved[0].endMs == 2000 && saved[1].startMs == 4000 && saved[1].endMs == 5000,
        L"interval helpers preserve original saved ranges");
    expectRect(timeline, {100, 7, 1100, 13}, L"interval mapping preserves supplied timeline");
    runner.Equal(videoplayer::IntervalTimelineX(0, 10000, timeline), 100L, L"A marker maps video start");
    runner.Equal(videoplayer::IntervalTimelineX(10000, 10000, timeline), 1100L, L"B marker maps video end");
    runner.Equal(videoplayer::IntervalTimelineX(5000, 10000, timeline), 600L, L"marker maps midpoint");
    runner.Equal(videoplayer::IntervalTimelineX(maxTime, maxTime, hugeTimeline), maxCoord,
        L"marker handles int64 maximum duration without overflow");
    videoplayer::PlaybackLoop edit;
    edit.SetStart(1000, 10000);
    runner.True(edit.MovePoint(true, 2000, 10000) && edit.EndMs() == -1,
        L"moving standalone A does not invent B");
    runner.True(!edit.MovePoint(false, 4000, 10000), L"unset B cannot be dragged");
    edit.SetEnd(5000, 10000);
    runner.True(edit.MovePoint(true, 3000, 10000) && edit.EndMs() == 5000 && edit.Mode() == videoplayer::LoopMode::Segment,
        L"moving A preserves B and enabled loop mode");
    runner.True(!edit.MovePoint(true, 4600, 10000) && edit.StartMs() == 3000,
        L"invalid short marker edit is atomic");
    edit.ToggleSegment(10000);
    runner.True(edit.MovePoint(false, 10000, 10000) && edit.Mode() == videoplayer::LoopMode::Off,
        L"moving B to duration preserves disabled loop mode");
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

void TestVideoDisplayGeometry(TestRunner& runner)
{
    using namespace videoplayer;

    const VideoDimensions widescreen{1600, 900};
    const GeometryRect wideViewport{0, 0, 1000, 400};
    const GeometryRect fittedWide{144, 0, 855, 400};
    runner.Equal(
        ComputeVideoDisplayRect(widescreen, wideViewport, {}),
        fittedWide,
        L"default display state is the ordinary fitted crop rectangle");

    const VideoDisplayState fillWide = ResizeVideoDisplay(
        {}, widescreen, wideViewport, fittedWide, VideoEdge::Right,
        {fittedWide.right, 200}, {wideViewport.right, 200});
    runner.Near(
        fillWide.scale,
        1000.0 / 711.0,
        0.000001,
        L"outward edge resize reaches pillarbox cover scale");
    const GeometryRect filledWide =
        ComputeVideoDisplayRect(widescreen, wideViewport, fillWide);
    runner.True(
        filledWide.left == wideViewport.left &&
            filledWide.right == wideViewport.right &&
            filledWide.top <= wideViewport.top &&
            filledWide.bottom >= wideViewport.bottom,
        L"16:9 presentation fills a wider viewport without blank gaps");

    const GeometryRect matchingViewport{0, 0, 800, 450};
    const GeometryRect matchingGrip{0, 0, 800, 450};
    const VideoDisplayState shrunk = ResizeVideoDisplay(
        {}, {800, 450}, matchingViewport, matchingGrip, VideoEdge::Right,
        {800, 225}, {600, 225});
    runner.Near(shrunk.scale, 0.5, 0.000001,
        L"inward edge drag shrinks symmetrically from immutable start geometry");
    runner.Equal(
        ComputeVideoDisplayRect({800, 450}, matchingViewport, shrunk),
        GeometryRect{200, 112, 600, 337},
        L"shrunk presentation stays centered on both smaller axes");

    runner.Equal(
        ComputeVideoDisplayRect(
            {1080, 1920}, {0, 0, 1000, 500}, {1.5, 0.5, 0.5}),
        GeometryRect{289, -125, 711, 625},
        L"portrait presentation scales uniformly around the viewport center");

    const VideoDisplayState clampedCenter = NormalizeVideoDisplayState(
        {1920, 1080}, {0, 0, 800, 600}, {2.0, -5.0, 9.0});
    runner.Near(clampedCenter.centerX, 0.25, 0.000001,
        L"oversized horizontal center clamps against the left blank-gap limit");
    runner.Near(clampedCenter.centerY, 2.0 / 3.0, 0.000001,
        L"oversized vertical center clamps against the bottom blank-gap limit");
    runner.Equal(
        ComputeVideoDisplayRect(
            {1920, 1080}, {0, 0, 800, 600}, clampedCenter),
        GeometryRect{0, -300, 1600, 600},
        L"clamped source fractions place their crop point under viewport center");

    const VideoDisplayState centeredSmall = NormalizeVideoDisplayState(
        {1920, 1080}, {0, 0, 800, 600}, {0.5, 0.0, 1.0});
    runner.Near(centeredSmall.centerX, 0.5, 0.000001,
        L"smaller horizontal presentation ignores stale pan center");
    runner.Near(centeredSmall.centerY, 0.5, 0.000001,
        L"smaller vertical presentation ignores stale pan center");
    runner.Near(
        NormalizeVideoDisplayState(
            {800, 450}, matchingViewport, {-100.0, 0.5, 0.5}).scale,
        0.25,
        0.000001,
        L"display scale clamps to one quarter");
    runner.Near(
        NormalizeVideoDisplayState(
            {100, 100}, {0, 0, 100, 100}, {100.0, 0.5, 0.5}).scale,
        8.0,
        0.000001,
        L"ordinary display scale clamps to eight");

    const VideoDisplayState canvasBound = NormalizeVideoDisplayState(
        {1, 1}, {0, 0, 4096, 4096}, {8.0, 0.5, 0.5});
    runner.Near(canvasBound.scale, 4.0, 0.000001,
        L"canvas allocation bound lowers the maximum scale");
    const GeometryRect canvasRect = ComputeVideoDisplayRect(
        {1, 1}, {0, 0, 4096, 4096}, canvasBound);
    runner.True(
        canvasRect.Width() == 16384 && canvasRect.Height() == 16384,
        L"display canvas dimensions never exceed 16384 pixels");

    const GeometryRect content{100, 100, 700, 500};
    const GeometryRect viewport{0, 0, 800, 600};
    runner.Equal(
        HitTestVideoEdge(content, viewport, {104, 105}, 8),
        VideoEdge::TopLeft,
        L"edge hit testing gives visible corners priority");
    runner.Equal(
        HitTestVideoEdge(content, viewport, {696, 300}, 8),
        VideoEdge::Right,
        L"edge hit testing recognizes a visible side");
    runner.Equal(
        HitTestVideoEdge(content, viewport, {300, 493}, 8),
        VideoEdge::Bottom,
        L"edge hit testing recognizes a visible bottom border");
    runner.Equal(
        HitTestVideoEdge(content, viewport, {-1, 100}, 8),
        VideoEdge::None,
        L"edge hit testing rejects points outside the viewport");

    const GeometryRect clippedContent{-200, 50, 600, 450};
    runner.Equal(
        HitTestVideoEdge(clippedContent, {0, 0, 500, 500}, {0, 200}, 8),
        VideoEdge::Left,
        L"visible intersection boundary remains a grip for clipped content");
    runner.Equal(
        HitTestVideoEdge(clippedContent, {0, 0, 500, 500}, {250, 54}, 8),
        VideoEdge::Top,
        L"visible portion of a partially clipped content border remains hittable");
    runner.Equal(
        HitTestVideoEdge(
            {-200, 50, 300, 450}, {0, 0, 500, 500}, {296, 54}, 8),
        VideoEdge::TopRight,
        L"visible clipped-border corner still has corner priority");
    runner.Equal(
        HitTestVideoEdge(
            {-100, -100, 900, 700}, viewport, {0, 300}, 8),
        VideoEdge::Left,
        L"oversized content on both axes retains a visible resize grip");
    runner.Equal(
        HitTestVideoEdge(
            {-100, -100, 900, 700}, viewport, {799, 599}, 0),
        VideoEdge::BottomRight,
        L"half-open visible border supports exact zero-tolerance corner hits");
    runner.Equal(
        HitTestVideoEdge(
            {-100, -100, 900, 700}, viewport, {800, 599}, 8),
        VideoEdge::None,
        L"oversized visible grip still rejects a point outside viewport");
    runner.Equal(
        HitTestVideoEdge(
            {100, 100, 110, 110}, viewport, {108, 108}, 20),
        VideoEdge::BottomRight,
        L"overlapping tolerance selects nearest opposing borders");

    const int dpiTolerance = ScaleLogicalPixels(6, 144);
    runner.Equal(
        HitTestVideoEdge(content, viewport, {91, 300}, dpiTolerance),
        VideoEdge::Left,
        L"caller-provided DPI-scaled device-pixel tolerance is honored exactly");
    runner.Equal(
        HitTestVideoEdge(content, viewport, {90, 300}, dpiTolerance),
        VideoEdge::None,
        L"one pixel beyond supplied hit tolerance is rejected");

    const VideoDimensions twoToOne{800, 400};
    const GeometryRect resizeViewport{0, 0, 800, 600};
    const VideoDisplayState halfSize{0.5, 0.5, 0.5};
    const GeometryRect halfGrip{200, 200, 600, 400};
    const VideoDisplayState cornerResize = ResizeVideoDisplay(
        halfSize, twoToOne, resizeViewport, halfGrip,
        VideoEdge::BottomRight, {600, 400}, {700, 450});
    runner.Near(cornerResize.scale, 0.75, 0.000001,
        L"corner drag projects both pointer axes onto one uniform scale");
    runner.Equal(
        ComputeVideoDisplayRect(twoToOne, resizeViewport, cornerResize),
        GeometryRect{100, 150, 700, 450},
        L"corner resizing preserves crop aspect ratio and fixed center");

    const VideoDisplayState topResize = ResizeVideoDisplay(
        halfSize, twoToOne, resizeViewport, halfGrip,
        VideoEdge::Top, {400, 200}, {400, 150});
    runner.Near(topResize.scale, 0.75, 0.000001,
        L"top-edge outward sign increases scale");
    const VideoDisplayState minimumResize = ResizeVideoDisplay(
        halfSize, twoToOne, resizeViewport, halfGrip,
        VideoEdge::Right, {600, 300}, {-5000, 300});
    runner.Near(minimumResize.scale, 0.25, 0.000001,
        L"current pointer clamps into viewport before minimum-scale clamp");
    const VideoDisplayState coverLimited = ResizeVideoDisplay(
        {}, twoToOne, resizeViewport, {0, 100, 800, 500},
        VideoEdge::Top, {400, 100}, {400, 0});
    runner.Near(coverLimited.scale, 1.5, 0.000001,
        L"outward resize stops at the crop cover scale");
    const VideoDisplayState existingLarger{2.0, 0.5, 0.5};
    const VideoDisplayState cannotGrowPastStart = ResizeVideoDisplay(
        existingLarger, twoToOne, resizeViewport,
        ComputeVideoDisplayRect(twoToOne, resizeViewport, existingLarger),
        VideoEdge::Right, {799, 300}, {800, 300});
    runner.Near(cannotGrowPastStart.scale, 2.0, 0.000001,
        L"resize never grows beyond an already-larger drag-start scale");

    const GeometryRect negativeViewport{-500, -200, 500, 200};
    const GeometryRect negativeGrip{-356, -200, 355, 200};
    const VideoDisplayState negativeFill = ResizeVideoDisplay(
        {}, widescreen, negativeViewport, negativeGrip, VideoEdge::Right,
        {negativeGrip.right, 0}, {negativeViewport.right, 0});
    runner.Equal(
        ComputeVideoDisplayRect(widescreen, negativeViewport, negativeFill),
        GeometryRect{-500, -282, 500, 281},
        L"negative screen coordinates retain bounded cover geometry");

    const VideoDisplayState anchored = NormalizeVideoDisplayState(
        {1920, 1080}, viewport, {1.5, 0.0, 0.5});
    const GeometryRect anchoredRect =
        ComputeVideoDisplayRect({1920, 1080}, viewport, anchored);
    const VideoDisplayState unchangedAnchor = ResizeVideoDisplay(
        anchored, {1920, 1080}, viewport, anchoredRect, VideoEdge::Left,
        {anchoredRect.left, 300}, {anchoredRect.left, 300});
    runner.Near(unchangedAnchor.scale, anchored.scale, 0.000001,
        L"returning to immutable mouse anchor restores exact start scale");
    runner.Near(unchangedAnchor.centerX, anchored.centerX, 0.000001,
        L"returning to mouse anchor has no horizontal center drift");
    runner.Near(unchangedAnchor.centerY, anchored.centerY, 0.000001,
        L"returning to mouse anchor has no vertical center drift");

    const double nan = (std::numeric_limits<double>::quiet_NaN)();
    const VideoDisplayState sanitized = NormalizeVideoDisplayState(
        {800, 450}, matchingViewport,
        {nan, (std::numeric_limits<double>::infinity)(), nan});
    runner.True(
        std::isfinite(sanitized.scale) && sanitized.scale == 1.0 &&
            sanitized.centerX == 0.5 && sanitized.centerY == 0.5,
        L"non-finite display state sanitizes to safe defaults");
    runner.Equal(
        ComputeVideoDisplayRect({}, viewport, {}),
        GeometryRect{},
        L"invalid crop size has no display rectangle");
    runner.Equal(
        HitTestVideoEdge(content, viewport, {100, 300}, -1),
        VideoEdge::None,
        L"negative hit tolerance is invalid");
    const VideoDisplayState invalidResize = ResizeVideoDisplay(
        {nan, nan, nan}, {}, viewport, {}, VideoEdge::None, {}, {});
    runner.True(
        std::isfinite(invalidResize.scale) && invalidResize.scale == 1.0 &&
            invalidResize.centerX == 0.5 && invalidResize.centerY == 0.5,
        L"invalid resize input returns a finite normalized default state");
}

void TestVideoAreaResize(TestRunner& runner)
{
    using namespace videoplayer;

    const auto nearRect = [&runner](
        const GeometryRect actual,
        const GeometryRect expected,
        const wchar_t* const name) {
        runner.True(
            std::abs(actual.left - expected.left) <= 2 &&
                std::abs(actual.top - expected.top) <= 2 &&
                std::abs(actual.right - expected.right) <= 2 &&
                std::abs(actual.bottom - expected.bottom) <= 2,
            name);
    };
    const auto cropIsBounded = [](const VideoDimensions video, const VideoCrop crop) {
        return crop.IsValid() && crop.x < video.width && crop.y < video.height &&
            crop.width <= video.width - crop.x &&
            crop.height <= video.height - crop.y;
    };

    const VideoDimensions video{1920, 1080};
    const GeometryRect viewport{0, 0, 800, 600};
    const VideoCrop startCrop{760, 390, 400, 300};
    const VideoDisplayState startState{0.5, 0.5, 0.5};
    const GeometryRect startContent{200, 150, 600, 450};

    const VideoAreaResizeResult right = ResizeVideoArea(
        video, startCrop, startState, viewport, startContent,
        VideoEdge::Right, {600, 300}, {700, 300});
    runner.True(right.valid, L"right-edge area resize produces a result");
    runner.Equal(
        right.crop, VideoCrop{660, 390, 600, 300},
        L"right-edge area resize changes source width but preserves height");
    nearRect(
        ComputeVideoDisplayRect(
            {right.crop.width, right.crop.height}, viewport, right.display),
        {100, 150, 700, 450},
        L"right-edge area resize changes only the rendered width");

    const VideoAreaResizeResult top = ResizeVideoArea(
        video, startCrop, startState, viewport, startContent,
        VideoEdge::Top, {400, 150}, {400, 100});
    runner.True(top.valid, L"top-edge area resize produces a result");
    runner.Equal(
        top.crop, VideoCrop{760, 340, 400, 400},
        L"top-edge area resize changes source height but preserves width");
    nearRect(
        ComputeVideoDisplayRect(
            {top.crop.width, top.crop.height}, viewport, top.display),
        {200, 100, 600, 500},
        L"top-edge area resize changes only the rendered height");

    const VideoAreaResizeResult corner = ResizeVideoArea(
        video, startCrop, startState, viewport, startContent,
        VideoEdge::BottomRight, {600, 450}, {700, 500});
    runner.True(corner.valid, L"corner area resize produces a result");
    runner.Equal(
        corner.crop, VideoCrop{660, 340, 600, 400},
        L"corner area resize changes width and height independently");
    nearRect(
        ComputeVideoDisplayRect(
            {corner.crop.width, corner.crop.height}, viewport, corner.display),
        {100, 100, 700, 500},
        L"corner resize remains an undistorted uniform-density rectangle");

    const VideoAreaResizeResult shrink = ResizeVideoArea(
        video, startCrop, startState, viewport, startContent,
        VideoEdge::Right, {600, 300}, {500, 300});
    runner.Equal(
        shrink.crop, VideoCrop{860, 390, 200, 300},
        L"inward area resize symmetrically shrinks the affected crop axis");
    nearRect(
        ComputeVideoDisplayRect(
            {shrink.crop.width, shrink.crop.height}, viewport, shrink.display),
        {300, 150, 500, 450},
        L"inward area resize preserves the unaffected rendered height");

    const VideoCrop zoomedOut = ZoomVideoCrop(
        video, right.crop, (std::numeric_limits<double>::max)(),
        0.5, 0.5, {right.crop.width, right.crop.height});
    runner.Equal(
        zoomedOut, VideoCrop{0, 60, 1920, 960},
        L"wheel zoom can expand a resized user aspect to its source limit");

    const GeometryRect wideViewport{0, 0, 800, 450};
    const VideoAreaResizeResult sourceLimited = ResizeVideoArea(
        video, {0, 0, 1920, 1080}, {}, wideViewport, wideViewport,
        VideoEdge::Top, {400, 0}, {400, 75});
    runner.True(sourceLimited.valid,
        L"area resize remains valid when one source axis is already full");
    runner.Equal(
        sourceLimited.crop, VideoCrop{0, 180, 1920, 720},
        L"full-source axis forces uniform density and crops the other axis");
    nearRect(
        ComputeVideoDisplayRect(
            {sourceLimited.crop.width, sourceLimited.crop.height},
            wideViewport, sourceLimited.display),
        {0, 75, 800, 375},
        L"source-limited resize does not distort either axis");

    const GeometryRect floorViewport{0, 0, 1000, 400};
    const VideoCrop floorCrop{210, 340, 1500, 400};
    const VideoDisplayState floorState{0.3, 0.5, 0.5};
    const GeometryRect floorContent{350, 160, 650, 240};
    const VideoAreaResizeResult scaleFloor = ResizeVideoArea(
        video, floorCrop, floorState, floorViewport, floorContent,
        VideoEdge::Right, {650, 200}, {500, 200});
    runner.True(scaleFloor.valid,
        L"area resize remains valid at the display scale floor");
    nearRect(
        ComputeVideoDisplayRect(
            {scaleFloor.crop.width, scaleFloor.crop.height},
            floorViewport, scaleFloor.display),
        {375, 160, 625, 240},
        L"scale-floor clamping never enlarges the unaffected short axis");

    const VideoDisplayState overscanState{2.0, 0.25, 2.0 / 3.0};
    const GeometryRect overscanContent = ComputeVideoDisplayRect(
        video, viewport, overscanState);
    const VideoAreaResizeResult perpendicularOnly = ResizeVideoArea(
        video, {0, 0, 1920, 1080}, overscanState, viewport,
        overscanContent, VideoEdge::Right, {799, 300}, {799, 100});
    runner.True(perpendicularOnly.valid,
        L"perpendicular-only edge motion is a valid no-op");
    runner.Equal(
        perpendicularOnly.crop, VideoCrop{0, 0, 1920, 1080},
        L"perpendicular-only edge motion preserves exact source crop");
    runner.True(
        perpendicularOnly.display.scale == overscanState.scale &&
            perpendicularOnly.display.centerX == overscanState.centerX &&
            perpendicularOnly.display.centerY == overscanState.centerY,
        L"perpendicular-only edge motion preserves exact display state");

    const GeometryRect negativeViewport{-500, -300, 300, 300};
    const VideoCrop oddStart{760, 340, 400, 400};
    const VideoDisplayState negativeState{2.0 / 3.0, 0.5, 0.5};
    const GeometryRect negativeContent{-300, -200, 100, 200};
    const VideoAreaResizeResult negative = ResizeVideoArea(
        {1919, 1079}, oddStart, negativeState, negativeViewport,
        negativeContent, VideoEdge::Right, {100, 0}, {200, 0});
    runner.True(
        negative.valid && cropIsBounded({1919, 1079}, negative.crop) &&
            (negative.crop.width == 1919U || (negative.crop.width & 1U) == 0U) &&
            (negative.crop.height == 1079U || (negative.crop.height & 1U) == 0U),
        L"negative coordinates and odd source bounds yield an aligned valid crop");
    nearRect(
        ComputeVideoDisplayRect(
            {negative.crop.width, negative.crop.height},
            negativeViewport, negative.display),
        {-400, -200, 200, 200},
        L"negative viewport coordinates preserve the requested frame");

    const VideoDimensions tinyVideo{15, 11};
    const GeometryRect tinyViewport{0, 0, 120, 100};
    const GeometryRect tinyContent = ComputeVideoDisplayRect(
        tinyVideo, tinyViewport, {0.5, 0.5, 0.5});
    const VideoAreaResizeResult tiny = ResizeVideoArea(
        tinyVideo, {0, 0, 15, 11}, {0.5, 0.5, 0.5}, tinyViewport,
        tinyContent, VideoEdge::Right,
        {tinyContent.right - 1, 50}, {120, 50});
    runner.True(
        tiny.valid && cropIsBounded(tinyVideo, tiny.crop) &&
            tiny.crop.width <= tinyVideo.width &&
            tiny.crop.height <= tinyVideo.height &&
            std::isfinite(tiny.display.scale),
        L"tiny odd sources remain finite and are never promoted past bounds");

    const VideoAreaResizeResult clampedPointer = ResizeVideoArea(
        video, startCrop, startState, viewport, startContent,
        VideoEdge::BottomRight, {600, 450},
        {(std::numeric_limits<int>::max)(), (std::numeric_limits<int>::max)()});
    runner.True(
        clampedPointer.valid && cropIsBounded(video, clampedPointer.crop),
        L"huge drag coordinates clamp to a bounded source crop");

    const double nan = (std::numeric_limits<double>::quiet_NaN)();
    runner.True(
        !ResizeVideoArea(
            video, startCrop, startState, viewport, startContent,
            VideoEdge::None, {600, 300}, {700, 300}).valid,
        L"missing area resize edge is invalid");
    runner.True(
        !ResizeVideoArea(
            video, {1800, 900, 400, 300}, startState, viewport, startContent,
            VideoEdge::Right, {600, 300}, {700, 300}).valid,
        L"out-of-source starting crop is invalid");
    runner.True(
        !ResizeVideoArea(
            video, startCrop, {nan, 0.5, 0.5}, viewport, startContent,
            VideoEdge::Right, {600, 300}, {700, 300}).valid,
        L"non-finite starting display state is invalid");
    runner.True(
        !ResizeVideoArea(
            video, startCrop, startState, viewport, startContent,
            VideoEdge::Right, {-1, 300}, {700, 300}).valid,
        L"drag anchor outside the viewport is invalid");
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

void TestVideoCropPanAndZoom(TestRunner& runner)
{
    using namespace videoplayer;
    const VideoDimensions video{1920, 1080};
    const VideoCrop crop{400, 200, 800, 400};

    runner.Equal(
        PanVideoCrop(video, crop, 11.6, -9.6),
        VideoCrop{412, 190, 800, 400},
        L"pan rounds source deltas and preserves crop extents");
    runner.Equal(
        PanVideoCrop(video, {101, 101, 801, 401}, 0.5, -0.5),
        VideoCrop{102, 100, 801, 401},
        L"pan half-pixel deltas round away from zero");
    runner.Equal(
        PanVideoCrop(video, crop, 0.49, -0.49),
        crop,
        L"sub-half-pixel pan remains stationary");
    runner.Equal(
        PanVideoCrop(video, crop,
            (std::numeric_limits<double>::max)(),
            (std::numeric_limits<double>::max)()),
        VideoCrop{1120, 680, 800, 400},
        L"huge finite positive pan clamps to bottom-right source corner");
    runner.Equal(
        PanVideoCrop(video, crop,
            -(std::numeric_limits<double>::max)(),
            -(std::numeric_limits<double>::max)()),
        VideoCrop{0, 0, 800, 400},
        L"huge finite negative pan clamps to top-left source corner");
    runner.Equal(
        PanVideoCrop({1919, 1079}, {100, 100, 800, 400}, 1.0e30, 1.0e30),
        VideoCrop{1119, 679, 800, 400},
        L"pan reaches odd decoded-source edges without changing extents");

    const VideoCrop centeredIn = ZoomVideoCrop(video, crop, 0.5, 0.5, 0.5);
    runner.Equal(
        centeredIn,
        VideoCrop{600, 300, 400, 200},
        L"center-anchor zoom in halves crop around the same source point");
    runner.Near(
        static_cast<double>(centeredIn.x) + centeredIn.width * 0.5,
        static_cast<double>(crop.x) + crop.width * 0.5,
        0.000001,
        L"center anchor source X remains stationary");
    runner.Near(
        static_cast<double>(centeredIn.y) + centeredIn.height * 0.5,
        static_cast<double>(crop.y) + crop.height * 0.5,
        0.000001,
        L"center anchor source Y remains stationary");
    runner.Equal(
        ZoomVideoCrop(video, crop, 0.5, 0.0, 0.0),
        VideoCrop{400, 200, 400, 200},
        L"top-left anchor keeps the crop corner stationary");
    runner.Equal(
        ZoomVideoCrop(video, crop, 0.5, 1.0, 1.0),
        VideoCrop{800, 400, 400, 200},
        L"bottom-right anchor keeps the crop corner stationary");

    const VideoCrop centeredOut = ZoomVideoCrop(video, crop, 1.5, 0.5, 0.5);
    runner.Equal(
        centeredOut,
        VideoCrop{200, 100, 1200, 600},
        L"unclamped zoom out preserves ratio around anchor");
    runner.True(
        static_cast<std::uint64_t>(centeredOut.width) * crop.height ==
            static_cast<std::uint64_t>(centeredOut.height) * crop.width,
        L"zoom keeps crop aspect ratio before a source limit is reached");
    runner.Equal(
        ZoomVideoCrop(video, {0, 0, 800, 400}, 2.0, 0.5, 0.5),
        VideoCrop{0, 0, 1600, 800},
        L"zoom anchor yields to top-left source bounds");

    VideoCrop independentlyExpanded{400, 400, 800, 100};
    for (int step = 0; step < 8; ++step) {
        independentlyExpanded = ZoomVideoCrop(
            video, independentlyExpanded, 2.0, 0.5, 0.5);
    }
    runner.Equal(
        independentlyExpanded,
        VideoCrop{0, 330, 1920, 240},
        L"repeated zoom out stops at source edge without changing narrow selection aspect");
    runner.True(
        independentlyExpanded.IsValid(),
        L"maximum same-aspect zoom result remains a valid applied crop");
    runner.Equal(
        ZoomVideoCrop(video, independentlyExpanded, 2.0, 0.5, 0.5),
        independentlyExpanded,
        L"extra zoom out keeps maximum same-aspect crop and position");

    const VideoDimensions oddSelectionLimit{321, 179};
    VideoCrop cappedRoundTrip{500, 300, 321, 179};
    for (int step = 0; step < 8; ++step) {
        cappedRoundTrip = ZoomVideoCrop(
            video, cappedRoundTrip, 0.8, 0.5, 0.5, oddSelectionLimit);
    }
    runner.True(
        cappedRoundTrip.width < oddSelectionLimit.width &&
            cappedRoundTrip.height < oddSelectionLimit.height,
        L"reference-aspect zoom can move inward from the selection");
    bool retainedAspect = true;
    for (int step = 0; step < 64; ++step) {
        cappedRoundTrip = ZoomVideoCrop(
            video, cappedRoundTrip, 1.12, 0.5, 0.5, oddSelectionLimit);
        retainedAspect = retainedAspect &&
            std::abs(static_cast<double>(cappedRoundTrip.height) -
                cappedRoundTrip.width * 179.0 / 321.0) <= 2.0;
    }
    runner.True(
        cappedRoundTrip.width == video.width &&
            cappedRoundTrip.height > oddSelectionLimit.height &&
            cappedRoundTrip.x + cappedRoundTrip.width <= video.width &&
            cappedRoundTrip.y + cappedRoundTrip.height <= video.height,
        L"repeated zoom-out expands beyond original odd selection within source");
    runner.True(retainedAspect,
        L"original odd selection aspect does not accumulate rounding drift");

    runner.Equal(
        ZoomVideoCrop(
            video, {1200, 700, 160, 90}, 2.0, 0.0, 0.0, {160, 90}),
        VideoCrop{1200, 700, 320, 180},
        L"first outward step expands past the original selection at its panned location");
    runner.Equal(
        ZoomVideoCrop(
            video, {1760, 990, 160, 90}, 8.0, 1.0, 1.0,
            {320, 180}),
        VideoCrop{640, 360, 1280, 720},
        L"zoom beyond original extents preserves bottom-right source anchor");
    runner.Equal(
        ZoomVideoCrop(video, {600, 400, 7, 9}, 1.01, 0.0, 0.0, {7, 9}),
        VideoCrop{600, 400, 16, 22},
        L"tiny selection can expand beyond its initial extents with bounded rounding");
    const VideoCrop widerThanLimit{400, 200, 800, 400};
    runner.Equal(
        ZoomVideoCrop(video, widerThanLimit, 0.5, 0.5, 0.5, {200, 100}),
        VideoCrop{600, 300, 400, 200},
        L"crop larger than original selection can zoom back in");
    runner.Equal(
        ZoomVideoCrop(
            {1919, 1079}, {100, 100, 800, 400},
            (std::numeric_limits<double>::max)(), 0.5, 0.5,
            {800, 400}),
        VideoCrop{0, 0, 1919, 960},
        L"reference aspect is retained at an odd decoded-source boundary");
    runner.Equal(
        ZoomVideoCrop({640, 360}, {220, 80, 200, 200}, 100.0, 0.5, 0.5, {200, 200}),
        VideoCrop{140, 0, 360, 360},
        L"square selection reaches source height while retaining horizontal pan room");
    runner.Equal(
        ZoomVideoCrop(video, {100, 100, 320, 180}, 100.0, 0.5, 0.5, {320, 180}),
        VideoCrop{0, 0, 1920, 1080},
        L"matching source aspect can reach the full frame as an active crop");
    runner.Equal(
        ZoomVideoCrop(video, {100, 100, 2, 500}, 100.0, 0.0, 0.0, {2, 500}),
        VideoCrop{100, 0, 4, 1080},
        L"source edge overrides minimum without stretching a very narrow selection");
    runner.Equal(
        ZoomVideoCrop(video, {100, 100, 500, 2}, 100.0, 0.0, 0.0, {500, 2}),
        VideoCrop{0, 100, 1920, 8},
        L"source edge overrides minimum without stretching a very short selection");

    runner.Equal(
        ZoomVideoCrop(video, {100, 100, 64, 32}, 0.001, 0.5, 0.5),
        VideoCrop{116, 108, 32, 16},
        L"zoom in uses one minimum-clamped scale and preserves aspect ratio");
    const VideoCrop oneTinySide{20, 30, 7, 90};
    runner.Equal(
        ZoomVideoCrop(video, oneTinySide, 0.5, 0.5, 0.5),
        oneTinySide,
        L"one already tiny side prevents zoom-in from distorting or enlarging crop");
    const VideoCrop tiny{11, 13, 7, 9};
    runner.Equal(
        ZoomVideoCrop(video, tiny, 0.5, 0.5, 0.5),
        tiny,
        L"zoom in never enlarges an already tiny crop to the minimum");
    const VideoCrop tinyZoomOut = ZoomVideoCrop(video, tiny, 1.1, 0.5, 0.5);
    runner.True(
        tinyZoomOut.width >= 16U && tinyZoomOut.height >= 16U &&
            tinyZoomOut.x + tinyZoomOut.width <= video.width &&
            tinyZoomOut.y + tinyZoomOut.height <= video.height,
        L"zoom out grows a tiny crop to bounded minimum dimensions");

    const VideoCrop oddAligned = ZoomVideoCrop(
        video, {101, 99, 801, 401}, 0.75, 0.5, 0.5);
    runner.Equal(
        oddAligned,
        VideoCrop{202, 150, 600, 300},
        L"zoom aligns ordinary odd crop coordinates and extents to even pixels");
    const VideoCrop oddFull = ZoomVideoCrop(
        {1919, 1079}, {100, 100, 800, 400},
        (std::numeric_limits<double>::max)(), 0.5, 0.5);
    runner.Equal(
        oddFull,
        VideoCrop{0, 0, 1919, 960},
        L"huge finite zoom reaches the same-aspect limit inside odd source dimensions");
    runner.True(oddFull.IsValid(), L"odd source-boundary crop remains valid");

    const double nan = (std::numeric_limits<double>::quiet_NaN)();
    const double infinity = (std::numeric_limits<double>::infinity)();
    runner.Equal(
        PanVideoCrop(video, crop, nan, 1.0), crop,
        L"NaN pan leaves crop unchanged");
    runner.Equal(
        PanVideoCrop(video, crop, infinity, 1.0), crop,
        L"infinite pan leaves crop unchanged");
    runner.Equal(
        PanVideoCrop({0, 1080}, crop, 10.0, 10.0), crop,
        L"pan with invalid video dimensions leaves crop unchanged");
    const VideoCrop outside{1900, 100, 100, 100};
    runner.Equal(
        PanVideoCrop(video, outside, -10.0, 10.0), outside,
        L"pan with out-of-source crop leaves it unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, 0.0, 0.5, 0.5), crop,
        L"zero zoom scale leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, -1.0, 0.5, 0.5), crop,
        L"negative zoom scale leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, infinity, 0.5, 0.5), crop,
        L"infinite zoom scale leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, nan, 0.5, 0.5), crop,
        L"NaN zoom scale leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, 0.5, -0.01, 0.5), crop,
        L"negative normalized anchor leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, 0.5, 0.5, 1.01), crop,
        L"normalized anchor above one leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, crop, 0.5, nan, 0.5), crop,
        L"NaN normalized anchor leaves crop unchanged");
    runner.Equal(
        ZoomVideoCrop(video, {100, 100, 0, 100}, 0.5, 0.5, 0.5),
        VideoCrop{100, 100, 0, 100},
        L"zoom with invalid crop leaves it unchanged");
    runner.Equal(
        ZoomVideoCrop(video, outside, 0.5, 0.5, 0.5), outside,
        L"zoom with out-of-source crop leaves it unchanged");
    runner.Equal(
        ZoomVideoCrop(video, {101, 99, 801, 401}, 1.0, 0.5, 0.5),
        VideoCrop{101, 99, 801, 401},
        L"unit zoom preserves odd crop exactly");
}

void TestPreviewMath(TestRunner& runner)
{
    using namespace videoplayer;

    runner.Equal(QuantizePreviewTimestamp(-10, 10'000), std::int64_t{0}, L"preview time clamps low");
    runner.Equal(QuantizePreviewTimestamp(20'000, 10'000), std::int64_t{10'000}, L"preview time clamps high");
    runner.Equal(QuantizePreviewTimestamp(374, 10'000, 750), std::int64_t{0}, L"preview custom quantum below midpoint");
    runner.Equal(QuantizePreviewTimestamp(375, 10'000, 750), std::int64_t{750}, L"preview custom quantum midpoint rounds up");
    runner.Equal(QuantizePreviewTimestamp(1'124, 10'000, 750), std::int64_t{750}, L"preview custom quantum upper lower-half edge");
    runner.Equal(QuantizePreviewTimestamp(1'125, 10'000, 750), std::int64_t{1'500}, L"preview custom quantum upper midpoint");
    runner.Equal(QuantizePreviewTimestamp(1'000, 1'000), std::int64_t{1'000}, L"duration endpoint is preserved");
    runner.Equal(QuantizePreviewTimestamp(500, 10'000, 1'000), std::int64_t{1'000}, L"custom quantum midpoint");
    runner.Equal(QuantizePreviewTimestamp(500, 0), std::int64_t{0}, L"invalid duration rejects quantization");
    runner.Equal(QuantizePreviewTimestamp(500, 1'000, 0), std::int64_t{0}, L"invalid quantum rejects quantization");
    runner.Equal(PreviewTimestampQuantum(60'000), std::int64_t{5'000}, L"short video uses five-second previews");
    runner.Equal(PreviewTimestampQuantum(600'000), std::int64_t{5'000}, L"ten-minute video keeps five-second previews");
    runner.Equal(PreviewTimestampQuantum(600'001), std::int64_t{6'000}, L"adaptive quantum rounds up to whole seconds");
    runner.Equal(PreviewTimestampQuantum(2'400'000), std::int64_t{20'000}, L"forty-minute video uses twenty-second previews");
    runner.Equal(PreviewTimestampQuantum(7'200'000), std::int64_t{60'000}, L"two-hour video stays within 120 preview intervals");
    runner.Equal(PreviewTimestampQuantum((std::numeric_limits<std::int64_t>::max)()),
        std::int64_t{76'861'433'640'457'000}, L"huge duration quantum rounds safely without a short-video cap");
    runner.Equal(QuantizePreviewTimestamp(2'499, 60'000), std::int64_t{0}, L"coarse preview below midpoint");
    runner.Equal(QuantizePreviewTimestamp(2'500, 60'000), std::int64_t{5'000}, L"coarse preview midpoint");
    runner.Equal(QuantizePreviewTimestamp(19'900, 2'400'000), std::int64_t{20'000}, L"long-video bucket used by default");
    runner.Equal(QuantizePreviewTimestamp(61'234, 61'234), std::int64_t{61'234}, L"non-grid media end stays exact");
    runner.Equal(QuantizePreviewTimestamp(
        (std::numeric_limits<std::int64_t>::max)() - 1,
        (std::numeric_limits<std::int64_t>::max)()),
        (std::numeric_limits<std::int64_t>::max)(),
        L"rounding the last huge-duration bucket clamps to the endpoint without overflow");
    runner.True(BuildPreviewWarmupTimestamps(0).empty(), L"zero-duration preview warmup is empty");
    runner.True(BuildPreviewWarmupTimestamps(-1).empty(), L"negative-duration preview warmup is empty");
    runner.Equal(BuildPreviewWarmupTimestamps(1),
        std::vector<std::int64_t>{0, 1}, L"sub-quantum video warms both distinct endpoints");
    runner.Equal(BuildPreviewWarmupTimestamps(40'000),
        std::vector<std::int64_t>{0, 40'000, 20'000, 10'000, 30'000, 5'000, 15'000, 25'000, 35'000},
        L"preview warmup visits endpoints midpoint quarters then smaller gaps");
    runner.Equal(BuildPreviewWarmupTimestamps(26'000),
        std::vector<std::int64_t>{0, 26'000, 15'000, 5'000, 20'000, 10'000, 25'000},
        L"non-grid endpoint and uneven bucket gaps preserve breadth-first warmup order");

    const std::int64_t warmupDurations[]{
        1, 4'999, 5'000, 5'001, 9'999, 10'000, 61'234,
        599'999, 600'000, 600'001, 2'400'000, 7'200'000,
        86'400'000, 604'800'001,
        (std::numeric_limits<std::int64_t>::max)() - 1,
        (std::numeric_limits<std::int64_t>::max)()};
    for (const auto duration : warmupDurations) {
        const auto quantum = PreviewTimestampQuantum(duration);
        auto timestamps = BuildPreviewWarmupTimestamps(duration);
        const std::wstring label = L"preview warmup duration " + std::to_wstring(duration);
        runner.True(quantum >= kPreviewTimestampQuantumMs && quantum % 1000 == 0,
            label + L" has a whole-second quantum of at least five seconds");
        runner.True(timestamps.size() <= static_cast<std::size_t>(kPreviewMaximumIntervals + 1),
            label + L" fits entirely in the bounded preview cache");
        runner.True(timestamps.size() >= 2 && timestamps[0] == 0 && timestamps[1] == duration,
            label + L" warms both endpoints first");
        std::sort(timestamps.begin(), timestamps.end());
        runner.True(std::adjacent_find(timestamps.begin(), timestamps.end()) == timestamps.end(),
            label + L" never requests the same bucket twice");
        const std::int64_t expectedIntervals = duration / quantum + (duration % quantum != 0 ? 1 : 0);
        runner.Equal(timestamps.size(), static_cast<std::size_t>(expectedIntervals + 1),
            label + L" contains every bucket and the exact endpoint");
        bool complete = !timestamps.empty() && timestamps.back() == duration;
        for (std::size_t index = 0; index + 1 < timestamps.size(); ++index) {
            const auto timestamp = timestamps[index];
            complete = complete && timestamp == static_cast<std::int64_t>(index) * quantum &&
                timestamp >= 0 && timestamp < duration &&
                QuantizePreviewTimestamp(timestamp, duration) == timestamp;
        }
        runner.True(complete, label + L" covers all regular quantization keys without overflow");
        bool coversRequests = true;
        for (std::int64_t sample = 0; sample <= 1000; ++sample) {
            const auto request = (duration / 1000) * sample + ((duration % 1000) * sample) / 1000;
            const auto key = QuantizePreviewTimestamp(request, duration);
            coversRequests = coversRequests && std::binary_search(timestamps.begin(), timestamps.end(), key);
        }
        runner.True(coversRequests, label + L" includes every sampled hover request");
    }
    int coarseBuckets = 0;
    int fineBuckets = 0;
    std::int64_t lastCoarse = -1;
    std::int64_t lastFine = -1;
    for (std::int64_t time = 0; time <= 2'400'000; time += 100) {
        const auto coarse = QuantizePreviewTimestamp(time, 2'400'000);
        const auto fine = QuantizePreviewTimestamp(time, 2'400'000, 750);
        if (coarse != lastCoarse) { ++coarseBuckets; lastCoarse = coarse; }
        if (fine != lastFine) { ++fineBuckets; lastFine = fine; }
    }
    runner.Equal(coarseBuckets, 121, L"forty-minute sweep has only 121 distinct preview frames");
    runner.True(fineBuckets > coarseBuckets * 20, L"coarse preview sweep eliminates over twenty times the old requests");

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

void TestPreviewCacheNearest(TestRunner& runner)
{
    using namespace videoplayer;
    constexpr std::int64_t minimum = (std::numeric_limits<std::int64_t>::min)();
    constexpr std::int64_t maximum = (std::numeric_limits<std::int64_t>::max)();
    PreviewCache cache(3);
    cache.ClearForMedia(4);
    runner.True(!cache.FindNearest(4, 2'000, maximum), L"nearest preview misses empty cache");
    const PreviewFramePtr earlier = MakePreviewFrame(4, 1'000, 1);
    const PreviewFramePtr middle = MakePreviewFrame(4, 3'000, 2);
    const PreviewFramePtr later = MakePreviewFrame(4, 5'000, 3);
    cache.Put(earlier);
    cache.Put(middle);
    cache.Put(later);
    runner.True(!cache.FindNearest(5, 1'000, maximum), L"nearest preview rejects another generation");
    runner.True(!cache.FindNearest(4, 2'000, 999), L"nearest preview respects distance limit");
    runner.True(cache.FindNearest(4, 2'000, 1'000) == earlier,
        L"nearest preview includes distance boundary and prefers earlier tie");
    runner.True(cache.FindNearest(4, 3'000, 0) == middle,
        L"nearest preview allows exact hit at zero distance");
    runner.True(!cache.FindNearest(4, 3'001, 0), L"nearest preview zero distance rejects nearby frame");
    runner.True(!cache.FindNearest(4, 3'000, -1), L"nearest preview rejects negative distance bound");
    runner.True(!cache.FindNearest(4, 3'000, minimum), L"nearest preview rejects minimum int64 bound");
    const PreviewFramePtr approximate = cache.FindNearest(4, 2'999, 2);
    runner.True(approximate == middle, L"nearest preview selects closest frame rather than LRU order");
    runner.True(approximate && approximate->timestampMs == 3'000,
        L"nearest preview preserves real frame timestamp");
    runner.True(cache.FindNearest(4, 2'000, 1'000) == earlier,
        L"nearest preview earlier tie wins after later frame was promoted");

    // Reset the access order so promotion and misses have independently
    // observable eviction behavior.
    cache.ClearForMedia(4);
    cache.Put(earlier);
    cache.Put(middle);
    cache.Put(later);
    runner.True(cache.FindNearest(4, 1'200, 200) == earlier,
        L"nearest preview accesses oldest frame");
    cache.Put(MakePreviewFrame(4, 7'000, 4));
    runner.True(!cache.Find(4, 3'000), L"nearest preview hit promotes frame before eviction");
    runner.True(cache.Find(4, 1'000) == earlier, L"nearest preview promoted frame survives eviction");
    PreviewCache misses(2);
    misses.ClearForMedia(4);
    misses.Put(earlier);
    misses.Put(middle);
    runner.True(!misses.FindNearest(4, 1'100, 50), L"nearest preview bounded miss finds no fallback");
    misses.Put(later);
    runner.True(!misses.Find(4, 1'000), L"nearest preview miss does not promote out-of-bound candidate");
    cache.ClearForMedia(5);
    runner.True(!cache.FindNearest(4, 1'000, maximum) &&
        !cache.FindNearest(5, 1'000, maximum), L"nearest preview cannot leak frames after media reset");
    PreviewCache disabled(0);
    disabled.ClearForMedia(4);
    disabled.Put(earlier);
    runner.True(!disabled.FindNearest(4, 1'000, maximum), L"nearest preview respects zero capacity");
    cache.Put(MakePreviewFrame(5, 1'000, 5, 0, 2));
    runner.True(!cache.FindNearest(5, 1'000, maximum), L"nearest preview never returns invalid pixels");

    // IsValid validates pixel storage, not timestamp sign. Exercise the full
    // timestamp domain to catch overflow, including distances above INT64_MAX.
    PreviewCache extremes(2);
    extremes.ClearForMedia(4);
    const PreviewFramePtr first = MakePreviewFrame(4, minimum, 1);
    const PreviewFramePtr last = MakePreviewFrame(4, maximum, 2);
    runner.True(first->IsValid() && last->IsValid(), L"extreme timestamp frames are valid cache inputs");
    extremes.Put(first);
    extremes.Put(last);
    runner.True(extremes.FindNearest(4, minimum, maximum) == first,
        L"nearest preview handles minimum timestamp exact hit");
    runner.True(extremes.FindNearest(4, maximum, maximum) == last,
        L"nearest preview handles maximum timestamp exact hit");
    runner.True(extremes.FindNearest(4, 0, maximum) == last,
        L"nearest preview handles positive inclusive int64 distance limit");
    runner.True(extremes.FindNearest(4, -1, maximum) == first,
        L"nearest preview handles negative inclusive int64 distance limit");
    runner.True(!extremes.FindNearest(4, 0, maximum - 1),
        L"nearest preview rejects extreme frames beyond large bound");
    extremes.ClearForMedia(4);
    extremes.Put(first);
    runner.True(!extremes.FindNearest(4, maximum, maximum),
        L"nearest preview does not wrap minimum-to-maximum distance");
    runner.True(extremes.FindNearest(4, minimum + 1, 1) == first,
        L"nearest preview safely subtracts nearby minimum timestamps");
    extremes.ClearForMedia(4);
    extremes.Put(last);
    runner.True(!extremes.FindNearest(4, minimum, maximum),
        L"nearest preview does not wrap maximum-to-minimum distance");
    runner.True(extremes.FindNearest(4, maximum - 1, 1) == last,
        L"nearest preview safely subtracts nearby maximum timestamps");
    extremes.ClearForMedia(4);
    const PreviewFramePtr negative = MakePreviewFrame(4, -1, 1);
    extremes.Put(negative);
    extremes.Put(MakePreviewFrame(4, 1, 2));
    runner.True(extremes.FindNearest(4, 0, 1) == negative,
        L"nearest preview equal distance across zero still selects earlier frame");
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
    runner.Equal(ToolbarRestoreActivationHeight(96), 48, L"restore activation zone at 96 DPI");
    runner.Equal(ToolbarRestoreActivationHeight(144), 72, L"restore activation zone at 144 DPI");
    runner.Equal(ToolbarRestoreActivationHeight(0), 48, L"restore activation zone invalid DPI fallback");

    ToolbarVisibilityInput input = FullscreenPlayingToolbarInput();
    input.fullscreen = false;
    input.currentlyVisible = false;
    input.restoreCurrentlyVisible = true;
    ToolbarVisibilityDecision decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldBeVisible, L"windowed toolbar is always visible");
    runner.Equal(decision.action, ToolbarVisibilityAction::Show, L"windowed hidden toolbar receives show action");
    runner.True(!decision.shouldRestoreBeVisible, L"ordinary windowed mode has no restore affordance");
    runner.Equal(decision.restoreAction, ToolbarVisibilityAction::Hide,
        L"leaving manual mode hides a stale restore affordance");

    input = FullscreenPlayingToolbarInput();
    input.wasFullscreen = false;
    input.fullscreen = false;
    input.manuallyHidden = true;
    input.playing = false;
    input.currentlyVisible = true;
    input.cursorInsideClient = true;
    input.cursorMoved = false;
    input.cursorY = 952;
    input.pointerOverToolbar = true;
    input.seekDragging = true;
    input.volumeDragging = true;
    input.previewVisible = true;
    input.controlHasFocus = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldBeVisible,
        L"manual windowed mode keeps the full toolbar hidden despite every blocker");
    runner.Equal(decision.action, ToolbarVisibilityAction::Hide,
        L"manual windowed mode hides an actually visible toolbar");
    runner.True(decision.interactionBlocksHide,
        L"manual mode reports ordinary blockers without letting them win");
    runner.True(decision.inRestoreActivationZone,
        L"restore activation top edge is inclusive");
    runner.True(decision.shouldRestoreBeVisible,
        L"stationary cursor in restore zone shows the restore affordance");
    runner.Equal(decision.restoreAction, ToolbarVisibilityAction::Show,
        L"restore-zone hover emits restore show action");

    input.currentlyVisible = false;
    input.restoreCurrentlyVisible = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldRestoreBeVisible,
        L"stationary cursor in restore zone keeps the restore affordance visible");
    runner.Equal(decision.restoreAction, ToolbarVisibilityAction::None,
        L"visible restore affordance needs no repeated show action");

    input.pointerOverToolbar = false;
    input.seekDragging = false;
    input.volumeDragging = false;
    input.previewVisible = false;
    input.controlHasFocus = false;
    input.cursorY = 951;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.inRestoreActivationZone,
        L"pixel above restore activation zone is outside");
    runner.True(!decision.shouldRestoreBeVisible,
        L"leaving restore zone hides the affordance");
    runner.Equal(decision.restoreAction, ToolbarVisibilityAction::Hide,
        L"leaving restore zone emits hide action");

    input.cursorY = 1'000;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.inRestoreActivationZone,
        L"client bottom edge is outside restore half-open zone");

    input = FullscreenPlayingToolbarInput();
    input.manuallyHidden = true;
    input.currentlyVisible = false;
    input.cursorInsideClient = true;
    input.cursorY = 928;
    input.dpi = 144;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.inRestoreActivationZone,
        L"DPI-scaled restore activation top edge is inclusive");
    runner.True(decision.shouldRestoreBeVisible,
        L"DPI-scaled restore zone shows the affordance");
    input.cursorY = 927;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.inRestoreActivationZone,
        L"pixel above DPI-scaled restore zone is outside");

    input = FullscreenPlayingToolbarInput();
    input.manuallyHidden = true;
    input.currentlyVisible = false;
    input.cursorInsideClient = false;
    input.pointerOverRestore = true;
    input.restoreCurrentlyVisible = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(decision.shouldRestoreBeVisible,
        L"pointer over restore popup keeps it visible outside the video zone");
    runner.Equal(decision.restoreAction, ToolbarVisibilityAction::None,
        L"pointer over restore popup needs no repeated show action");

    input.ownedPopupsAllowed = false;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldRestoreBeVisible,
        L"lifecycle popup gate suppresses restore affordance");
    runner.Equal(decision.restoreAction, ToolbarVisibilityAction::Hide,
        L"lifecycle popup gate hides a visible restore affordance");

    input.ownedPopupsAllowed = true;
    input.zoomSelecting = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldRestoreBeVisible,
        L"zoom selection suppresses restore affordance");
    runner.True(!decision.shouldBeVisible,
        L"zoom selection cannot override explicit manual toolbar hiding");

    input = FullscreenPlayingToolbarInput();
    input.wasFullscreen = false;
    input.manuallyHidden = true;
    input.currentlyVisible = true;
    decision = EvaluateToolbarVisibility(input);
    runner.True(!decision.shouldBeVisible,
        L"fullscreen transition preserves explicit manual toolbar hiding");
    runner.Equal(decision.action, ToolbarVisibilityAction::Hide,
        L"manual hiding wins on first fullscreen decision");

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
    runner.Equal(kPrivateSaveDialogFlags,
        static_cast<DWORD>(OFN_EXPLORER | OFN_PATHMUSTEXIST |
            OFN_NOCHANGEDIR | OFN_DONTADDTORECENT),
        L"export save dialog avoids Recent Items and CWD persistence");
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
    TestApplicationIconResources(runner);
    TestTimeFormatting(runner);
    TestPlaybackMath(runner);
    TestPlaybackLoop(runner);
    TestVideoExport(runner);
    TestRealVideoExportIntegration(runner);
    TestRemovalPlans(runner);
    TestRemovalWithoutReencode(runner);
    TestProgressStrip(runner);
    TestIntervalTimeline(runner);
    TestUtf8(runner);
    TestMediaOpen(runner);
    TestMediaOpenLaunchIntegration(runner);
    TestLongPaths(runner);
    TestFileExistence(runner);
    TestVideoGeometryBasics(runner);
    TestVideoContentFit(runner);
    TestVideoDisplayGeometry(runner);
    TestVideoAreaResize(runner);
    TestVideoCropMapping(runner);
    TestVideoCropPanAndZoom(runner);
    TestPreviewMath(runner);
    TestPreviewCache(runner);
    TestPreviewCacheNearest(runner);
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
