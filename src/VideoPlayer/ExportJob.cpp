#include "targetver.h"
#include "ExportJob.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "ExportProcess.h"
#include "ExportPlanner.h"
#include "MediaProbe.h"
#include "PathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>

namespace videoplayer {
namespace {

std::mutex gExportMutex;

ExportFileIdentity ReadIdentity(const std::wstring& path) {
    ExportFileIdentity identity{};
    HANDLE file = CreateFileW(PathForWin32Io(path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return identity;
    identity.exists = true;
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(file, &information) != FALSE) {
        identity.valid = true;
        identity.volume = information.dwVolumeSerialNumber;
        identity.fileIndex = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32U) |
            information.nFileIndexLow;
        identity.size = (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) |
            information.nFileSizeLow;
        identity.modifiedTime =
            (static_cast<std::uint64_t>(information.ftLastWriteTime.dwHighDateTime) << 32U) |
            information.ftLastWriteTime.dwLowDateTime;
    }
    CloseHandle(file);
    return identity;
}

bool SameIdentity(const ExportFileIdentity& first, const ExportFileIdentity& second) {
    return (first.exists == second.exists && !first.exists) ||
        (first.valid && second.valid && first.volume == second.volume &&
         first.fileIndex == second.fileIndex && first.size == second.size &&
         first.modifiedTime == second.modifiedTime);
}

std::wstring Utf8(const std::string& value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), count);
    return result;
}

std::wstring JobId() {
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK) return std::to_wstring(GetTickCount64());
    wchar_t text[40]{};
    StringFromGUID2(guid, text, static_cast<int>(std::size(text)));
    std::wstring result(text);
    result.erase(std::remove_if(result.begin(), result.end(),
        [](const wchar_t value) { return value == L'{' || value == L'}' || value == L'-'; }), result.end());
    return result;
}

