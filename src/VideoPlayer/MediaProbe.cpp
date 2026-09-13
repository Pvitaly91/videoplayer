#include "MediaProbe.h"

#include "ExportProcess.h"
#include "PathUtils.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

namespace videoplayer {
namespace {

std::wstring Utf8(const std::string& input) {
    if (input.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), result.data(), required) != required) return {};
    return result;
}

std::string Unquote(std::string value) {
    if (value.size() < 2 || value.front() != '"' || value.back() != '"') return value;
    value = value.substr(1, value.size() - 2);
    std::string result;
    result.reserve(value.size());
    bool escaped = false;
    for (const char current : value) {
        if (escaped) {
            switch (current) {
            case 'n': result.push_back('\n'); break;
            case 'r': result.push_back('\r'); break;
            case 't': result.push_back('\t'); break;
            default: result.push_back(current); break;
            }
            escaped = false;
        } else if (current == '\\') {
            escaped = true;
        } else {
            result.push_back(current);
        }
    }
    if (escaped) result.push_back('\\');
    return result;
}

std::int64_t Integer(const std::string& value) {
    char* end = nullptr;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    return end == value.c_str() ? 0 : static_cast<std::int64_t>(parsed);
}

double Floating(const std::string& value) {
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() || !std::isfinite(parsed) ? 0.0 : parsed;
}

Rational Ratio(std::string value) {
    std::replace(value.begin(), value.end(), ':', '/');
    const auto slash = value.find('/');
    if (slash == std::string::npos) return {Integer(value), 1};
    const std::int64_t denominator = Integer(value.substr(slash + 1));
    return {Integer(value.substr(0, slash)), denominator == 0 ? 1 : denominator};
}

unsigned BitDepthFromPixelFormat(const std::wstring& pixelFormat) {
    for (const unsigned depth : {16U, 14U, 12U, 10U, 9U}) {
        if (pixelFormat.find(std::to_wstring(depth)) != std::wstring::npos) return depth;
    }
    return pixelFormat.empty() ? 0U : 8U;
}

void AssignStream(MediaStreamInfo& stream,
    const std::string& field, const std::string& value) {
    if (field == "index") stream.index = static_cast<int>(Integer(value));
    else if (field == "codec_type") stream.codecType = Utf8(value);
    else if (field == "codec_name") stream.codecName = Utf8(value);
    else if (field == "profile") stream.profile = Utf8(value);
    else if (field == "field_order") stream.fieldOrder = Utf8(value);
    else if (field == "pix_fmt") stream.pixelFormat = Utf8(value);
    else if (field == "color_space") stream.colorSpace = Utf8(value);
    else if (field == "color_transfer") stream.colorTransfer = Utf8(value);
    else if (field == "color_primaries") stream.colorPrimaries = Utf8(value);
    else if (field == "width") stream.width = static_cast<unsigned>((std::max<std::int64_t>)(0, Integer(value)));
    else if (field == "height") stream.height = static_cast<unsigned>((std::max<std::int64_t>)(0, Integer(value)));
    else if (field == "bits_per_raw_sample") stream.bitDepth = static_cast<unsigned>((std::max<std::int64_t>)(0, Integer(value)));
    else if (field == "level") stream.level = static_cast<int>(Integer(value));
    else if (field == "bit_rate") stream.bitRate = Integer(value);
    else if (field == "sample_aspect_ratio") stream.sampleAspectRatio = Ratio(value);
    else if (field == "display_aspect_ratio") stream.displayAspectRatio = Ratio(value);
    else if (field == "avg_frame_rate") stream.averageFrameRate = Ratio(value);
    else if (field == "r_frame_rate") stream.realFrameRate = Ratio(value);
    else if (field == "time_base") stream.timeBase = Ratio(value);
    else if (field == "start_time") stream.startSeconds = Floating(value);
    else if (field == "duration") stream.durationSeconds = Floating(value);
    else if (field == "disposition.attached_pic") stream.attachedPicture = Integer(value) != 0;
    else if (field == "disposition.default") stream.defaultDisposition = Integer(value) != 0;
    else if (field.size() >= 8 && field.compare(field.size() - 8, 8, "rotation") == 0) {
        stream.rotationDegrees = static_cast<int>(Integer(value));
    }
}

}  // namespace

