#include "targetver.h"
#include "RemoveSegment.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "ExportPlanner.h"
#include "ExportProcess.h"
#include "MediaProbe.h"
#include "PathUtils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>

namespace videoplayer {
namespace {

std::mutex gRemovalMutex;

bool SameIdentity(const ExportFileIdentity& a, const ExportFileIdentity& b) {
    return (!a.exists && !b.exists) ||
        (a.valid && b.valid && a.volume == b.volume && a.fileIndex == b.fileIndex &&
         a.size == b.size && a.modifiedTime == b.modifiedTime);
}

bool SameFile(const ExportFileIdentity& a, const ExportFileIdentity& b) {
    return a.valid && b.valid && a.volume == b.volume && a.fileIndex == b.fileIndex;
}

std::wstring Utf8(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length);
    return result;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring UniqueSuffix() {
    GUID guid{};
    if (CoCreateGuid(&guid) != S_OK) return std::to_wstring(GetTickCount64());
    wchar_t buffer[40]{};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    std::wstring result(buffer);
    result.erase(std::remove_if(result.begin(), result.end(),
        [](wchar_t c) { return c == L'{' || c == L'}' || c == L'-'; }), result.end());
    return result;
}

std::wstring Seconds(std::int64_t milliseconds) {
    return FormatExportSeconds(milliseconds);
}

std::wstring ProcessFailure(const ExportProcessResult& process) {
    return !process.error.empty() ? process.error :
        L"FFmpeg/ffprobe завершився з кодом " + std::to_wstring(process.exitCode) +
        L".\r\n" + Utf8(process.standardError);
}

std::string ConcatPath(const std::wstring& path) {
    std::wstring slashPath = PathForWin32Io(path);
    std::replace(slashPath.begin(), slashPath.end(), L'\\', L'/');
    const std::string utf8 = Utf8(slashPath);
    std::string escaped;
    for (const char c : utf8) {
        if (c == ' ' || c == '\\' || c == '\'') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return escaped;
}

struct TemporaryFiles final {
    std::vector<std::wstring> paths;
    ~TemporaryFiles() {
        for (const auto& path : paths) DeleteFileW(PathForWin32Io(path).c_str());
    }
};

bool RunCopy(const std::wstring& ffmpeg, const ExportSnapshot& snapshot,
    const MediaProbeResult& probe, const RemovalPlan& plan,
    const std::wstring& destination, std::int64_t startMs, std::int64_t lengthMs,
    std::atomic_bool& cancellation, std::wstring& error) {
    const MediaStreamInfo* video = probe.MainVideo();
    if (video == nullptr) {
        error = L"Відеопотік не знайдено.";
        return false;
    }
    std::vector<std::wstring> args{L"-hide_banner", L"-nostdin", L"-v", L"error",
        L"-y"};
    // Keyframes are relative to the main video's first PTS, whereas input
    // seeking is relative to the container start. These differ for AAC/Opus
    // priming and for containers whose audio begins before the video.
    const auto seekMs = startMs + static_cast<std::int64_t>(
        std::llround((video->startSeconds - probe.startSeconds) * 1000.0));
    if (seekMs > 0) {
        args.push_back(L"-ss");
        args.push_back(Seconds(seekMs));
    }
    args.insert(args.end(), {L"-i", PathForWin32Io(snapshot.sourcePath),
        L"-t", Seconds(lengthMs), L"-map", L"0:" + std::to_wstring(video->index),
        L"-map", L"0:a?", L"-map_chapters", L"-1", L"-sn", L"-dn",
        L"-c", L"copy",
        // Some demuxers seek to a preceding GOP. Stream-copy -t also stops
        // on decode time and can include reordered packets beyond the end.
        // Drop both kinds of preroll/overrun by their presentation time;
        // amount=0 leaves every retained encoded payload unchanged. Allow
        // the half-millisecond rounding used by the keyframe planner.
        L"-bsf:v", L"noise=amount=0:drop=lt(pts*tb\\,-0.0005)+gte(pts*tb\\," +
            Seconds(lengthMs) + L"-0.0005)",
        L"-bsf:a", L"noise=amount=0:drop=lt(pts*tb\\,0)+gte(pts*tb\\," +
            Seconds(lengthMs) + L")",
        L"-avoid_negative_ts", L"disabled",
        L"-f", plan.muxer, PathForWin32Io(destination)});
    const ExportProcessResult process =
        RunHiddenExportProcess(ffmpeg, args, &cancellation, {}, 256U * 1024U);
    if (process.cancelled || cancellation.load()) {
        error = L"Операцію скасовано.";
        return false;
    }
    if (!process.started || process.exitCode != 0) {
        error = ProcessFailure(process);
        return false;
    }
    return true;
}

}  // namespace

std::vector<std::int64_t> ProbeVideoKeyframes(
    const std::wstring& ffprobePath, const std::wstring& sourcePath,
    const MediaProbeResult& probe, std::atomic_bool* cancellation,
    std::wstring& error) {
    std::vector<std::int64_t> keyframes;
    const MediaStreamInfo* video = probe.MainVideo();
    if (!probe.valid || video == nullptr || probe.durationSeconds <= 0.0) {
        error = L"Немає коректної тривалості або відеопотоку.";
        return keyframes;
    }
    const std::vector<std::wstring> args{
        L"-v", L"error", L"-select_streams", std::to_wstring(video->index),
        L"-show_packets", L"-show_entries", L"packet=pts_time,flags",
        L"-of", L"compact=p=0:nk=0", PathForWin32Io(sourcePath)};
    const ExportProcessResult process = RunHiddenExportProcess(ffprobePath, args,
        cancellation, [&](const std::string& line) {
            if (line.find("K") == std::string::npos) return;
            const std::size_t start = line.find("pts_time=");
            if (start == std::string::npos) return;
            char* end = nullptr;
            const double pts = std::strtod(line.c_str() + start + 9, &end);
            if (end == line.c_str() + start + 9 || !std::isfinite(pts)) return;
            const auto ms = static_cast<std::int64_t>(
                std::llround((pts - video->startSeconds) * 1000.0));
            const auto duration = static_cast<std::int64_t>(
                std::llround(probe.durationSeconds * 1000.0));
            if (ms >= 0 && ms < duration) keyframes.push_back(ms);
        }, 0);
    if (process.cancelled || (cancellation != nullptr && cancellation->load())) {
        error = L"Аналіз ключових кадрів скасовано.";
        return {};
    }
    if (!process.started || process.exitCode != 0) {
        error = ProcessFailure(process);
        return {};
    }
    std::sort(keyframes.begin(), keyframes.end());
    keyframes.erase(std::unique(keyframes.begin(), keyframes.end()), keyframes.end());
    if (keyframes.empty()) error = L"Ключові кадри не знайдено; без перекодування видалення неможливе.";
    return keyframes;
}

RemovalPlan BuildRemovalPlan(const ExportSnapshot& snapshot,
    const MediaProbeResult& probe, const std::vector<std::int64_t>& keyframes,
    const std::wstring& destinationPath) {
    RemovalPlan plan{};
    plan.requestedIntervals = snapshot.removalIntervals;
    if (plan.requestedIntervals.empty() && snapshot.hasSegment) {
        plan.requestedIntervals.push_back(
            {snapshot.segmentStartMs, snapshot.segmentEndMs});
    }
    if (plan.requestedIntervals.empty()) {
        plan.error = L"Спочатку додайте хоча б один інтервал A–B для видалення.";
        return plan;
    }
    if (!probe.valid || probe.MainVideo() == nullptr || keyframes.empty() ||
        !std::isfinite(probe.durationSeconds) || probe.durationSeconds <= 0.0 ||
        probe.durationSeconds >=
            static_cast<double>((std::numeric_limits<std::int64_t>::max)()) / 1000.0) {
        plan.error = L"Не вдалося проаналізувати відео та ключові кадри.";
        return plan;
    }
    const auto durationMs = static_cast<std::int64_t>(
        std::llround(probe.durationSeconds * 1000.0));
    if (durationMs <= 0) {
        plan.error = L"Тривалість відео некоректна.";
        return plan;
    }
    plan.muxer = ExportMuxerForPath(destinationPath);
    if (plan.muxer.empty()) {
        plan.error = L"Підтримуються лише MP4, MOV, M4V, MKV та WebM.";
        return plan;
    }
    std::vector<std::int64_t> boundaries;
    for (const auto keyframe : keyframes) {
        if (keyframe >= 0 && keyframe < durationMs) boundaries.push_back(keyframe);
    }
    if (boundaries.empty()) {
        plan.error = L"Ключові кадри виходять за тривалість відео.";
        return plan;
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    std::vector<VideoInterval> aligned;
    for (auto& interval : plan.requestedIntervals) {
        // Some containers report their duration a fraction of a second shorter
        // than the player's last timestamp. Only tolerate this at the end.
        if (interval.startMs < 0 || interval.startMs >= durationMs ||
            interval.endMs <= interval.startMs ||
            interval.endMs - (std::min)(interval.endMs, durationMs) > 500) {
            plan.error = L"Кожен інтервал повинен мати A < B і лежати в межах відео.";
            return plan;
        }
        interval.endMs = (std::min)(interval.endMs, durationMs);
        const auto left = std::upper_bound(boundaries.begin(), boundaries.end(), interval.startMs);
        const auto right = std::lower_bound(boundaries.begin(), boundaries.end(), interval.endMs);
        aligned.push_back({left == boundaries.begin() ? 0 : *std::prev(left),
            right == boundaries.end() ? durationMs : *right});
    }
    std::sort(aligned.begin(), aligned.end(),
        [](const VideoInterval& a, const VideoInterval& b) {
            return a.startMs < b.startMs ||
                (a.startMs == b.startMs && a.endMs < b.endMs);
        });
    for (const auto& interval : aligned) {
        if (!plan.actualIntervals.empty() &&
            interval.startMs <= plan.actualIntervals.back().endMs) {
            plan.actualIntervals.back().endMs =
                (std::max)(plan.actualIntervals.back().endMs, interval.endMs);
        } else {
            plan.actualIntervals.push_back(interval);
        }
    }
    std::int64_t cursor = 0;
    for (const auto& interval : plan.actualIntervals) {
        if (cursor < interval.startMs) {
            plan.retainedIntervals.push_back({cursor, interval.startMs});
            plan.expectedDurationMs += interval.startMs - cursor;
        }
        cursor = interval.endMs;
    }
    if (cursor < durationMs) {
        plan.retainedIntervals.push_back({cursor, durationMs});
        plan.expectedDurationMs += durationMs - cursor;
    }
    if (plan.retainedIntervals.empty()) {
        plan.error = L"Округлення інтервалів до ключових кадрів видалило б усе відео.";
        return plan;
    }
    plan.summary = L"Без перекодування буде видалено (" +
        std::to_wstring(plan.actualIntervals.size()) + L"):\r\n";
    for (const auto& interval : plan.actualIntervals) {
        plan.summary += L"  " + Seconds(interval.startMs) + L"–" +
            Seconds(interval.endMs) + L" с\r\n";
    }
    plan.summary += L"\r\nМежі розширено до ключових кадрів; інтервали, що "
        L"перетинаються або дотикаються, об'єднано.\r\n"
        L"Орієнтовна тривалість результату: " +
        Seconds(plan.expectedDurationMs) + L" с.\r\n\r\n";
    plan.summary += L"Вибрані інтервали (" +
        std::to_wstring(plan.requestedIntervals.size()) + L"):\r\n";
    for (const auto& interval : plan.requestedIntervals) {
        plan.summary += L"  " + Seconds(interval.startMs) + L"–" +
            Seconds(interval.endMs) + L" с\r\n";
    }
    plan.summary += L"\r\nВідео й аудіо копіюються без перекодування. Інші відеопотоки, "
        L"субтитри та глави не переносяться.";
    plan.valid = true;
    return plan;
}

ExportJobResult RunRemovalJob(
    const std::wstring& ffmpegPath, const std::wstring& ffprobePath,
    const ExportSnapshot& snapshot, const MediaProbeResult& probe,
    const RemovalPlan& plan, const std::wstring& destinationPath,
    const bool overwriteConfirmed, const bool replaceSource,
    std::atomic_bool& cancellation, ExportProgress progress) {
    ExportJobResult result{};
    result.finalPath = destinationPath;
    std::unique_lock<std::mutex> lock(gRemovalMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        result.diagnostics = L"Уже виконується інше видалення інтервалів.";
        return result;
    }
    if (!plan.valid || plan.retainedIntervals.empty()) {
        result.diagnostics = plan.error;
        return result;
    }
    const ExportFileIdentity sourceAtStart = CaptureExportFileIdentity(snapshot.sourcePath);
    const ExportFileIdentity destAtStart = CaptureExportFileIdentity(destinationPath);
    if (!SameIdentity(snapshot.sourceIdentity, sourceAtStart) || !sourceAtStart.valid) {
        result.diagnostics = L"Файл-джерело змінився після відкриття; операцію скасовано.";
        return result;
    }
    if (replaceSource) {
        if (_wcsicmp(destinationPath.c_str(), snapshot.sourcePath.c_str()) != 0 ||
            !SameFile(sourceAtStart, destAtStart)) {
            result.diagnostics = L"Перезапис дозволено лише для відкритого файла.";
            return result;
        }
    } else if (SameFile(sourceAtStart, destAtStart)) {
        result.diagnostics = L"Шлях результату посилається на джерело або його hard link.";
        return result;
    }
    if (destAtStart.exists && !overwriteConfirmed) {
        result.diagnostics = L"Перезапис файла не підтверджено.";
        return result;
    }
    const std::wstring suffix = L"." + UniqueSuffix();
    const std::wstring extension = plan.muxer == L"matroska" ? L".mkv" :
        plan.muxer == L"webm" ? L".webm" : L".mp4";
    result.partialPath = destinationPath + suffix + L".part";
    TemporaryFiles cleanup{};
    std::vector<std::wstring> pieces;
    std::wstring error;
    for (std::size_t index = 0; index < plan.retainedIntervals.size(); ++index) {
        if (cancellation.load()) {
            result.cancelled = true;
            result.diagnostics = L"Копіювання скасовано.";
            return result;
        }
        const auto& interval = plan.retainedIntervals[index];
        if (interval.startMs < 0 || interval.endMs <= interval.startMs) {
            result.diagnostics = L"Некоректний план фрагментів для збереження.";
            return result;
        }
        const std::wstring piece = destinationPath + suffix + L".piece-" +
            std::to_wstring(index + 1) + extension;
        cleanup.paths.push_back(piece);
        if (progress) progress(5 + static_cast<int>(index * 55 /
                plan.retainedIntervals.size()),
            L"Копіювання фрагмента " + std::to_wstring(index + 1) + L" з " +
                std::to_wstring(plan.retainedIntervals.size()) + L"…");
        if (!RunCopy(ffmpegPath, snapshot, probe, plan, piece, interval.startMs,
                interval.endMs - interval.startMs, cancellation, error)) {
            result.cancelled = cancellation.load();
            result.diagnostics = error;
            return result;
        }
        pieces.push_back(piece);
    }
    // Keep the partial path last so the verified source-replacement result can
    // be handed to the UI while all intermediate pieces are still cleaned up.
    cleanup.paths.push_back(result.partialPath);
    if (pieces.empty()) {
        result.diagnostics = L"Немає фрагментів для збереження.";
        return result;
    }
    if (progress) progress(65, L"Об'єднання фрагментів без перекодування…");
    if (pieces.size() == 1) {
        if (MoveFileExW(PathForWin32Io(pieces.front()).c_str(),
                PathForWin32Io(result.partialPath).c_str(), 0) == FALSE) {
            result.diagnostics = L"Не вдалося підготувати результат (Win32 " +
                std::to_wstring(GetLastError()) + L").";
            return result;
        }
    } else {
        std::string manifest = "ffconcat version 1.0\n";
        for (std::size_t index = 0; index < pieces.size(); ++index) {
            const auto& interval = plan.retainedIntervals[index];
            manifest += "file " + ConcatPath(pieces[index]) + "\n";
            // Container duration can include AAC priming or rounding. Keep
            // joins on the source's video timeline rather than accumulating
            // that overhead once for every retained piece.
            manifest += "duration " + Utf8(Seconds(interval.endMs - interval.startMs)) + "\n";
        }
        const std::vector<std::wstring> args{L"-hide_banner", L"-v", L"error",
            L"-y", L"-f", L"concat", L"-auto_convert", L"0", L"-safe", L"0",
            L"-protocol_whitelist", L"file,pipe", L"-i", L"pipe:0",
            L"-map", L"0", L"-c", L"copy", L"-f", plan.muxer,
            PathForWin32Io(result.partialPath)};
        const auto process = RunHiddenExportProcess(
            ffmpegPath, args, &cancellation, {}, 256U * 1024U, manifest);
        if (process.cancelled || cancellation.load()) {
            result.cancelled = true;
            result.diagnostics = L"Об'єднання скасовано.";
            return result;
        }
        if (!process.started || process.exitCode != 0) {
            result.diagnostics = ProcessFailure(process);
            return result;
        }
    }
    if (progress) progress(80, L"Перевірка контейнера й декодування…");
    const MediaProbeResult verified =
        ProbeMediaFile(ffprobePath, result.partialPath, &cancellation);
    const MediaStreamInfo* originalVideo = probe.MainVideo();
    const MediaStreamInfo* verifiedVideo = verified.MainVideo();
    std::vector<std::wstring> originalAudio, verifiedAudio;
    int verifiedVideoCount = 0;
    for (const auto& stream : probe.streams) {
        if (stream.codecType == L"audio") originalAudio.push_back(stream.codecName);
    }
    for (const auto& stream : verified.streams) {
        if (stream.codecType == L"audio") verifiedAudio.push_back(stream.codecName);
        if (stream.codecType == L"video" && !stream.attachedPicture) ++verifiedVideoCount;
    }
    const double expected = plan.expectedDurationMs / 1000.0;
    if (!verified.valid || verifiedVideo == nullptr || originalVideo == nullptr ||
        verifiedVideo->codecName != originalVideo->codecName ||
        verifiedVideo->width != originalVideo->width ||
        verifiedVideo->height != originalVideo->height ||
        verifiedVideo->rotationDegrees != originalVideo->rotationDegrees ||
        verifiedVideoCount != 1 || verifiedAudio != originalAudio ||
        std::abs(verified.durationSeconds - expected) > (std::max)(0.75, expected * 0.04)) {
        result.diagnostics = L"Перевірка контейнера/тривалості не пройдена. "
            L"Очікувалось приблизно " + Seconds(plan.expectedDurationMs) +
            L" с; отримано " +
            std::to_wstring(verified.durationSeconds) + L" с. " + verified.error;
        return result;
    }
    const std::vector<std::wstring> decodeArgs{L"-v", L"error", L"-xerror",
        L"-i", PathForWin32Io(result.partialPath), L"-map", L"0:v:0",
        L"-map", L"0:a?", L"-f", L"null", L"NUL"};
    const auto decoded = RunHiddenExportProcess(
        ffmpegPath, decodeArgs, &cancellation, {}, 256U * 1024U);
    if (decoded.cancelled || cancellation.load()) {
        result.cancelled = true;
        result.diagnostics = L"Перевірку скасовано.";
        return result;
    }
    if (!decoded.started || decoded.exitCode != 0) {
        result.diagnostics = L"Результат не пройшов повне декодування. "
            L"Цей файл може містити відкриті GOP або залежності між ключовими "
            L"кадрами, які не можна безпечно розрізати без перекодування.\r\n" +
            ProcessFailure(decoded);
        return result;
    }
    if (cancellation.load()) {
        result.cancelled = true;
        result.diagnostics = L"Операцію скасовано до фіналізації.";
        return result;
    }
    if (!SameIdentity(snapshot.sourceIdentity,
            CaptureExportFileIdentity(snapshot.sourcePath)) ||
        !SameIdentity(destAtStart, CaptureExportFileIdentity(destinationPath))) {
        result.diagnostics = L"Джерело або файл призначення змінився під час операції.";
        return result;
    }
    if (replaceSource) {
        // The owning window releases LibVLC/preview handles, rechecks identity,
        // commits this verified file atomically, then reopens the source.
        cleanup.paths.pop_back();
        result.success = true;
        return result;
    }
    if (cancellation.load()) {
        result.cancelled = true;
        result.diagnostics = L"Операцію скасовано до перезапису.";
        return result;
    }
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
        (overwriteConfirmed ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExW(PathForWin32Io(result.partialPath).c_str(),
            PathForWin32Io(destinationPath).c_str(), flags) == FALSE) {
        result.diagnostics = L"Не вдалося зберегти результат атомарно (Win32 " +
            std::to_wstring(GetLastError()) + L").";
        return result;
    }
    cleanup.paths.pop_back();
    result.partialPath.clear();
    result.success = true;
    if (progress) progress(100, L"Готово");
    return result;
}

}  // namespace videoplayer
