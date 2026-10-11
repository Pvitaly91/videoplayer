#include "ExportPlanner.h"

#include "PathUtils.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <iomanip>
#include <locale>
#include <limits>
#include <sstream>

namespace videoplayer {
namespace {

std::wstring LowerExtension(const std::wstring& path) {
    const auto dot = path.find_last_of(L'.');
    std::wstring extension = dot == std::wstring::npos ? L"" : path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](const wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return extension;
}

std::wstring EncoderForCodec(const std::wstring& codec) {
    if (codec == L"h264") return L"libx264";
    if (codec == L"hevc" || codec == L"h265") return L"libx265";
    if (codec == L"mpeg4") return L"mpeg4";
    if (codec == L"vp9") return L"libvpx-vp9";
    if (codec == L"av1") return L"libaom-av1";
    return {};
}

bool IsHdr(const MediaStreamInfo& video) {
    return video.bitDepth > 8 || video.colorTransfer == L"smpte2084" ||
        video.colorTransfer == L"arib-std-b67";
}

bool ContainerAcceptsCodec(
    const std::wstring& muxer, const std::wstring& type, const std::wstring& codec) {
    if (muxer == L"matroska") return true;
    if (type == L"video") {
        if (muxer == L"webm") {
            return codec == L"vp8" || codec == L"vp9" || codec == L"av1";
        }
        return codec == L"h264" || codec == L"hevc" || codec == L"mpeg4" ||
            codec == L"av1" || codec == L"vp9" || codec == L"mjpeg";
    }
    if (type == L"audio") {
        if (muxer == L"webm") return codec == L"opus" || codec == L"vorbis";
        return codec == L"aac" || codec == L"mp3" || codec == L"alac" ||
            codec == L"ac3" || codec == L"eac3" || codec == L"opus";
    }
    return false;
}

void Add(std::vector<std::wstring>& args,
    const std::wstring& name, const std::wstring& value) {
    args.push_back(name);
    args.push_back(value);
}

}  // namespace

std::wstring FormatExportSeconds(const std::int64_t milliseconds) {
    std::wostringstream value;
    value.imbue(std::locale::classic());
    value << std::fixed << std::setprecision(3)
          << static_cast<double>(milliseconds) / 1000.0;
    return value.str();
}

std::wstring ExportMuxerForPath(const std::wstring& path) {
    const std::wstring extension = LowerExtension(path);
    if (extension == L".mkv") return L"matroska";
    if (extension == L".webm") return L"webm";
    if (extension == L".mp4" || extension == L".m4v" || extension == L".mov") {
        return L"mp4";
    }
    return {};
}

ExportPlan BuildExportPlan(
    const ExportSnapshot& snapshot,
    const MediaProbeResult& probe,
    const ExportOptions& options,
    const ExportSourceRegion& visibleRegion,
    const std::vector<std::int64_t>& keyframes) {
    ExportPlan plan{};
    const MediaStreamInfo* video = probe.MainVideo();
    if (!probe.valid || video == nullptr) {
        plan.error = probe.error.empty() ? L"FFprobe не знайшов основний відеопотік." : probe.error;
        return plan;
    }
    if (snapshot.sourcePath.empty() || options.destinationPath.empty()) {
        plan.error = L"Не вказано шлях джерела або результату.";
        return plan;
    }
    plan.muxer = ExportMuxerForPath(options.destinationPath);
    if (plan.muxer.empty()) {
        plan.error = L"Підтримуються контейнери MP4, MKV і WebM.";
        return plan;
    }
    plan.outputExtension = LowerExtension(options.destinationPath);
    plan.startMs = options.fragment == ExportFragment::Segment
        ? snapshot.segmentStartMs : 0;
    plan.endMs = options.fragment == ExportFragment::Segment
        ? snapshot.segmentEndMs : snapshot.durationMs;
    if (plan.endMs <= plan.startMs || plan.startMs < 0) {
        plan.error = L"Недійсний інтервал експорту A–B.";
        return plan;
    }

    const bool crop = options.image == ExportImage::VisibleZoom;
    if (crop && !visibleRegion.valid) {
        plan.error = L"Не вдалося визначити видиму область зуму.";
        return plan;
    }
    if (crop) plan.effectiveCrop = visibleRegion.aligned;
    plan.outputWidth = crop && options.resolution == ExportResolution::NativeCrop
        ? plan.effectiveCrop.width : video->width;
    plan.outputHeight = crop && options.resolution == ExportResolution::NativeCrop
        ? plan.effectiveCrop.height : video->height;
    plan.outputHasAudio = options.audio == ExportAudio::Original && probe.HasAudio();
    plan.expectedVideoCodec = video->codecName;
    plan.expectedPixelFormat = video->pixelFormat;
    plan.expectedBitDepth = video->bitDepth;
    plan.expectedSampleAspectRatio = video->sampleAspectRatio;
    plan.expectedRotationDegrees = video->rotationDegrees;
    if (plan.outputHasAudio) {
        for (const auto& stream : probe.streams) {
            if (stream.codecType == L"audio") plan.expectedAudioCodecs.push_back(stream.codecName);
        }
    }
    const bool segment = options.fragment == ExportFragment::Segment;
    // A full frame never needs encoding, including an A-B selection.
    plan.streamCopy = !crop;
    if (crop && options.method == ExportMethod::FastStreamCopy) {
        plan.error = L"Зум змінює пікселі та потребує перекодування. Для копіювання виберіть повний кадр.";
        return plan;
    }
    if (segment && plan.streamCopy) {
        if (!std::isfinite(probe.durationSeconds) || probe.durationSeconds <= 0.0 ||
            probe.durationSeconds >= static_cast<double>((std::numeric_limits<std::int64_t>::max)()) / 1000.0) {
            plan.error = L"Некоректна тривалість для копіювання A–B.";
            return plan;
        }
        const auto duration = static_cast<std::int64_t>(std::llround(probe.durationSeconds * 1000.0));
        if (plan.startMs >= duration || plan.endMs - (std::min)(plan.endMs, duration) > 500) {
            plan.error = L"Інтервал A–B виходить за межі відео.";
            return plan;
        }
        std::vector<std::int64_t> boundaries;
        for (const auto keyframe : keyframes) {
            if (keyframe >= 0 && keyframe < duration) boundaries.push_back(keyframe);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        const auto left = std::upper_bound(boundaries.begin(), boundaries.end(), plan.startMs);
        if (left == boundaries.begin()) {
            plan.error = L"Не знайдено ключового кадру для початку A–B. Перекодування не виконується.";
            return plan;
        }
        const auto right = std::lower_bound(boundaries.begin(), boundaries.end(), plan.endMs);
        plan.startMs = *std::prev(left);
        plan.endMs = right == boundaries.end() ? duration : *right;
        const long double seek = static_cast<long double>(plan.startMs) +
            (static_cast<long double>(video->startSeconds) - probe.startSeconds) * 1000.0L;
        if (!std::isfinite(seek) || seek >= (std::numeric_limits<std::int64_t>::max)() ||
            seek <= (std::numeric_limits<std::int64_t>::min)()) {
            plan.error = L"Некоректні часові мітки для копіювання A–B.";
            return plan;
        }
        plan.copySeekMs = (std::max)(std::int64_t{0}, static_cast<std::int64_t>(std::llround(seek)));
        plan.keyframeAligned = true;
    }
    const std::wstring sourceFamilyEncoder = EncoderForCodec(video->codecName);
    const bool preservesVideoCodec = plan.streamCopy ||
        (!sourceFamilyEncoder.empty() && probe.HasEncoder(sourceFamilyEncoder));
    if (preservesVideoCodec &&
        !ContainerAcceptsCodec(plan.muxer, L"video", video->codecName)) {
        plan.error = L"Вибраний контейнер несумісний з codec family відео. Виберіть MKV або інший сумісний контейнер.";
        return plan;
    }
    if (plan.outputHasAudio && (plan.streamCopy || !segment)) {
        for (const auto& stream : probe.streams) {
            if (stream.codecType == L"audio" &&
                !ContainerAcceptsCodec(plan.muxer, L"audio", stream.codecName)) {
                plan.error = L"Контейнер не підтримує оригінальне аудіо. Виберіть MKV або «Без аудіо»; перекодування не виконується.";
                return plan;
            }
        }
    }

    auto& args = plan.ffmpegArguments;
    args = {L"-hide_banner", L"-nostats", L"-loglevel", L"warning",
        L"-progress", L"pipe:1", L"-y"};
    if (crop) args.push_back(L"-noautorotate");
    if (plan.keyframeAligned && plan.copySeekMs > 0) {
        Add(args, L"-ss", FormatExportSeconds(plan.copySeekMs));
    }
    Add(args, L"-i", PathForWin32Io(snapshot.sourcePath));
    if (segment) {
        if (!plan.keyframeAligned) Add(args, L"-ss", FormatExportSeconds(plan.startMs));
        Add(args, L"-t", FormatExportSeconds(plan.endMs - plan.startMs));
    }
    Add(args, L"-map", L"0:" + std::to_wstring(video->index));
    if (plan.outputHasAudio) {
        args.insert(args.end(), {L"-map", L"0:a?"});
    } else {
        args.push_back(L"-an");
    }
    args.insert(args.end(), {L"-sn", L"-dn", L"-map_chapters", L"-1"});

    if (plan.streamCopy) {
        args.insert(args.end(), {L"-c", L"copy"});
        if (plan.keyframeAligned) {
            const auto length = FormatExportSeconds(plan.endMs - plan.startMs);
            // Same packet-preserving bounds as lossless interval deletion.
            Add(args, L"-bsf:v", L"noise=amount=0:drop=lt(pts*tb\\,-0.0005)+gte(pts*tb\\," + length + L"-0.0005)");
            if (plan.outputHasAudio) {
                Add(args, L"-bsf:a", L"noise=amount=0:drop=lt(pts*tb\\,0)+gte(pts*tb\\," + length + L")");
            }
            Add(args, L"-avoid_negative_ts", L"disabled");
        }
    } else {
        plan.videoEncoder = EncoderForCodec(video->codecName);
        if (!plan.videoEncoder.empty() && !probe.HasEncoder(plan.videoEncoder)) {
            plan.videoEncoder.clear();
        }
        bool codecFallback = false;
        if (plan.videoEncoder.empty()) {
            if (!probe.HasEncoder(L"libx264") ||
                (plan.outputHasAudio && segment && !probe.HasEncoder(L"aac"))) {
                plan.error = L"Bundled FFmpeg не має encoder codec family джерела або H.264/AAC fallback.";
                return plan;
            }
            if (!options.confirmedCodecFallback) {
                if (plan.muxer != L"mp4") {
                    plan.error = L"Codec-family encoder недоступний. Для явного H.264/AAC fallback спочатку виберіть файл MP4.";
                    return plan;
                }
                plan.requiresCodecFallbackConfirmation = true;
                plan.error = L"Кодек джерела не має відповідного encoder у portable FFmpeg. Потрібне явне підтвердження H.264/AAC MP4.";
                return plan;
            }
            if (plan.muxer != L"mp4") {
                plan.error = L"Резервне кодування H.264/AAC підтримується лише для MP4.";
                return plan;
            }
            plan.videoEncoder = L"libx264";
            codecFallback = true;
            plan.expectedVideoCodec = L"h264";
            plan.expectedPixelFormat = L"yuv420p";
            plan.expectedBitDepth = 8;
            plan.warning = L"Відео буде перекодовано в H.264 після явного підтвердження.";
        }
        if (codecFallback && IsHdr(*video)) {
            plan.error = L"Експорт зупинено: HDR/10-bit не можна непомітно перетворювати на 8-bit SDR.";
            return plan;
        }
        if (!video->fieldOrder.empty() && video->fieldOrder != L"progressive" &&
            video->fieldOrder != L"unknown") {
            plan.error = L"Interlaced transcode зупинено: exporter не виконує непомітний deinterlace.";
            return plan;
        }
        if (crop) {
            std::wstring filter = L"crop=" + std::to_wstring(plan.effectiveCrop.width) +
                L":" + std::to_wstring(plan.effectiveCrop.height) + L":" +
                std::to_wstring(plan.effectiveCrop.x) + L":" +
                std::to_wstring(plan.effectiveCrop.y);
            if (options.resolution == ExportResolution::SourceCanvas) {
                filter += L",scale=" + std::to_wstring(video->width) + L":" +
                    std::to_wstring(video->height) + L":force_original_aspect_ratio=decrease";
                filter += L",pad=" + std::to_wstring(video->width) + L":" +
                    std::to_wstring(video->height) + L":(ow-iw)/2:(oh-ih)/2:black";
            }
            Add(args, L"-vf", filter);
        }
        Add(args, L"-c:v", plan.videoEncoder);
        if (video->bitRate > 0) {
            plan.targetVideoBitRate = video->bitRate;
            if (crop && options.resolution == ExportResolution::NativeCrop &&
                video->width != 0 && video->height != 0) {
                const long double sourcePixels = static_cast<long double>(video->width) *
                    static_cast<long double>(video->height);
                const long double outputPixels = static_cast<long double>(plan.outputWidth) *
                    static_cast<long double>(plan.outputHeight);
                const auto proportional = static_cast<std::int64_t>(
                    static_cast<long double>(video->bitRate) * outputPixels / sourcePixels + 0.5L);
                plan.targetVideoBitRate = (std::min)(video->bitRate,
                    (std::max<std::int64_t>)(16000, proportional));
            }
            Add(args, L"-b:v", std::to_wstring(plan.targetVideoBitRate));
        }
        if (!codecFallback && !video->pixelFormat.empty()) {
            Add(args, L"-pix_fmt", video->pixelFormat);
        } else if (codecFallback) {
            Add(args, L"-pix_fmt", L"yuv420p");
        }
        if (!video->colorPrimaries.empty()) Add(args, L"-color_primaries", video->colorPrimaries);
        if (!video->colorTransfer.empty()) Add(args, L"-color_trc", video->colorTransfer);
        if (!video->colorSpace.empty()) Add(args, L"-colorspace", video->colorSpace);
        args.insert(args.end(), {L"-map_metadata", L"0"});
        if (video->rotationDegrees != 0) {
            Add(args, L"-metadata:s:v:0", L"rotate=" + std::to_wstring(video->rotationDegrees));
        }
        if (plan.outputHasAudio) {
            if (segment) {
                const std::wstring audioEncoder = plan.muxer == L"webm" ? L"libopus" : L"aac";
                if (!probe.HasEncoder(audioEncoder)) {
                    plan.error = L"Bundled FFmpeg не має потрібного audio encoder: " + audioEncoder;
                    return plan;
                }
                Add(args, L"-c:a", audioEncoder);
                const std::wstring outputAudioCodec = audioEncoder == L"libopus" ? L"opus" : L"aac";
                for (auto& codec : plan.expectedAudioCodecs) codec = outputAudioCodec;
            } else {
                Add(args, L"-c:a", L"copy");
            }
        }
    }
    if (plan.muxer == L"mp4") args.insert(args.end(), {L"-movflags", L"+faststart"});
    Add(args, L"-f", plan.muxer);

    std::wostringstream summary;
    summary << (segment ? L"Фрагмент A–B: " : L"Повне відео: ")
        << FormatExportSeconds(plan.startMs) << L"–" << FormatExportSeconds(plan.endMs)
        << L" с\r\nЗображення: ";
    if (crop) {
        summary << L"видимий зум " << plan.effectiveCrop.width << L"×"
            << plan.effectiveCrop.height << L" @ " << plan.effectiveCrop.x << L","
            << plan.effectiveCrop.y << L"\r\nЗапитана source-область: "
            << std::fixed << std::setprecision(3) << visibleRegion.x << L","
            << visibleRegion.y << L" " << visibleRegion.width << L"×"
            << visibleRegion.height << L"; фактична вирівняна: "
            << plan.effectiveCrop.x << L"," << plan.effectiveCrop.y << L" "
            << plan.effectiveCrop.width << L"×" << plan.effectiveCrop.height;
    } else {
        summary << L"повний кадр";
    }
    summary << L"\r\nРозмір кадру результату: " << plan.outputWidth << L"×"
        << plan.outputHeight;
    if (crop && options.resolution == ExportResolution::NativeCrop) {
        summary << L" (фізичний crop, без доданого полотна)";
    } else if (crop) {
        summary << L" (полотно джерела з black padding)";
    }
    summary << L"\r\nВідео: " << (plan.streamCopy ? L"без перекодування" : plan.videoEncoder);
    if (plan.targetVideoBitRate > 0) {
        summary << L", цільовий bitrate " << plan.targetVideoBitRate / 1000 << L" кбіт/с";
    }
    summary
        << L"\r\nАудіо: " << (options.audio == ExportAudio::None ? L"вилучено" :
            !plan.outputHasAudio ? L"відсутнє у джерелі" :
            segment && !plan.streamCopy ? (plan.muxer == L"webm" ? L"перекодовано Opus для точного A–B" :
                L"перекодовано AAC для точного A–B") : L"оригінальний потік без перекодування")
        << L"\r\nДодаткові відеопотоки, субтитри, вкладення та розділи: не експортуються.";
    if (plan.keyframeAligned) {
        summary << L"\r\nЗапитаний A–B: " << FormatExportSeconds(snapshot.segmentStartMs)
            << L"–" << FormatExportSeconds(snapshot.segmentEndMs) << L" с."
            << L"\r\nМежі розширюються до ключових кадрів для безпечного копіювання."
            << L"\r\nФактично буде збережено: " << FormatExportSeconds(plan.startMs)
            << L"–" << FormatExportSeconds(plan.endMs) << L" с; без втрати якості.";
    }
    plan.summary = summary.str();
    plan.valid = true;
    return plan;
}

}  // namespace videoplayer