bool SameFile(const std::wstring& first, const std::wstring& second) {
    HANDLE one = CreateFileW(PathForWin32Io(first).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE two = CreateFileW(PathForWin32Io(second).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (one == INVALID_HANDLE_VALUE || two == INVALID_HANDLE_VALUE) {
        if (one != INVALID_HANDLE_VALUE) CloseHandle(one);
        if (two != INVALID_HANDLE_VALUE) CloseHandle(two);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION a{}, b{};
    const bool equal = GetFileInformationByHandle(one, &a) != FALSE &&
        GetFileInformationByHandle(two, &b) != FALSE &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
    CloseHandle(one);
    CloseHandle(two);
    return equal;
}

int ParseProgress(const std::string& line, const std::int64_t durationMs) {
    static constexpr char prefix[] = "out_time_ms=";
    if (line.compare(0, sizeof(prefix) - 1, prefix) != 0 || durationMs <= 0) return -1;
    const long long microseconds = std::strtoll(line.c_str() + sizeof(prefix) - 1, nullptr, 10);
    return static_cast<int>((std::clamp)(microseconds / (durationMs * 1000.0) * 100.0, 0.0, 99.0));
}

enum class SourceAudioPresence { Absent, Present, Unknown, Cancelled };

SourceAudioPresence CheckSourceIntervalAudio(
    const std::wstring& ffmpegPath,
    const std::wstring& ffprobePath,
    const ExportSnapshot& snapshot,
    const ExportPlan& plan,
    std::atomic_bool& cancellation) {
    const auto source = ProbeMediaFile(ffprobePath, snapshot.sourcePath, &cancellation);
    if (cancellation.load()) return SourceAudioPresence::Cancelled;
    if (!source.valid) return SourceAudioPresence::Unknown;
    if (!source.HasAudio()) return SourceAudioPresence::Absent;

    // A container may declare an empty audio stream, or its audio may end
    // before A. Use the export's own timestamp normalization/seek to check
    // actual decoded samples; raw packet timestamps can differ for TS/HLS.
    // Keep the main video enabled as in the real export: TS/HLS timestamp
    // normalization can otherwise shift an audio-only input differently.
    // Checksums go to a pipe, so this creates no file and adds no silent track.
    std::atomic_bool hasSamples{false};
    const std::vector<std::wstring> arguments{
        L"-hide_banner", L"-v", L"error", L"-xerror",
        L"-i", PathForWin32Io(snapshot.sourcePath),
        L"-ss", FormatExportSeconds(plan.keyframeAligned ? plan.copySeekMs : plan.startMs),
        L"-t", FormatExportSeconds(plan.endMs - plan.startMs),
        L"-map", L"0:" + std::to_wstring(source.MainVideo()->index),
        L"-map", L"0:a?", L"-sn", L"-dn",
        L"-c:v", L"copy", L"-c:a", L"pcm_s16le", L"-f", L"framecrc", L"pipe:1"};
    const auto decoded = RunHiddenExportProcess(ffmpegPath, arguments, &cancellation,
        [&](const std::string& line) {
            const auto first = line.find_first_not_of(" \t\r");
            // Output stream 0 is the copied video; streams 1+ are decoded audio.
            // Unknown data also fails closed rather than proving audio absent.
            if (first != std::string::npos && line[first] != '#') {
                char* end = nullptr;
                const auto streamIndex = std::strtol(line.c_str() + first, &end, 10);
                if (end == line.c_str() + first || end == nullptr || *end != ',' ||
                    streamIndex != 0) {
                    hasSamples.store(true);
                }
            }
        }, 64U * 1024U);
    if (decoded.cancelled || cancellation.load()) return SourceAudioPresence::Cancelled;
    if (!decoded.started || decoded.exitCode != 0) return SourceAudioPresence::Unknown;
    return hasSamples.load() ? SourceAudioPresence::Present : SourceAudioPresence::Absent;
}

}  // namespace

ExportFileIdentity CaptureExportFileIdentity(const std::wstring& path) {
    return ReadIdentity(path);
}

ExportJobResult RunExportJob(
    const std::wstring& ffmpegPath,
    const std::wstring& ffprobePath,
    const ExportSnapshot& snapshot,
    const ExportOptions& options,
    const ExportPlan& plan,
    const bool overwriteConfirmed,
    std::atomic_bool& cancellation,
    ExportProgress progress) {
    std::unique_lock<std::mutex> oneJob(gExportMutex, std::try_to_lock);
    ExportJobResult result{};
    result.finalPath = options.destinationPath;
    if (!oneJob.owns_lock()) {
        result.diagnostics = L"У цьому процесі вже виконується інший експорт.";
        return result;
    }
    if (!plan.valid) {
        result.diagnostics = plan.error;
        return result;
    }
    if (options.fragment == ExportFragment::Segment && options.image == ExportImage::FullFrame &&
        (!plan.streamCopy || !plan.keyframeAligned)) {
        result.diagnostics = L"A–B без зуму зберігається лише копіюванням після аналізу ключових кадрів.";
        return result;
    }
    const ExportFileIdentity sourceAtStart = ReadIdentity(snapshot.sourcePath);
    const ExportFileIdentity destinationAtStart = ReadIdentity(options.destinationPath);
    if (!snapshot.sourceIdentity.valid || !sourceAtStart.valid) {
        result.diagnostics = L"Не вдалося зафіксувати identity файла-джерела.";
        return result;
    }
    if (!SameIdentity(snapshot.sourceIdentity, sourceAtStart)) {
        result.diagnostics = L"Файл-джерело змінився після відкриття exporter; запуск зупинено.";
        return result;
    }
    if (SameFile(snapshot.sourcePath, options.destinationPath)) {
        result.diagnostics = L"Файл результату є тим самим файлом (або hard link), що й джерело.";
        return result;
    }
    const DWORD destinationAttributes = GetFileAttributesW(PathForWin32Io(options.destinationPath).c_str());
    if (destinationAttributes != INVALID_FILE_ATTRIBUTES && !overwriteConfirmed) {
        result.diagnostics = L"Файл результату вже існує; перезапис не підтверджено.";
        return result;
    }
    result.partialPath = options.destinationPath + L"." + JobId() + L".part";
    std::vector<std::wstring> arguments = plan.ffmpegArguments;
    arguments.push_back(PathForWin32Io(result.partialPath));
    const std::int64_t durationMs = plan.endMs - plan.startMs;
    const std::wstring operation = plan.streamCopy ? L"Копіювання без перекодування…" : L"Експорт відео…";
    if (progress) progress(0, operation);
    ExportProcessResult process = RunHiddenExportProcess(
        ffmpegPath, arguments, &cancellation,
        [&](const std::string& line) {
            const int percent = ParseProgress(line, durationMs);
            if (progress && percent >= 0) progress(percent, operation);
        });
    if (process.cancelled || cancellation.load()) {
        result.cancelled = true;
        result.diagnostics = L"Експорт скасовано користувачем.";
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    if (!process.started || process.exitCode != 0) {
        result.diagnostics = !process.error.empty() ? process.error :
            L"FFmpeg завершився з кодом " + std::to_wstring(process.exitCode) + L".\r\n" + Utf8(process.standardError);
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }

    if (progress) progress(99, L"Перевірка результату…");
    MediaProbeResult verification = ProbeMediaFile(ffprobePath, result.partialPath, &cancellation);
    const MediaStreamInfo* verifiedVideo = verification.MainVideo();
    const double expectedSeconds = static_cast<double>(durationMs) / 1000.0;
    const double durationTolerance = (std::max)(0.35, expectedSeconds * 0.03);
    int videoStreamCount = 0;
    std::vector<std::wstring> verifiedAudioCodecs;
    for (const auto& stream : verification.streams) {
        if (stream.codecType == L"video" && !stream.attachedPicture) ++videoStreamCount;
        if (stream.codecType == L"audio") verifiedAudioCodecs.push_back(stream.codecName);
    }
    bool acceptsAbsentAudio = false;
    if (verification.valid && plan.outputHasAudio && !verification.HasAudio()) {
        if (progress) progress(99, L"Перевірка аудіо фрагмента…");
        const auto sourceAudio = CheckSourceIntervalAudio(
            ffmpegPath, ffprobePath, snapshot, plan, cancellation);
        if (sourceAudio == SourceAudioPresence::Cancelled) {
            result.cancelled = true;
            result.diagnostics = L"Перевірку скасовано користувачем.";
            DeleteFileW(PathForWin32Io(result.partialPath).c_str());
            return result;
        }
        // Missing output audio is harmless only when its source interval has
        // no samples. Real audio loss or an inconclusive check remains an error.
        acceptsAbsentAudio = sourceAudio == SourceAudioPresence::Absent;
    }
    const bool audioMatches = acceptsAbsentAudio ||
        (verification.HasAudio() == plan.outputHasAudio &&
         verifiedAudioCodecs == plan.expectedAudioCodecs);
    const bool sampleAspectMatches = verifiedVideo != nullptr &&
        verifiedVideo->sampleAspectRatio.denominator != 0 &&
        plan.expectedSampleAspectRatio.denominator != 0 &&
        verifiedVideo->sampleAspectRatio.numerator * plan.expectedSampleAspectRatio.denominator ==
            plan.expectedSampleAspectRatio.numerator * verifiedVideo->sampleAspectRatio.denominator;
    if (!verification.valid || verifiedVideo == nullptr ||
        verification.durationSeconds <= 0.0 ||
        std::abs(verification.durationSeconds - expectedSeconds) > durationTolerance ||
        verifiedVideo->width != plan.outputWidth || verifiedVideo->height != plan.outputHeight ||
        !audioMatches || videoStreamCount != 1 ||
        verifiedVideo->codecName != plan.expectedVideoCodec ||
        (!plan.expectedPixelFormat.empty() &&
            verifiedVideo->pixelFormat != plan.expectedPixelFormat) ||
        (plan.expectedBitDepth != 0 && verifiedVideo->bitDepth != plan.expectedBitDepth) ||
        !sampleAspectMatches ||
        verifiedVideo->rotationDegrees != plan.expectedRotationDegrees) {
        result.diagnostics = L"Перевірка контейнера не пройдена. Очікувалось: " +
            std::to_wstring(plan.outputWidth) + L"×" + std::to_wstring(plan.outputHeight) +
            L", " + FormatExportSeconds(durationMs) + L" с, audio=" +
            (plan.outputHasAudio ? L"так" : L"ні") + L". Отримано: " +
            (verifiedVideo == nullptr ? L"немає відео" :
                std::to_wstring(verifiedVideo->width) + L"×" + std::to_wstring(verifiedVideo->height)) +
            L", " + std::to_wstring(verification.durationSeconds) + L" с, audio=" +
            (verification.HasAudio() ? L"так" : L"ні") + L". " + verification.error;
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    const std::vector<std::wstring> decodeArguments{
        L"-v", L"error", L"-xerror", L"-i", PathForWin32Io(result.partialPath),
        L"-map", L"0:v:0", L"-map", L"0:a?", L"-f", L"null", L"NUL"};
    ExportProcessResult decode = RunHiddenExportProcess(
        ffmpegPath, decodeArguments, &cancellation, {}, 256U * 1024U);
    if (decode.cancelled || cancellation.load()) {
        result.cancelled = true;
        result.diagnostics = L"Перевірку скасовано користувачем.";
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    if (!decode.started || decode.exitCode != 0) {
        result.diagnostics = (plan.keyframeAligned
            ? L"Скопійований фрагмент не декодується безпечно (можливі відкриті GOP). Виберіть інші межі A–B; автоматичного перекодування немає.\r\n"
            : L"Декодування перевірочного файла завершилося помилкою.\r\n") + Utf8(decode.standardError);
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    if (SameFile(snapshot.sourcePath, options.destinationPath)) {
        result.diagnostics = L"Під час експорту результат став тотожним джерелу; фіналізацію зупинено.";
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    if (!SameIdentity(snapshot.sourceIdentity, ReadIdentity(snapshot.sourcePath))) {
        result.diagnostics = L"Файл-джерело змінився під час експорту; фіналізацію зупинено.";
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    if (!SameIdentity(destinationAtStart, ReadIdentity(options.destinationPath))) {
        result.diagnostics = L"Файл призначення змінився під час експорту; його не перезаписано.";
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (overwriteConfirmed ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExW(PathForWin32Io(result.partialPath).c_str(),
            PathForWin32Io(options.destinationPath).c_str(), flags) == FALSE) {
        result.diagnostics = L"Не вдалося атомарно перейменувати partial-файл (Win32 " +
            std::to_wstring(GetLastError()) + L").";
        DeleteFileW(PathForWin32Io(result.partialPath).c_str());
        return result;
    }
    result.partialPath.clear();
    result.success = true;
    if (progress) progress(100, L"Готово");
    return result;
}

}  // namespace videoplayer