MediaProbeResult ParseFfprobeFlat(const std::string& flatOutput) {
    MediaProbeResult result{};
    std::map<int, MediaStreamInfo> streams;
    std::istringstream lines(flatOutput);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        const std::string key = line.substr(0, equal);
        const std::string value = Unquote(line.substr(equal + 1));
        static constexpr char streamPrefix[] = "streams.stream.";
        if (key.compare(0, sizeof(streamPrefix) - 1, streamPrefix) == 0) {
            const auto indexStart = sizeof(streamPrefix) - 1;
            const auto dot = key.find('.', indexStart);
            if (dot == std::string::npos) continue;
            const int ordinal = static_cast<int>(Integer(key.substr(indexStart, dot - indexStart)));
            std::string field = key.substr(dot + 1);
            if (field.compare(0, 12, "disposition.") == 0) {
                AssignStream(streams[ordinal], field, value);
            } else if (field.find("side_data_list.") == 0) {
                const auto lastDot = field.find_last_of('.');
                AssignStream(streams[ordinal], lastDot == std::string::npos ? field : field.substr(lastDot + 1), value);
            } else {
                AssignStream(streams[ordinal], field, value);
            }
        } else if (key == "format.format_name") result.formatName = Utf8(value);
        else if (key == "format.start_time") result.startSeconds = Floating(value);
        else if (key == "format.duration") result.durationSeconds = Floating(value);
        else if (key == "format.bit_rate") result.bitRate = Integer(value);
        else if (key.compare(0, 17, "chapters.chapter.") == 0) {
            const auto dot = key.find('.', 17);
            const int ordinal = static_cast<int>(Integer(key.substr(17, dot - 17)));
            result.chapterCount = (std::max)(result.chapterCount, ordinal + 1);
        }
    }
    for (auto& item : streams) {
        if (item.second.bitDepth == 0) item.second.bitDepth = BitDepthFromPixelFormat(item.second.pixelFormat);
        result.streams.push_back(std::move(item.second));
    }
    result.valid = result.MainVideo() != nullptr;
    if (!result.valid) result.error = L"FFprobe не знайшов основний відеопотік.";
    return result;
}

MediaProbeResult ProbeMediaFile(
    const std::wstring& ffprobePath,
    const std::wstring& sourcePath,
    std::atomic_bool* cancellation) {
    const std::vector<std::wstring> arguments{
        L"-v", L"error", L"-print_format", L"flat",
        L"-show_format", L"-show_streams", L"-show_chapters", PathForWin32Io(sourcePath)};
    ExportProcessResult process = RunHiddenExportProcess(
        ffprobePath, arguments, cancellation, {}, 4U * 1024U * 1024U);
    if (!process.started || process.exitCode != 0 || process.cancelled) {
        MediaProbeResult result{};
        result.error = process.cancelled ? L"Аналіз скасовано." :
            (!process.error.empty() ? process.error : L"FFprobe завершився з помилкою: " + Utf8(process.standardError));
        return result;
    }
    MediaProbeResult result = ParseFfprobeFlat(process.standardOutput);
    const auto separator = ffprobePath.find_last_of(L"\\/");
    const std::wstring directory = separator == std::wstring::npos
        ? L"." : ffprobePath.substr(0, separator);
    ExportProcessResult probeVersion = RunHiddenExportProcess(
        ffprobePath, {L"-version"}, cancellation, {}, 64U * 1024U);
    const auto probeLine = probeVersion.standardOutput.find('\n');
    result.ffprobeVersion = Utf8(probeVersion.standardOutput.substr(0, probeLine));
    const std::wstring ffmpegPath = directory + L"\\ffmpeg.exe";
    ExportProcessResult ffmpegVersion = RunHiddenExportProcess(
        ffmpegPath, {L"-version"}, cancellation, {}, 64U * 1024U);
    const auto ffmpegLine = ffmpegVersion.standardOutput.find('\n');
    result.ffmpegVersion = Utf8(ffmpegVersion.standardOutput.substr(0, ffmpegLine));
    ExportProcessResult encoders = RunHiddenExportProcess(
        ffmpegPath, {L"-hide_banner", L"-encoders"}, cancellation, {}, 1024U * 1024U);
    const std::string encoderText = encoders.standardOutput + encoders.standardError;
    for (const wchar_t* candidate : {L"libx264", L"libx265", L"mpeg4",
            L"libvpx-vp9", L"libaom-av1", L"aac", L"libopus"}) {
        std::string narrow;
        for (const wchar_t value : std::wstring(candidate)) narrow.push_back(static_cast<char>(value));
        if (encoderText.find(" " + narrow + " ") != std::string::npos) {
            result.availableEncoders.emplace_back(candidate);
        }
    }
    return result;
}

