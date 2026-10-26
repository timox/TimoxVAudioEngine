
#include "software_clock_host.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <system_error>

namespace {
DWORD LastClockError() noexcept {
    const auto error = GetLastError();
    return error != ERROR_SUCCESS ? error : ERROR_GEN_FAILURE;
}
}

SoftwareClockDeadline::SoftwareClockDeadline(std::int64_t origin, std::int64_t frequency,
    double rate, std::uint32_t frames) noexcept
    : origin_(origin), ticksPerBlockNumerator_(static_cast<long double>(frequency) * frames), sampleRate_(rate) {}
std::int64_t SoftwareClockDeadline::deadline() const noexcept {
    return origin_ + static_cast<std::int64_t>(std::ceil(ticksPerBlockNumerator_ * index_ / sampleRate_));
}
std::uint64_t SoftwareClockDeadline::advancePast(std::int64_t now) noexcept {
    const auto previous = index_++;
    if (now > origin_) {
        const auto next = static_cast<std::uint64_t>(std::floor(
            static_cast<long double>(now - origin_) * sampleRate_ / ticksPerBlockNumerator_)) + 1;
        index_ = (std::max)(index_, next);
    }
    if (deadline() <= now) ++index_;
    return index_ - previous - 1;
}

struct SoftwareClockHost::Session {
    double rate = 48000;
    std::uint32_t frames = 256;
    HANDLE stopEvent = nullptr, readyEvent = nullptr, timer = nullptr;
    std::thread worker;
    AudioClockProcessCallback callback = nullptr;
    void* context = nullptr;
    std::int64_t frequency = 0;
    std::atomic<DWORD> initializationError{ERROR_SUCCESS};
    std::atomic<DWORD> runtimeError{ERROR_SUCCESS};
    std::atomic<std::uint64_t> callbacks{0}, delayedStarts{0}, overruns{0}, worstNs{0};
    ~Session() { releaseHandles(); }
    void releaseHandles() noexcept {
        if (timer) CloseHandle(timer);
        if (stopEvent) CloseHandle(stopEvent);
        if (readyEvent) CloseHandle(readyEvent);
        timer = stopEvent = readyEvent = nullptr;
    }
    bool arm(std::int64_t deadline) noexcept {
        LARGE_INTEGER now{}, due{};
        QueryPerformanceCounter(&now);
        // Relative OS wait recomputed from absolute QPC deadline: no accumulated drift.
        due.QuadPart = -(std::max)(1LL, static_cast<long long>(std::ceil(
            static_cast<long double>(deadline - now.QuadPart) * 10000000 / frequency)));
        return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
    }
    void run() noexcept {
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (!mmcss) {
            initializationError.store(LastClockError(), std::memory_order_release);
            SetEvent(readyEvent); return;
        }
        if (!AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH)) {
            initializationError.store(LastClockError(), std::memory_order_release);
            SetEvent(readyEvent); AvRevertMmThreadCharacteristics(mmcss); return;
        }
        LARGE_INTEGER epoch{};
        QueryPerformanceCounter(&epoch);
        SoftwareClockDeadline schedule(epoch.QuadPart, frequency, rate, frames);
        if (!arm(schedule.deadline())) {
            initializationError.store(LastClockError(), std::memory_order_release);
            SetEvent(readyEvent); AvRevertMmThreadCharacteristics(mmcss); return;
        }
        SetEvent(readyEvent);
        const HANDLE waits[] = {stopEvent, timer};
        const long double period = static_cast<long double>(frequency) * frames / rate;
        for (;;) {
            const auto wake = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wake == WAIT_OBJECT_0) break;
            if (wake != WAIT_OBJECT_0 + 1) {
                runtimeError.store(wake == WAIT_FAILED ? LastClockError() : ERROR_GEN_FAILURE,
                    std::memory_order_release);
                break;
            }
            LARGE_INTEGER begin{}, end{};
            QueryPerformanceCounter(&begin);
            if (begin.QuadPart < schedule.deadline()) {
                if (!arm(schedule.deadline())) {
                    runtimeError.store(LastClockError(), std::memory_order_release);
                    break;
                }
                continue;
            }
            const auto stopStatus = WaitForSingleObject(stopEvent, 0);
            if (stopStatus == WAIT_OBJECT_0) break;
            if (stopStatus != WAIT_TIMEOUT) {
                runtimeError.store(stopStatus == WAIT_FAILED ? LastClockError() : ERROR_GEN_FAILURE,
                    std::memory_order_release);
                break;
            }
            // Whole-quantum delays, excluding ordinary sub-quantum timer jitter.
            if (begin.QuadPart - schedule.deadline() >= period)
                delayedStarts.fetch_add(1, std::memory_order_relaxed);
            callback(nullptr, nullptr, frames, context);
            QueryPerformanceCounter(&end);
            callbacks.fetch_add(1, std::memory_order_relaxed);
            const auto elapsed = end.QuadPart - begin.QuadPart;
            if (elapsed >= period) overruns.fetch_add(1, std::memory_order_relaxed);
            const auto ns = static_cast<std::uint64_t>(static_cast<long double>(elapsed) * 1000000000 / frequency);
            auto worst = worstNs.load(std::memory_order_relaxed);
            while (ns > worst && !worstNs.compare_exchange_weak(worst, ns, std::memory_order_relaxed)) {}
            schedule.advancePast(end.QuadPart);
            if (!arm(schedule.deadline())) {
                runtimeError.store(LastClockError(), std::memory_order_release);
                break;
            }
        }
        AvRevertMmThreadCharacteristics(mmcss);
    }
};

