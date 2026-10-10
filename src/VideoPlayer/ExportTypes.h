#pragma once

#include "VideoGeometry.h"

#include <cstdint>
#include <string>
#include <vector>

namespace videoplayer {

enum class ExportFragment : std::uint8_t { WholeVideo, Segment };
enum class ExportImage : std::uint8_t { FullFrame, VisibleZoom };
enum class ExportResolution : std::uint8_t { SourceCanvas, NativeCrop };
enum class ExportMethod : std::uint8_t { Exact, FastStreamCopy };
enum class ExportAudio : std::uint8_t { Original, None };

struct Rational final {
    std::int64_t numerator = 0;
    std::int64_t denominator = 1;
    bool IsValid() const noexcept { return denominator != 0; }
};

struct ExportSourceRegion final {
    bool valid = false;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    VideoCrop aligned{};
};

struct ExportFileIdentity final {
    bool exists = false;
    bool valid = false;
    std::uint32_t volume = 0;
    std::uint64_t fileIndex = 0;
    std::uint64_t size = 0;
    std::uint64_t modifiedTime = 0;
};

struct VideoInterval final {
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
};

struct ExportSnapshot final {
    std::wstring sourcePath;
    ExportFileIdentity sourceIdentity{};
    std::uint32_t generation = 0;
    std::int64_t durationMs = 0;
    bool hasSegment = false;
    std::int64_t segmentStartMs = 0;
    std::int64_t segmentEndMs = 0;
    std::vector<VideoInterval> removalIntervals;
    VideoDimensions playerVideo{};
    VideoCrop playerCrop{};
    GeometryRect presentationScreen{};
    GeometryRect renderScreen{};
    GeometryRect viewportScreen{};
    bool zoomApplied = false;
};

struct MediaStreamInfo final {
    int index = -1;
    std::wstring codecType;
    std::wstring codecName;
    std::wstring profile;
    std::wstring fieldOrder;
    std::wstring pixelFormat;
    std::wstring colorSpace;
    std::wstring colorTransfer;
    std::wstring colorPrimaries;
    unsigned width = 0;
    unsigned height = 0;
    unsigned bitDepth = 0;
    int level = 0;
    std::int64_t bitRate = 0;
    Rational sampleAspectRatio{1, 1};
    Rational displayAspectRatio{0, 1};
    Rational averageFrameRate{0, 1};
    Rational realFrameRate{0, 1};
    Rational timeBase{0, 1};
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    int rotationDegrees = 0;
    bool attachedPicture = false;
    bool defaultDisposition = false;
};

struct MediaProbeResult final {
    bool valid = false;
    std::wstring formatName;
    double startSeconds = 0.0;
    double durationSeconds = 0.0;
    std::int64_t bitRate = 0;
    std::vector<MediaStreamInfo> streams;
    int chapterCount = 0;
    std::wstring ffprobeVersion;
    std::wstring ffmpegVersion;
    std::vector<std::wstring> availableEncoders;
    std::wstring error;

    const MediaStreamInfo* MainVideo() const noexcept {
        for (const auto& stream : streams) {
            if (stream.codecType == L"video" && !stream.attachedPicture &&
                stream.defaultDisposition) return &stream;
        }
        for (const auto& stream : streams) {
            if (stream.codecType == L"video" && !stream.attachedPicture) return &stream;
        }
        return nullptr;
    }
    bool HasAudio() const noexcept {
        for (const auto& stream : streams) {
            if (stream.codecType == L"audio") return true;
        }
        return false;
    }
    bool HasEncoder(const std::wstring& name) const noexcept {
        for (const auto& encoder : availableEncoders) {
            if (encoder == name) return true;
        }
        return false;
    }
};

struct ExportOptions final {
    ExportFragment fragment = ExportFragment::WholeVideo;
    ExportImage image = ExportImage::FullFrame;
    ExportResolution resolution = ExportResolution::NativeCrop;
    ExportMethod method = ExportMethod::Exact;
    ExportAudio audio = ExportAudio::Original;
    std::wstring destinationPath;
    bool confirmedCodecFallback = false;
};

struct ExportPlan final {
    bool valid = false;
    bool streamCopy = false;
    bool requiresCodecFallbackConfirmation = false;
    std::wstring muxer;
    std::wstring outputExtension;
    std::wstring videoEncoder;
    std::wstring summary;
    std::wstring warning;
    std::wstring error;
    VideoCrop effectiveCrop{};
    unsigned outputWidth = 0;
    unsigned outputHeight = 0;
    std::int64_t targetVideoBitRate = 0;
    bool outputHasAudio = false;
    std::wstring expectedVideoCodec;
    std::wstring expectedPixelFormat;
    unsigned expectedBitDepth = 0;
    Rational expectedSampleAspectRatio{1, 1};
    int expectedRotationDegrees = 0;
    std::vector<std::wstring> expectedAudioCodecs;
    std::int64_t startMs = 0;
    std::int64_t endMs = 0;
    std::vector<std::wstring> ffmpegArguments;
};

}  // namespace videoplayer
