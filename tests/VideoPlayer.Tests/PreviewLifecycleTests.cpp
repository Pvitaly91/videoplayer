#include "PreviewLifecycleTests.h"
#include "PreviewEngine.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace videoplayer;
using namespace std::chrono_literals;

enum class Behavior {
    Ready, Timeout, Cancelled, Unsupported, RuntimeFailure, Throw,
    WaitForCancel, LateFrameAfterCancel, WaitForReady,
};

struct FakeState final {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Behavior> steps;
    std::vector<PreviewDecodeRequest> decoded;
    std::set<std::thread::id> workerThreads;
    unsigned int constructed = 0;
    unsigned int destroyed = 0;
    unsigned int activeDecodes = 0;
    unsigned int maximumDecodes = 0;
    unsigned int maximumBackends = 0;
    unsigned int cleanupCount = 0;
    bool allowReady = false;
    bool blockCleanup = false;
    bool cleanupEntered = false;
    bool allowCleanup = false;
    bool cleanupTimedOut = false;
    bool blockDestruction = false;
    bool destructionEntered = false;
    bool allowDestruction = false;
    bool destructionTimedOut = false;

    void SetSteps(std::initializer_list<Behavior> values) {
        std::lock_guard<std::mutex> lock(mutex);
        steps.assign(values);
    }

    bool WaitDecoded(const std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] { return decoded.size() >= count; });
    }

    std::size_t DecodeCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return decoded.size();
    }

    bool RemainsAtDecodeCount(const std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return !changed.wait_for(lock, 650ms, [&] { return decoded.size() != count; });
    }

    bool WaitIdleAfter(const std::size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] {
            return decoded.size() >= count && activeDecodes == 0;
        });
    }

    bool WaitSpeculationReleased() {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, 3s, [&] {
            return cleanupCount != 0 || destroyed != 0;
        });
    }
};

class FakeBackend final : public PreviewBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {
        std::lock_guard<std::mutex> lock(state_->mutex);
        ++state_->constructed;
        state_->maximumBackends = (std::max)(state_->maximumBackends,
            state_->constructed - state_->destroyed);
    }

    ~FakeBackend() override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        if (state_->blockDestruction) {
            state_->destructionEntered = true;
            state_->changed.notify_all();
            if (!state_->changed.wait_for(lock, 3s, [this] { return state_->allowDestruction; })) {
                state_->destructionTimedOut = true;
            }
            state_->blockDestruction = false;
        }
        ++state_->destroyed;
        state_->changed.notify_all();
    }

    PreviewDecodeStatus Decode(const PreviewDecodeRequest& request,
        const PreviewCancellation& cancellation, PreviewFramePtr& result) override {
        Behavior behavior = Behavior::Ready;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            state_->decoded.push_back(request);
            state_->workerThreads.insert(std::this_thread::get_id());
            ++state_->activeDecodes;
            state_->maximumDecodes = (std::max)(state_->maximumDecodes, state_->activeDecodes);
            if (!state_->steps.empty()) {
                behavior = state_->steps.front();
                state_->steps.pop_front();
            }
        }
        state_->changed.notify_all();
        struct ActiveDecode final {
            FakeState& state;
            ~ActiveDecode() {
                std::lock_guard<std::mutex> lock(state.mutex);
                --state.activeDecodes;
                state.changed.notify_all();
            }
        } active{*state_};

        if (behavior == Behavior::WaitForCancel || behavior == Behavior::LateFrameAfterCancel) {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (!cancellation.IsCancelled(request) &&
                std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(2ms);
            }
            if (behavior == Behavior::WaitForCancel) {
                return PreviewDecodeStatus::Cancelled;
            }
        }
        if (behavior == Behavior::WaitForReady) {
            std::unique_lock<std::mutex> lock(state_->mutex);
            const bool released = state_->changed.wait_for(lock, 3s, [&] {
                return state_->allowReady || cancellation.IsCancelled(request);
            });
            if (cancellation.IsCancelled(request)) return PreviewDecodeStatus::Cancelled;
            if (!released) return PreviewDecodeStatus::RetryableFailure;
        }
        switch (behavior) {
        case Behavior::Timeout: return PreviewDecodeStatus::RetryableFailure;
        case Behavior::Cancelled: return PreviewDecodeStatus::Cancelled;
        case Behavior::Unsupported: return PreviewDecodeStatus::UnsupportedMedia;
        case Behavior::RuntimeFailure: return PreviewDecodeStatus::RuntimeFailure;
        case Behavior::Throw: throw std::runtime_error("injected preview decode failure");
        default: break;
        }
        auto frame = std::make_shared<PreviewFrame>();
        frame->mediaGeneration = request.mediaGeneration;
        frame->timestampMs = request.timestampMs;
        frame->width = frame->height = 2;
        frame->pitch = 8;
        frame->pixels.assign(16, static_cast<std::uint8_t>(request.mediaGeneration));
        result = std::move(frame);
        return PreviewDecodeStatus::Ready;
    }

    void ReleaseMedia() noexcept override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        ++state_->cleanupCount;
        state_->changed.notify_all();
        if (state_->blockCleanup) {
            state_->cleanupEntered = true;
            state_->changed.notify_all();
            if (!state_->changed.wait_for(lock, 3s, [this] { return state_->allowCleanup; })) {
                state_->cleanupTimedOut = true;
            }
            state_->blockCleanup = false;
        }
    }

