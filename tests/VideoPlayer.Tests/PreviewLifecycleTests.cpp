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
    WaitForCancel, LateFrameAfterCancel,
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
        check(f.engine.IsInitialized() && f.engine.GetWorkerState() == PreviewWorkerState::NotStarted,
            L"preview stays lazy before first hover");
        f.state->SetSteps({Behavior::Timeout, Behavior::Ready});
        const auto request = f.engine.RequestFrame(1500, 10000);
        check(f.Ready(request, 1, 1500), L"preview timeout performs bounded retry then succeeds");
        check(f.state->DecodeCount() == 2, L"preview retry count is bounded to two attempts");
        f.state->SetSteps({Behavior::Timeout, Behavior::Timeout, Behavior::Ready});
        const auto failed = f.engine.RequestFrame(2250, 10000);
        check(f.WaitFinished(failed) && f.engine.GetRequestState(failed) == PreviewRequestState::RetryableFailure,
            L"preview exhausted timeout finishes request without poisoning media");
        check(!f.engine.IsRequestPending(failed), L"preview sameRequest gate permits failed hover retry");
        const auto retry = f.engine.RequestFrame(2250, 10000);
        check(f.Ready(retry, 1, 2250), L"preview next explicit same timestamp hover recovers after timeout");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::WaitForCancel, Behavior::Ready});
        const auto cancelled = f.engine.RequestFrame(1500, 10000);
        check(f.state->WaitDecoded(1), L"preview cancellation fixture entered real worker decode");
        f.engine.CancelRequests();
        check(!f.engine.IsRequestPending(cancelled), L"preview cancelled sameRequest is not pending");
        const auto repeated = f.engine.RequestFrame(1500, 10000);
        check(f.Ready(repeated, 1, 1500), L"preview cancel permits same timestamp without media reset");
        const auto count = f.state->DecodeCount();
        f.engine.CancelRequests();
        check(f.Ready(f.engine.RequestFrame(1500, 10000), 1, 1500) && f.state->DecodeCount() == count,
            L"preview closing hover retains valid RAM thumbnails");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(1500, 10000), 1, 1500), L"preview A first frame");
        f.engine.SetMedia(L"B", 2);
        check(f.Ready(f.engine.RequestFrame(2250, 20000), 2, 2250), L"preview A to B same engine");
        f.engine.SetMedia(L"A", 3);
        check(f.Ready(f.engine.RequestFrame(1500, 10000), 3, 1500), L"preview A B A refreshes generation and pixels");
        check(f.state->constructed == 1 && f.state->maximumDecodes == 1,
            L"preview media changes keep one backend and one active decode");
    }
    {
        Fixture f;
        const auto first = f.engine.RequestFrame(1500, 10000);
        check(f.WaitFinished(first), L"preview old ready event fixture");
        f.engine.SetMedia(L"B", 2);
        check(!f.engine.TakeLatestResult(), L"preview old queued ready wakeup after SetMedia has no result");
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::Ready});
        const auto old = f.engine.RequestFrame(2250, 10000);
        check(f.state->WaitDecoded(2), L"preview media replacement starts during decode");
        f.engine.SetMedia(L"A", 3);
        const auto current = f.engine.RequestFrame(3000, 10000);
        check(f.Ready(current, 3, 3000) && !f.engine.IsRequestPending(old),
            L"preview stale callback cannot publish or cache frame after media replacement");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(10000, 10000), 1, 10000), L"preview end timestamp decodes");
        check(f.Ready(f.engine.RequestFrame(0, 10000), 1, 0), L"preview end to beginning requests remain independent");
        f.state->SetSteps({Behavior::Cancelled, Behavior::Ready});
        const auto cancelled = f.engine.RequestFrame(2250, 10000);
        check(f.WaitFinished(cancelled) && f.engine.GetRequestState(cancelled) == PreviewRequestState::Cancelled,
            L"preview backend cancellation is not a media failure");
        check(f.Ready(f.engine.RequestFrame(2250, 10000), 1, 2250), L"preview backend cancelled result allows retry");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::Throw, Behavior::Ready});
        const auto failed = f.engine.RequestFrame(1500, 10000);
        check(f.WaitFinished(failed) && f.engine.GetWorkerState() == PreviewWorkerState::Faulted &&
            f.engine.GetRequestState(failed) == PreviewRequestState::RuntimeFailure && !f.engine.HasWorker(),
            L"preview backend decode exception reports Faulted instead of a live-looking request");
        check(f.Ready(f.engine.RequestFrame(1500, 10000), 1, 1500), L"preview explicit hover recreates failed backend");
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
        const auto failed = f.engine.RequestFrame(1500, 10000);
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
        const auto rejected = f.engine.RequestFrame(1500, 10000);
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
        const auto resumed = f.engine.RequestFrame(1500, 10000);
        check(f.Ready(resumed, 1, 1500),
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
        const auto unsupported = f.engine.RequestFrame(1500, 10000);
        check(f.WaitFinished(unsupported) && f.engine.GetRequestState(unsupported) == PreviewRequestState::UnsupportedMedia,
            L"preview confirmed unsupported differs from timeout");
        const auto rejected = f.engine.RequestFrame(2250, 10000);
        check(f.engine.GetRequestState(rejected) == PreviewRequestState::UnsupportedMedia &&
            f.state->DecodeCount() == 1, L"preview unsupported uses session-only negative cache");
        f.engine.SetMedia(L"B", 2);
        check(f.Ready(f.engine.RequestFrame(1500, 10000), 2, 1500), L"preview other media clears unsupported state");
    }
    {
        Fixture f;
        check(f.Ready(f.engine.RequestFrame(1500, 10000), 1, 1500), L"preview slow cleanup fixture initialized");
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
        const auto request = f.engine.RequestFrame(2250, 10000);
        {
            std::lock_guard<std::mutex> lock(f.state->mutex);
            f.state->allowCleanup = true;
        }
        f.state->changed.notify_all();
        check(f.Ready(request, 3, 2250), L"preview newest media decodes after old cleanup safely finishes");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::LateFrameAfterCancel, Behavior::Ready});
        f.engine.RequestFrame(1500, 10000);
        check(f.state->WaitDecoded(1), L"preview coalescing fixture active");
        f.engine.RequestFrame(2250, 10000);
        const auto newest = f.engine.RequestFrame(3000, 10000);
        check(f.Ready(newest, 1, 3000), L"preview coalesces rapid hover into current result");
        check(f.state->maximumDecodes == 1, L"preview rapid hover never starts concurrent decoders");
    }
    {
        Fixture f;
        f.state->SetSteps({Behavior::WaitForCancel});
        f.engine.RequestFrame(1500, 10000);
        check(f.state->WaitDecoded(1), L"preview shutdown fixture inside decode");
        f.engine.Shutdown();
        check(f.engine.GetWorkerState() == PreviewWorkerState::Stopped && !f.engine.IsInitialized() &&
            f.state->activeDecodes == 0 && f.state->constructed == f.state->destroyed &&
            !f.engine.TakeLatestResult(), L"preview shutdown cooperatively joins before destroying resources");
    }
}