SoftwareClockHost::SoftwareClockHost() = default;
SoftwareClockHost::~SoftwareClockHost() { close(); }
std::vector<AudioClockDeviceInfo> SoftwareClockHost::enumerate() const { return {}; }
bool SoftwareClockHost::open(const std::wstring& id, void*, AudioClockCapabilities& caps, std::string& error) {
    if (id != L"software") { error = "Invalid software clock identifier"; return false; }
    close(); session_ = std::make_unique<Session>();
    return refreshCapabilities(caps, error);
}
bool SoftwareClockHost::setSampleRate(double rate, std::string& error) {
    if (!session_) { error = "Software clock is closed"; return false; }
    if (session_->worker.joinable()) { error = "Stop software clock before changing sample rate"; return false; }
    if (!std::isfinite(rate) || rate < 8000 || rate > 384000) {
        error = "Software sample rate must be finite and between 8000 and 384000 Hz"; return false;
    }
    session_->rate = rate; error.clear(); return true;
}
bool SoftwareClockHost::refreshCapabilities(AudioClockCapabilities& caps, std::string& error) {
    if (!session_) { error = "Software clock is closed"; return false; }
    caps = {};
    caps.minBufferFrames = 16; caps.maxBufferFrames = 8192;
    caps.preferredBufferFrames = 256; caps.bufferGranularity = 1;
    caps.currentSampleRate = session_->rate;
    caps.supportedSampleRates = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};
    if (std::find(caps.supportedSampleRates.begin(), caps.supportedSampleRates.end(), session_->rate) == caps.supportedSampleRates.end())
        caps.supportedSampleRates.push_back(session_->rate);
    error.clear(); return true;
}
bool SoftwareClockHost::consumeSampleRateChange() noexcept { return false; }
std::uint32_t SoftwareClockHost::consumeErrorCode() noexcept {
    return session_ ? session_->runtimeError.exchange(ERROR_SUCCESS, std::memory_order_acq_rel) : ERROR_SUCCESS;
}
AudioClockTimingCounters SoftwareClockHost::consumeTimingCounters() noexcept {
    if (!session_) return {};
    return {session_->callbacks.exchange(0), session_->delayedStarts.exchange(0),
        session_->overruns.exchange(0), session_->worstNs.exchange(0)};
}
void SoftwareClockHost::setStageMeasurementsEnabled(bool) noexcept {}
StageTimingCounters SoftwareClockHost::consumeOutputCopyStageCounters() noexcept { return {}; }
bool SoftwareClockHost::start(std::uint32_t frames, const std::vector<std::uint32_t>& inputs,
    const std::vector<std::uint32_t>& outputs, AudioClockProcessCallback callback, void* context, std::string& error) {
    if (!session_) { error = "Software clock is closed"; return false; }
    auto& s = *session_;
    if (s.worker.joinable()) { error = "Software clock is already running"; return false; }
    if (!inputs.empty() || !outputs.empty()) { error = "Software clock has no physical channels"; return false; }
    if (!callback || frames < 16 || frames > 8192) { error = "Invalid software callback or quantum"; return false; }
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) {
        error = "QPC unavailable for software clock"; return false;
    }
    s.frequency = frequency.QuadPart;
    s.frames = frames; s.callback = callback; s.context = context;
    s.initializationError.store(ERROR_SUCCESS);
    s.runtimeError.store(ERROR_SUCCESS);
    const auto handleFailure = [&s, &error]() {
        const auto failure = GetLastError();
        error = "Software clock handle initialization failed: " + std::to_string(failure);
        s.releaseHandles();
        return false;
    };
    s.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.stopEvent) return handleFailure();
    s.readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!s.readyEvent) return handleFailure();
    constexpr DWORD highResolution = 0x00000002;
    s.timer = CreateWaitableTimerExW(nullptr, nullptr, highResolution, TIMER_ALL_ACCESS);
    if (!s.timer) s.timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (!s.timer) return handleFailure();
    try { s.worker = std::thread([&s] { s.run(); }); }
    catch (const std::system_error& e) {
        error = std::string("Software clock thread creation failed: ") + e.what();
        s.releaseHandles(); return false;
    }
    const auto wait = WaitForSingleObject(s.readyEvent, INFINITE);
    const auto failure = wait == WAIT_OBJECT_0 ? s.initializationError.load(std::memory_order_acquire) : GetLastError();
    if (wait != WAIT_OBJECT_0 || failure != ERROR_SUCCESS) {
        error = "Software clock worker initialization failed: " + std::to_string(failure);
        stop(); return false;
    }
    error.clear(); return true;
}
void SoftwareClockHost::stop() noexcept {
    if (!session_) return;
    auto& s = *session_;
    if (s.worker.joinable()) { SetEvent(s.stopEvent); s.worker.join(); }
    s.releaseHandles();
}
void SoftwareClockHost::close() noexcept { stop(); session_.reset(); }