private:
    std::shared_ptr<FakeState> state_;
};

class Fixture final {
public:
    Fixture() : engine([state = state] { return std::make_unique<FakeBackend>(state); }) {
        window = ::CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        engine.Initialize(window);
        engine.SetMedia(L"A", 1);
    }
    ~Fixture() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->allowCleanup = true;
            state->allowDestruction = true;
        }
        state->changed.notify_all();
        engine.Shutdown();
        if (window != nullptr) {
            ::DestroyWindow(window);
        }
    }
    bool WaitFinished(const std::uint64_t request) {
        const auto deadline = std::chrono::steady_clock::now() + 4s;
        while (engine.IsRequestPending(request) && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(2ms);
        }
        return !engine.IsRequestPending(request);
    }
    bool Ready(const std::uint64_t request, const std::uint32_t generation,
        const std::int64_t timestamp) {
        if (!WaitFinished(request)) {
            return false;
        }
        const auto result = engine.TakeLatestResult();
        return result && result->frame && result->requestId == request &&
            result->mediaGeneration == generation && result->timestampMs == timestamp &&
            result->frame->pixels.front() == static_cast<std::uint8_t>(generation);
    }

    bool WaitCached(const std::int64_t timestamp) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto frame = engine.FindNearestCachedFrame(timestamp, 0);
            if (frame && frame->timestampMs == timestamp) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }

    std::shared_ptr<FakeState> state = std::make_shared<FakeState>();
    PreviewEngine engine;
    HWND window = nullptr;
};
}  // namespace