std::wstring DescribeMediaProbe(const MediaProbeResult& probe) {
    const MediaStreamInfo* video = probe.MainVideo();
    if (video == nullptr) return probe.error;
    int videoCount = 0;
    int audioCount = 0;
    int subtitleCount = 0;
    int attachmentCount = 0;
    int coverCount = 0;
    for (const auto& stream : probe.streams) {
        if (stream.codecType == L"video") {
            ++videoCount;
            if (stream.attachedPicture) ++coverCount;
        } else if (stream.codecType == L"audio") ++audioCount;
        else if (stream.codecType == L"subtitle") ++subtitleCount;
        else if (stream.codecType == L"attachment") ++attachmentCount;
    }
    std::wostringstream text;
    text << L"Контейнер: " << probe.formatName << L"; тривалість: " << probe.durationSeconds
        << L" с\r\nВідео #" << video->index << L": " << video->codecName;
    if (!video->profile.empty()) text << L" (" << video->profile << L")";
    text << L", " << video->width << L"×" << video->height
        << L", SAR " << video->sampleAspectRatio.numerator << L":" << video->sampleAspectRatio.denominator
        << L", fps " << video->averageFrameRate.numerator << L"/" << video->averageFrameRate.denominator
        << L", " << video->pixelFormat << L", " << video->bitDepth << L"-bit";
    if (video->level != 0) text << L", level " << video->level;
    if (!video->fieldOrder.empty() && video->fieldOrder != L"progressive" &&
        video->fieldOrder != L"unknown") text << L", " << video->fieldOrder;
    text << L"\r\ntime_base " << video->timeBase.numerator << L"/" << video->timeBase.denominator
        << L"; r_frame_rate " << video->realFrameRate.numerator << L"/" << video->realFrameRate.denominator
        << L"; bitrate " << video->bitRate;
    if (!video->colorPrimaries.empty() || !video->colorTransfer.empty() || !video->colorSpace.empty()) {
        text << L"; color " << video->colorPrimaries << L"/" << video->colorTransfer
            << L"/" << video->colorSpace;
    }
    if (video->rotationDegrees != 0) text << L", rotation " << video->rotationDegrees << L"°";
    text << L"\r\nПотоки: video " << videoCount << L" (cover art " << coverCount
        << L"), audio " << audioCount << L", subtitles " << subtitleCount
        << L", attachments " << attachmentCount << L"; chapters " << probe.chapterCount
        << L"; усього " << probe.streams.size();
    if (!probe.ffmpegVersion.empty()) text << L"\r\n" << probe.ffmpegVersion;
    return text.str();
}

}  // namespace videoplayer