void RunPreviewLifecycleTests(const std::function<void(bool, const wchar_t*)>& check) {
    check(VerifyPreviewCaptureIsolationForTesting(),
        L"preview callbacks enforce preroll, token and retired-context isolation");
    {
        Fixture f;
        f.engine.WarmMedia(10000);
        f.engine.SetPrefetchEnabled(true);
        f.engine.WarmMedia(0);
        f.engine.WarmMedia(-1);
        check(!f.engine.HasWorker() && f.state->DecodeCount() == 0,
            L"preview eager warming requires enabled prefetch and a known positive duration");
        f.engine.WarmMedia(10000);
        check(f.WaitCached(0) && f.WaitCached(5000) && f.WaitCached(10000) &&
            f.state->DecodeCount() == 3 && !f.engine.TakeLatestResult(),
            L"preview media opening warms every coarse bucket before the first hover without publishing a hover result");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            check(!f.state->decoded.empty() && f.state->maximumDecodes == 1 && f.state->workerThreads.size() == 1 &&
                f.state->decoded.front().timestampMs == 0,
                L"preview eager warming uses one sequential worker beginning at the video start");
        }
        for (const auto timestamp : {std::int64_t{0}, std::int64_t{5000}, std::int64_t{10000}}) {
            const auto request = f.engine.RequestFrame(timestamp, 10000);
            check(!f.engine.IsRequestPending(request) && f.Ready(request, 1, timestamp) &&
                f.state->DecodeCount() == 3,
                L"preview a warmed timeline bucket is ready synchronously with no decoder work");
        }
        f.engine.WarmMedia(10000);
        f.engine.WarmMedia(10000);
        check(f.state->RemainsAtDecodeCount(3),
            L"preview repeated known-duration events do not restart completed warming");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::WaitForReady});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1), L"preview idempotent warming fixture enters its first decode");
        f.engine.WarmMedia(10000);
        f.engine.WarmMedia(10000);
        check(f.state->RemainsAtDecodeCount(1),
            L"preview repeated warm requests preserve an in-flight capture instead of cancelling it");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.WaitCached(0) && f.WaitCached(5000) && f.WaitCached(10000) &&
            f.state->DecodeCount() == 3,
            L"preview idempotent warming completes each bucket only once");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::Ready, Behavior::WaitForReady});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1), L"preview eager foreground preemption fixture starts background work");
        const auto foreground = f.engine.RequestFrame(5000, 10000);
        check(f.WaitFinished(foreground) && f.state->WaitDecoded(3) &&
            !f.engine.FindNearestCachedFrame(0, 0),
            L"preview a hover preempts eager decoding and rejects pixels arriving after cancellation");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            check(f.state->decoded.size() >= 2 && f.state->decoded[1].timestampMs == 5000,
                L"preview the foreground bucket runs ahead of all remaining eager buckets");
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.WaitCached(0) && f.WaitCached(10000) && f.Ready(foreground, 1, 5000) &&
            f.engine.GetRequestState(foreground) == PreviewRequestState::Ready &&
            f.state->DecodeCount() == 4,
            L"preview warming resumes the cancelled bucket and preserves the foreground result and request state");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::WaitForReady});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1), L"preview hidden-hover warming fixture starts background work");
        f.engine.CancelRequests();
        check(f.state->WaitDecoded(2) && !f.engine.FindNearestCachedFrame(0, 0) &&
            !f.engine.TakeLatestResult(),
            L"preview hiding a hover cancels stale pixels without abandoning the media warming plan");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            check(f.state->decoded.size() >= 2 && f.state->decoded[1].timestampMs == 0,
                L"preview hover cancellation retries the interrupted eager bucket");
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.WaitCached(0) && f.WaitCached(5000) && f.WaitCached(10000) &&
            f.state->DecodeCount() == 4 && !f.engine.TakeLatestResult(),
            L"preview background completion never recreates a hidden hover result");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::WaitForReady});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1), L"preview warm media replacement fixture starts old generation");
        f.engine.SetMedia(L"B", 2);
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(2) && !f.engine.FindNearestCachedFrame(0, 10000),
            L"preview changing media clears eager cache and rejects the old generation's late frame");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            check(f.state->decoded.size() >= 2 && f.state->decoded[1].path == L"B" &&
                f.state->decoded[1].mediaGeneration == 2,
                L"preview the replacement eager plan reads only the new media");
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.WaitCached(0) && f.WaitCached(5000) && f.WaitCached(10000) &&
            f.state->DecodeCount() == 4,
            L"preview replacement media warms a fresh complete timeline");
        const auto frame = f.engine.FindNearestCachedFrame(0, 0);
        check(frame && frame->mediaGeneration == 2 && frame->pixels.front() == 2,
            L"preview replacement timeline contains only current-generation pixels");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::LateFrameAfterCancel});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1), L"preview warm pause fixture enters background decoding");
        f.engine.SetPrefetchEnabled(false);
        check(f.state->WaitIdleAfter(1) && f.state->WaitSpeculationReleased() &&
            f.state->RemainsAtDecodeCount(1) && !f.engine.FindNearestCachedFrame(0, 0),
            L"preview disabling warming pauses its plan and rejects the interrupted frame");
        f.engine.SetPrefetchEnabled(true);
        check(f.WaitCached(0) && f.WaitCached(5000) && f.WaitCached(10000) &&
            f.state->DecodeCount() == 4,
            L"preview reenabling warming resumes the paused plan without requiring another duration event");
    }
    for (const auto failure : {Behavior::Timeout, Behavior::Cancelled}) {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({failure});
        f.engine.WarmMedia(10000);
        check(f.WaitCached(5000) && f.WaitCached(10000) && f.state->DecodeCount() == 3 &&
            !f.engine.FindNearestCachedFrame(0, 0) && !f.engine.TakeLatestResult() &&
            f.state->RemainsAtDecodeCount(3),
            L"preview eager timeout or unsolicited cancellation skips its bucket without busy retrying");
        f.engine.SetPrefetchEnabled(false);
        check(f.Ready(f.engine.RequestFrame(0, 10000), 1, 0) && f.state->DecodeCount() == 4,
            L"preview a failed eager bucket stays available for a successful explicit hover retry");
    }
    for (const auto failure : {Behavior::Unsupported, Behavior::RuntimeFailure, Behavior::Throw}) {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({failure});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1) && f.state->WaitSpeculationReleased() &&
            f.state->RemainsAtDecodeCount(1) && !f.engine.TakeLatestResult() &&
            !f.engine.FindNearestCachedFrame(0, 10000),
            L"preview unsupported or broken eager decoder stops the background plan without a retry storm");
        f.engine.SetPrefetchEnabled(false);
        check(f.Ready(f.engine.RequestFrame(0, 10000), 1, 0) && f.state->DecodeCount() == 2,
            L"preview eager media or runtime failures never poison a later explicit foreground request");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::WaitForReady});
        f.engine.WarmMedia(10000);
        check(f.state->WaitDecoded(1), L"preview changed-duration fixture begins the old plan");
        f.engine.WarmMedia(2000);
        check(f.state->WaitDecoded(2) && !f.engine.FindNearestCachedFrame(0, 0),
            L"preview a corrected duration cancels old background capture before replacing its plan");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            check(f.state->decoded.size() >= 2 && f.state->decoded[1].durationMs == 2000,
                L"preview the replacement warm plan uses the corrected duration");
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.WaitCached(0) && f.WaitCached(2000) && f.state->DecodeCount() == 3 &&
            !f.engine.FindNearestCachedFrame(5000, 0) && f.state->RemainsAtDecodeCount(3),
            L"preview corrected-duration warming includes its end and never continues obsolete buckets");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        check(!f.engine.HasWorker() && !f.engine.FindNearestCachedFrame(10000, 5000),
            L"preview enabling prefetch and looking up fallback stay lazy");
        const auto foreground = f.engine.RequestFrame(10000, 60000);
        check(f.WaitFinished(foreground) && f.WaitCached(15000) && f.WaitCached(5000),
            L"preview foreground decode warms both adjacent coarse buckets");
        check(f.Ready(foreground, 1, 10000) && f.state->DecodeCount() == 3 &&
            f.engine.GetRequestState(foreground) == PreviewRequestState::Ready,
            L"preview speculation preserves foreground result and stops after two neighbors");
        const auto cached = f.engine.RequestFrame(15000, 60000);
        const auto result = f.engine.TakeLatestResult();
        check(result && result->requestId == cached && result->timestampMs == 15000 &&
            f.state->DecodeCount() == 3 && !f.engine.IsRequestPending(cached),
            L"preview warmed neighbor is synchronous and cache hit starts no extra prefetch");
        const auto nearest = f.engine.FindNearestCachedFrame(5200, 5000);
        check(nearest && nearest->timestampMs == 5000 && nearest->mediaGeneration == 1 &&
            f.engine.GetRequestState(cached) == PreviewRequestState::Ready &&
            !f.engine.FindNearestCachedFrame(25000, 1000),
            L"preview nearest lookup keeps actual time and never changes request state");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            check(f.state->maximumDecodes == 1 && f.state->workerThreads.size() == 1 &&
                f.state->decoded[1].timestampMs == 15000 &&
                f.state->decoded[2].timestampMs == 5000,
                L"preview neighbors use the same single worker in next then previous order");
        }
        f.engine.SetMedia(L"B", 2);
        check(!f.engine.FindNearestCachedFrame(5000, 60000),
            L"preview nearest fallback cannot return a previous media generation");
    }
    for (const auto endpoint : {std::int64_t{0}, std::int64_t{12001}}) {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        const auto request = f.engine.RequestFrame(endpoint, 12001);
        const auto neighbor = endpoint == 0 ? 5000 : 10000;
        check(f.WaitCached(neighbor) && f.Ready(request, 1, endpoint) &&
            f.state->DecodeCount() == 2,
            L"preview prefetch stays within start and non-grid end boundaries");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(5000, 60000), 1, 5000) &&
            f.Ready(f.engine.RequestFrame(15000, 60000), 1, 15000),
            L"preview cached-neighbor fixture is populated without speculative decoding");
        f.engine.SetPrefetchEnabled(true);
        check(f.Ready(f.engine.RequestFrame(10000, 60000), 1, 10000) &&
            f.state->DecodeCount() == 3,
            L"preview prefetch skips already cached adjacent frames");
    }
    for (const auto failure : {Behavior::Timeout, Behavior::Unsupported,
            Behavior::RuntimeFailure, Behavior::Throw, Behavior::Cancelled}) {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::Ready, failure});
        const auto foreground = f.engine.RequestFrame(10000, 60000);
        check(f.state->WaitDecoded(2) && f.state->WaitSpeculationReleased() &&
            f.Ready(foreground, 1, 10000) && f.state->DecodeCount() == 2 &&
            f.engine.GetWorkerState() == PreviewWorkerState::Running &&
            !f.engine.FindNearestCachedFrame(15000, 0),
            L"preview speculative failure neither retries nor changes valid foreground state");
        f.engine.SetPrefetchEnabled(false);
        check(f.Ready(f.engine.RequestFrame(15000, 60000), 1, 15000) &&
            f.state->DecodeCount() == 3,
            L"preview speculative failure never poisons a later explicit request");
    }
    for (int action = 0; action < 4; ++action) {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::Ready, Behavior::LateFrameAfterCancel});
        const auto foreground = f.engine.RequestFrame(10000, 60000);
        check(f.state->WaitDecoded(2) && f.engine.GetRequestState(foreground) ==
            PreviewRequestState::Ready,
            L"preview cancellation fixture is inside a speculative neighbor decode");
        if (action == 0) f.engine.SetPrefetchEnabled(false);
        if (action == 1) f.engine.CancelRequests();
        if (action == 2) f.engine.SetMedia(L"B", 2);
        if (action == 3) f.engine.Shutdown();
        check(f.state->WaitIdleAfter(2) && f.state->WaitSpeculationReleased() &&
            !f.engine.FindNearestCachedFrame(15000, 0) && f.state->DecodeCount() == 2,
            L"preview disable cancel media reset and shutdown reject late speculative pixels");
        if (action == 0) {
            check(f.Ready(foreground, 1, 10000),
                L"preview disabling speculation does not cancel a ready foreground result");
        } else {
            check(!f.engine.TakeLatestResult() && !f.engine.IsRequestPending(foreground),
                L"preview cancellation does not allow speculation to recreate a hidden result");
        }
        f.engine.SetPrefetchEnabled(false);
        if (action != 3) {
            check(f.Ready(f.engine.RequestFrame(15000, 60000), action == 2 ? 2 : 1, 15000) &&
                f.state->DecodeCount() == 3,
                L"preview cancelled speculation never enters the RAM cache");
        } else {
            check(f.engine.GetWorkerState() == PreviewWorkerState::Stopped &&
                f.state->constructed == f.state->destroyed,
                L"preview shutdown joins speculative decode before backend destruction");
        }
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::Ready, Behavior::LateFrameAfterCancel});
        const auto first = f.engine.RequestFrame(10000, 60000);
        check(f.state->WaitDecoded(2), L"preview foreground preemption fixture warms neighbor");
        const auto replacement = f.engine.RequestFrame(35000, 60000);
        check(f.Ready(replacement, 1, 35000) && f.WaitCached(40000) && f.WaitCached(30000) &&
            f.engine.GetRequestState(first) == PreviewRequestState::Cancelled &&
            !f.engine.FindNearestCachedFrame(15000, 0) && f.state->DecodeCount() == 5,
            L"preview new foreground preempts speculation and only warms its own neighbors");
    }
    {
        Fixture f;
        f.engine.SetPrefetchEnabled(true);
        f.state->SetSteps({Behavior::WaitForReady});
        const auto foreground = f.engine.RequestFrame(10000, 60000);
        check(f.state->WaitDecoded(1), L"preview disable fixture enters foreground decode");
        f.engine.SetPrefetchEnabled(false);
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.Ready(foreground, 1, 10000) && f.state->DecodeCount() == 1,
            L"preview disabling prefetch never cancels an in-flight foreground decode");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::WaitForReady});
        const auto first = f.engine.RequestFrame(4900, 60000);
        check(f.engine.RequestFrame(5100, 60000) == first,
            L"preview nearby pending hovers reuse one request ID");
        check(f.state->WaitDecoded(1), L"preview same-bucket fixture enters decode");
        check(f.engine.RequestFrame(6200, 60000) == first &&
            f.engine.IsRequestPending(first) && f.state->DecodeCount() == 1,
            L"preview nearby in-flight hovers do not cancel the decoder");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->allowReady = true;
        }
        f.state->changed.notify_all();
        check(f.Ready(first, 1, 5000), L"preview reused in-flight capture completes normally");
        const auto cached = f.engine.RequestFrame(5200, 60000);
        const auto immediate = f.engine.TakeLatestResult();
        check(immediate && immediate->requestId == cached && immediate->frame &&
            immediate->timestampMs == 5000 && immediate->mediaGeneration == 1 &&
            !f.engine.IsRequestPending(cached) && f.state->DecodeCount() == 1,
            L"preview cached coarse bucket is available synchronously without decoding");
        f.engine.SetMedia(L"B", 2);
        const auto next = f.engine.RequestFrame(5200, 60000);
        check(next != first && f.Ready(next, 2, 5000) && f.state->DecodeCount() == 2,
            L"preview bucket reuse never crosses media generation");
    }
    {
        Fixture f;
        std::uint64_t last = 0;
        for (int index = 0; index < 20; ++index) {
            last = f.engine.RequestFrame(index * 5000, 120000);
        }
        check(f.Ready(last, 1, 95000) && f.state->DecodeCount() == 1,
            L"preview hover-settle coalesces a rapid twenty-bucket sweep to one decode");
    }
    {
        Fixture f;
        check(f.engine.IsInitialized() && f.engine.GetWorkerState() == PreviewWorkerState::NotStarted,
            L"preview stays lazy before first hover");
        f.state->SetSteps({Behavior::Timeout, Behavior::Ready});
        const auto request = f.engine.RequestFrame(5000, 30000);
        check(f.Ready(request, 1, 5000), L"preview timeout performs bounded retry then succeeds");
        check(f.state->DecodeCount() == 2, L"preview retry count is bounded to two attempts");
        f.state->SetSteps({Behavior::Timeout, Behavior::Timeout, Behavior::Ready});
        const auto failed = f.engine.RequestFrame(10000, 30000);
        check(f.WaitFinished(failed) && f.engine.GetRequestState(failed) == PreviewRequestState::RetryableFailure,
            L"preview exhausted timeout finishes request without poisoning media");
        check(!f.engine.IsRequestPending(failed), L"preview sameRequest gate permits failed hover retry");
        const auto retry = f.engine.RequestFrame(10000, 30000);
        check(f.Ready(retry, 1, 10000), L"preview next explicit same timestamp hover recovers after timeout");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::WaitForCancel, Behavior::Ready});
        const auto cancelled = f.engine.RequestFrame(5000, 30000);
        check(f.state->WaitDecoded(1), L"preview cancellation fixture entered real worker decode");
        f.engine.CancelRequests();
        check(!f.engine.IsRequestPending(cancelled), L"preview cancelled sameRequest is not pending");
        const auto repeated = f.engine.RequestFrame(5000, 30000);
        check(f.Ready(repeated, 1, 5000), L"preview cancel permits same timestamp without media reset");
        const auto count = f.state->DecodeCount();
        f.engine.CancelRequests();
        check(f.Ready(f.engine.RequestFrame(5000, 30000), 1, 5000) && f.state->DecodeCount() == count,
            L"preview closing hover retains valid RAM thumbnails");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(5000, 30000), 1, 5000), L"preview A first frame");
        f.engine.SetMedia(L"B", 2);
        check(f.Ready(f.engine.RequestFrame(10000, 60000), 2, 10000), L"preview A to B same engine");
        f.engine.SetMedia(L"A", 3);
        check(f.Ready(f.engine.RequestFrame(5000, 30000), 3, 5000), L"preview A B A refreshes generation and pixels");
        check(f.state->constructed == 1 && f.state->maximumDecodes == 1,
            L"preview media changes keep one backend and one active decode");
    }
    {
        Fixture f;
        const auto first = f.engine.RequestFrame(5000, 30000);
        check(f.WaitFinished(first), L"preview old ready event fixture");
        f.engine.SetMedia(L"B", 2);
        check(!f.engine.TakeLatestResult(), L"preview old queued ready wakeup after SetMedia has no result");
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::Ready});
        const auto old = f.engine.RequestFrame(10000, 30000);
        check(f.state->WaitDecoded(2), L"preview media replacement starts during decode");
        f.engine.SetMedia(L"A", 3);
        const auto current = f.engine.RequestFrame(15000, 30000);
        check(f.Ready(current, 3, 15000) && !f.engine.IsRequestPending(old),
            L"preview stale callback cannot publish or cache frame after media replacement");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(30000, 30000), 1, 30000), L"preview end timestamp decodes");
        check(f.Ready(f.engine.RequestFrame(0, 30000), 1, 0), L"preview end to beginning requests remain independent");
        f.state->SetSteps({Behavior::Cancelled, Behavior::Ready});
        const auto cancelled = f.engine.RequestFrame(10000, 30000);
        check(f.WaitFinished(cancelled) && f.engine.GetRequestState(cancelled) == PreviewRequestState::Cancelled,
            L"preview backend cancellation is not a media failure");
        check(f.Ready(f.engine.RequestFrame(10000, 30000), 1, 10000), L"preview backend cancelled result allows retry");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::Throw, Behavior::Ready});
        const auto failed = f.engine.RequestFrame(5000, 30000);
        check(f.WaitFinished(failed) && f.engine.GetWorkerState() == PreviewWorkerState::Faulted &&
            f.engine.GetRequestState(failed) == PreviewRequestState::RuntimeFailure && !f.engine.HasWorker(),
            L"preview backend decode exception reports Faulted instead of a live-looking request");
        check(f.Ready(f.engine.RequestFrame(5000, 30000), 1, 5000), L"preview explicit hover recreates failed backend");
        check(f.state->constructed == 2 && f.state->destroyed == 1 &&
            f.state->workerThreads.size() == 1 && f.state->maximumDecodes == 1,
            L"preview recovery reuses the same single worker thread");
    }
    {
        Fixture f;
        {
            const std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->blockDestruction = true;
        }
        f.engine.FailNextWorkerPublicationForTesting();
        const auto failed = f.engine.RequestFrame(5000, 30000);
        bool destructionBlocked = false;
        {
            std::unique_lock<std::mutex> lock(f.state->mutex);
            destructionBlocked = f.state->changed.wait_for(lock, 3s, [&] {
                return f.state->destructionEntered;
            }) && !f.state->destructionTimedOut && !f.state->allowDestruction;
        }
        check(destructionBlocked && f.engine.GetWorkerState() == PreviewWorkerState::Faulted &&
            !f.engine.HasWorker() && !f.engine.WorkerHasExitedForTesting() &&
            f.engine.GetRequestState(failed) == PreviewRequestState::RuntimeFailure,
            L"preview outer publication exception exposes Faulted while backend teardown is blocked");
        check(!f.engine.TakeLatestResult(),
            L"preview outer publication failure leaves no ready result or fake success");

        const auto retryStarted = std::chrono::steady_clock::now();
        const auto rejected = f.engine.RequestFrame(5000, 30000);
        const auto retryElapsed = std::chrono::steady_clock::now() - retryStarted;
        check(retryElapsed < 250ms && !f.engine.IsRequestPending(rejected) &&
            f.engine.GetRequestState(rejected) == PreviewRequestState::RuntimeFailure &&
            f.engine.WorkerLaunchCountForTesting() == 1,
            L"preview hover during outer worker exit returns without joining cleanup or launching another thread");
        {
            const std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->allowDestruction = true;
        }
        f.state->changed.notify_all();
        const auto exitDeadline = std::chrono::steady_clock::now() + 3s;
        while (!f.engine.WorkerHasExitedForTesting() &&
            std::chrono::steady_clock::now() < exitDeadline) {
            std::this_thread::sleep_for(2ms);
        }
        check(f.engine.WorkerHasExitedForTesting(),
            L"preview outer worker records actual exit after resource destruction");
        const auto resumed = f.engine.RequestFrame(5000, 30000);
        check(f.Ready(resumed, 1, 5000),
            L"preview explicit hover rejoins exited worker and recovers same timestamp");
        const unsigned int launches = f.engine.WorkerLaunchCountForTesting();
        {
            const std::lock_guard<std::mutex> lock(f.state->mutex);
            check(!f.state->destructionTimedOut && f.state->constructed == 2 &&
                f.state->destroyed == 1 && f.state->maximumBackends == 1 &&
                f.state->maximumDecodes == 1 && launches == 2,
                L"preview outer recovery starts exactly one replacement after the first backend is gone");
        }
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::Unsupported, Behavior::Ready});
        const auto unsupported = f.engine.RequestFrame(5000, 30000);
        check(f.WaitFinished(unsupported) && f.engine.GetRequestState(unsupported) == PreviewRequestState::UnsupportedMedia,
            L"preview confirmed unsupported differs from timeout");
        const auto rejected = f.engine.RequestFrame(10000, 30000);
        check(f.engine.GetRequestState(rejected) == PreviewRequestState::UnsupportedMedia &&
            f.state->DecodeCount() == 1, L"preview unsupported uses session-only negative cache");
        f.engine.SetMedia(L"B", 2);
        check(f.Ready(f.engine.RequestFrame(5000, 30000), 2, 5000), L"preview other media clears unsupported state");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(5000, 30000), 1, 5000), L"preview slow cleanup fixture initialized");
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->blockCleanup = true;
        }
        const auto started = std::chrono::steady_clock::now();
        f.engine.SetMedia(L"", 2);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        {
            std::unique_lock<std::mutex> lock(f.state->mutex);
            const bool entered = f.state->changed.wait_for(lock, 3s, [&] { return f.state->cleanupEntered; });
            check(entered && !f.state->allowCleanup && !f.state->cleanupTimedOut && elapsed < 250ms,
                L"preview SetMedia returns while old media cleanup is still blocked on worker");
        }
        f.engine.SetMedia(L"B", 3);
        const auto request = f.engine.RequestFrame(10000, 30000);
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->allowCleanup = true;
        }
        f.state->changed.notify_all();
        check(f.Ready(request, 3, 10000), L"preview newest media decodes after old cleanup safely finishes");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::Ready});
        f.engine.RequestFrame(5000, 30000);
        check(f.state->WaitDecoded(1), L"preview coalescing fixture active");
        f.engine.RequestFrame(10000, 30000);
        const auto newest = f.engine.RequestFrame(15000, 30000);
        check(f.Ready(newest, 1, 15000), L"preview coalesces rapid hover into current result");
        check(f.state->maximumDecodes == 1, L"preview rapid hover never starts concurrent decoders");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::WaitForCancel});
        f.engine.RequestFrame(5000, 30000);
        check(f.state->WaitDecoded(1), L"preview shutdown fixture inside decode");
        f.engine.Shutdown();
        check(f.engine.GetWorkerState() == PreviewWorkerState::Stopped && !f.engine.IsInitialized() &&
            f.state->activeDecodes == 0 && f.state->constructed == f.state->destroyed &&
            !f.engine.TakeLatestResult(), L"preview shutdown cooperatively joins before destroying resources");
    }
}
