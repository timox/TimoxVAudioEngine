#include "physical_asio_host.h"
#include "asio_error.h"
#include "asio_sample_conversion.h"

#include <windows.h>
#include "asiolist.h"
#include "iasiodrv.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace {
using BufferDispatch = void (*)(void*, long) noexcept;
std::atomic<void*> gActiveSession{nullptr};
BufferDispatch gBufferDispatch = nullptr;
std::atomic<bool>* gRateChanged = nullptr;

void asioBufferSwitch(long index, ASIOBool) {
    if (void* session = gActiveSession.load(std::memory_order_acquire))
        if (gBufferDispatch) gBufferDispatch(session, index);
}

ASIOTime* asioBufferSwitchTimeInfo(ASIOTime* timeInfo, long index, ASIOBool) {
    asioBufferSwitch(index, ASIOFalse);
    return timeInfo;
}

void asioSampleRateChanged(ASIOSampleRate) {
    if (gRateChanged) gRateChanged->store(true, std::memory_order_release);
}

long asioMessage(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported)
        return value == kAsioSupportsTimeInfo || value == kAsioEngineVersion ? 1 : 0;
    if (selector == kAsioSupportsTimeInfo) return 1;
    if (selector == kAsioEngineVersion) return 2;
    return 0;
}

}

namespace {
bool isOwnVasioDriver(const char* name) {
    if (!name) return false;
    return _stricmp(name, "TimoxVasio") == 0;
}

std::wstring wideAsioName(const char (&name)[32]) {
    const auto end = std::find(std::begin(name), std::end(name), '\0');
    const auto length = static_cast<int>(end - std::begin(name));
    if (length <= 0) return {};
    const int wideLength = MultiByteToWideChar(CP_ACP, 0, name, length, nullptr, 0);
    if (wideLength <= 0) return {};
    std::wstring result(static_cast<std::size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, name, length, result.data(), wideLength) != wideLength)
        return {};
    return result;
}

void readChannelNames(IASIO& driver, long inputCount, long outputCount,
                      std::vector<std::wstring>& inputNames,
                      std::vector<std::wstring>& outputNames) {
    const auto readDirection = [&driver](long count, ASIOBool isInput,
                                         std::vector<std::wstring>& names) {
        names.clear();
        names.reserve(static_cast<std::size_t>(count));
        for (long channel = 0; channel < count; ++channel) {
            ASIOChannelInfo info{};
            info.channel = channel;
            info.isInput = isInput;
            names.push_back(driver.getChannelInfo(&info) == ASE_OK ? wideAsioName(info.name) : L"");
        }
    };
    readDirection(inputCount, ASIOTrue, inputNames);
    readDirection(outputCount, ASIOFalse, outputNames);
}

void resolveKnownPhysicalChannelNames(const std::string& driverName,
                                      long inputCount, long outputCount,
                                      std::vector<std::wstring>& outputNames) {
    // SSL's Windows ASIO driver returns "Out 3" through "Out 8" for the
    // SSL 12, even though those endpoints have fixed hardware roles. Keep
    // names supplied by the driver, and resolve only its generic labels.
    if (_stricmp(driverName.c_str(), "SSL ASIO Driver 1") != 0 ||
        inputCount != 16 || outputCount != 8 || outputNames.size() != 8 ||
        outputNames[0] != L"Mon L" || outputNames[1] != L"Mon R") return;

    constexpr std::array<const wchar_t*, 6> ssl12Names{
        L"Line 3", L"Line 4", L"Headphone A L", L"Headphone A R",
        L"Headphone B L", L"Headphone B R"
    };
    for (std::size_t index = 2; index < outputNames.size(); ++index) {
        const auto genericName = std::wstring(L"Out ") + std::to_wstring(index + 1);
        if (outputNames[index] == genericName)
            outputNames[index] = ssl12Names[index - 2];
    }
}
}

struct PhysicalAsioHost::Session {
    AsioDriverList drivers;
    int driverIndex = -1;
    std::string physicalDriverName;
    IASIO* driver = nullptr;
    bool initialized = false;
    bool postOutput = false; // Queried before buffers; immutable while callbacks run.
    bool buffersCreated = false;
    bool started = false;
    std::uint32_t bufferFrames = 0;
    PhysicalAsioProcessCallback process = nullptr;
    void* processContext = nullptr;
    std::vector<ASIOBufferInfo> buffers;
    ASIOCallbacks callbacks{};
    bool hasClockOnlyBuffer = false;
    bool clockOnlyBufferIsOutput = false;
    std::size_t clockOnlyBufferIndex = 0;
    std::uint32_t clockOnlyBytesPerSample = 0;
    std::vector<ASIOChannelInfo> inputChannels;
    std::vector<ASIOChannelInfo> outputChannels;
    std::vector<AsioSampleConversion::DecodeBlockFn> inputDecoders;
    std::vector<AsioSampleConversion::EncodeBlockFn> outputEncoders;
    std::vector<float> inputSamples;
    std::vector<float> outputSamples;
    std::vector<const float*> inputPointers;
    std::vector<float*> outputPointers;
    std::atomic<bool> sampleRateChanged{false};
    std::uint64_t timingFrequency = 0;
    std::uint64_t timingPeriodTicks = 0;
    std::uint64_t previousCallbackStart = 0; // Physical callback thread only.
    std::atomic<std::uint64_t> timingCallbacks{0};
    std::atomic<std::uint64_t> timingDelayedStarts{0};
    std::atomic<std::uint64_t> timingOverruns{0};
    std::atomic<std::uint64_t> timingWorstTicks{0};
    std::atomic<bool> stageMeasurementsEnabled{false};
    std::uint32_t stageSampleIndex = 0; // Physical callback thread only.
    StageTimingAccumulator outputCopyTiming;

    void Process(long bufferIndex) noexcept {
        if (bufferIndex < 0 || bufferIndex > 1 || !process) return;
        LARGE_INTEGER callbackStart{};
        QueryPerformanceCounter(&callbackStart);
        const auto startTick = static_cast<std::uint64_t>(callbackStart.QuadPart);
        if (previousCallbackStart && startTick >= previousCallbackStart && timingPeriodTicks &&
            startTick - previousCallbackStart > timingPeriodTicks + timingPeriodTicks / 2)
            timingDelayedStarts.fetch_add(1, std::memory_order_relaxed);
        previousCallbackStart = startTick;
        const auto index = static_cast<std::size_t>(bufferIndex);
        const auto frames = bufferFrames;
        const auto inputCount = inputChannels.size();
        const auto outputCount = outputChannels.size();
        std::fill(inputPointers.begin(), inputPointers.end(), nullptr);
        std::fill(outputPointers.begin(), outputPointers.end(), nullptr);
        for (std::size_t channel = 0; channel < inputCount; ++channel) {
            const auto& info = inputChannels[channel];
            auto* destination = inputSamples.data() + channel * frames;
            const auto* source = static_cast<const std::uint8_t*>(buffers[channel].buffers[index]);
            inputDecoders[channel](destination, source, frames);
            inputPointers[static_cast<std::size_t>(info.channel)] = destination;
        }
        for (std::size_t channel = 0; channel < outputCount; ++channel) {
            auto* destination = outputSamples.data() + channel * frames;
            std::fill_n(destination, frames, 0.0f);
            outputPointers[static_cast<std::size_t>(outputChannels[channel].channel)] = destination;
        }
        process(inputPointers.data(), outputPointers.data(), frames, processContext);
        const bool sampleOutputCopy = stageMeasurementsEnabled.load(std::memory_order_relaxed) &&
            (++stageSampleIndex % 16u == 0);
        LARGE_INTEGER outputCopyStart{};
        if (sampleOutputCopy) QueryPerformanceCounter(&outputCopyStart);
        const auto outputBase = inputCount;
        for (std::size_t channel = 0; channel < outputCount; ++channel) {
            const auto& info = outputChannels[channel];
            auto* destination = static_cast<std::uint8_t*>(buffers[outputBase + channel].buffers[index]);
            const auto* source = outputPointers[static_cast<std::size_t>(info.channel)];
            if (source) outputEncoders[channel](destination, source, frames);
            else std::memset(destination, 0, static_cast<std::size_t>(frames) *
                AsioSampleConversion::BytesPerSample(info.type));
        }
        if (hasClockOnlyBuffer && clockOnlyBufferIsOutput) {
            auto* destination = static_cast<std::uint8_t*>(buffers[clockOnlyBufferIndex].buffers[index]);
            std::memset(destination, 0, static_cast<std::size_t>(frames) * clockOnlyBytesPerSample);
        }
        if (postOutput) driver->outputReady();
        if (sampleOutputCopy && outputCount > 0) {
            LARGE_INTEGER outputCopyEnd{};
            QueryPerformanceCounter(&outputCopyEnd);
            if (outputCopyEnd.QuadPart >= outputCopyStart.QuadPart)
                outputCopyTiming.Add(static_cast<std::uint64_t>(outputCopyEnd.QuadPart - outputCopyStart.QuadPart));
        }
        LARGE_INTEGER callbackEnd{};
        QueryPerformanceCounter(&callbackEnd);
        const auto endTick = static_cast<std::uint64_t>(callbackEnd.QuadPart);
        if (endTick >= startTick) {
            const auto elapsed = endTick - startTick;
            if (timingPeriodTicks && elapsed > timingPeriodTicks)
                timingOverruns.fetch_add(1, std::memory_order_relaxed);
            auto worst = timingWorstTicks.load(std::memory_order_relaxed);
            if (elapsed > worst) timingWorstTicks.compare_exchange_strong(worst, elapsed,
                std::memory_order_relaxed, std::memory_order_relaxed);
        }
        timingCallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    ~Session() {
        if (!driver) return;
        if (started) driver->stop();
        if (buffersCreated) driver->disposeBuffers();
        if (gActiveSession.load(std::memory_order_acquire) == this) {
            gActiveSession.store(nullptr, std::memory_order_release);
            gRateChanged = nullptr;
        }
        drivers.asioCloseDriver(driverIndex);
    }
};

PhysicalAsioHost::PhysicalAsioHost() = default;
PhysicalAsioHost::~PhysicalAsioHost() { close(); }

std::vector<PhysicalAsioDriverInfo> PhysicalAsioHost::enumerate() const {
    AsioDriverList list;
    std::vector<PhysicalAsioDriverInfo> result;
    const int count = static_cast<int>(list.asioGetNumDev());
    result.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        char name[128]{};
        CLSID clsid{};
        if (list.asioGetDriverName(index, name, sizeof(name)) != 0 ||
            list.asioGetDriverCLSID(index, &clsid) != 0 || isOwnVasioDriver(name)) continue;

        wchar_t guid[40]{};
        if (!StringFromGUID2(clsid, guid, static_cast<int>(std::size(guid)))) continue;
        result.push_back({guid, name});
    }
    return result;
}

bool PhysicalAsioHost::open(const std::wstring& id, void* systemHandle,
                            PhysicalAsioCapabilities& capabilities, std::string& error) {
    close();
    error.clear();
    capabilities = {};

    CLSID requested{};
    if (id.empty() || FAILED(CLSIDFromString(id.c_str(), &requested))) {
        error = "Invalid ASIO driver CLSID";
        return false;
    }

    auto pending = std::make_unique<Session>();
    const int count = static_cast<int>(pending->drivers.asioGetNumDev());
    for (int index = 0; index < count; ++index) {
        CLSID candidate{};
        char name[128]{};
        if (pending->drivers.asioGetDriverCLSID(index, &candidate) != 0 ||
            pending->drivers.asioGetDriverName(index, name, sizeof(name)) != 0 ||
            isOwnVasioDriver(name)) continue;
        if (IsEqualCLSID(candidate, requested)) {
            pending->driverIndex = index;
            pending->physicalDriverName = name;
            break;
        }
    }
    if (pending->driverIndex < 0) {
        error = "ASIO driver CLSID is not present in the external inventory";
        return false;
    }

    LPVOID instance = nullptr;
    const LONG opened = pending->drivers.asioOpenDriver(pending->driverIndex, &instance);
    if (opened != 0 || !instance) {
        error = "ASIO driver activation failed (code " + std::to_string(opened) + ")";
        return false;
    }
    pending->driver = static_cast<IASIO*>(instance);
    if (pending->driver->init(systemHandle) != ASIOTrue) {
        char message[128]{};
        pending->driver->getErrorMessage(message);
        error = message[0] ? message : "ASIO driver initialization failed";
        return false;
    }
    pending->initialized = true;
    // ASIOOutputReady contract permits querying before buffers exist.
    // PortAudio's LoadAsioDriver uses the same capability handshake.
    pending->postOutput = pending->driver->outputReady() == ASE_OK;

    ASIOError result = pending->driver->getChannels(&capabilities.inputChannels,
                                                     &capabilities.outputChannels);
    if (result != ASE_OK) {
        error = "ASIO getChannels failed (code " + std::to_string(result) + ")";
        return false;
    }
    readChannelNames(*pending->driver, capabilities.inputChannels, capabilities.outputChannels,
                     capabilities.inputChannelNames, capabilities.outputChannelNames);
    resolveKnownPhysicalChannelNames(pending->physicalDriverName, capabilities.inputChannels,
                                     capabilities.outputChannels, capabilities.outputChannelNames);
    result = pending->driver->getBufferSize(&capabilities.minBufferFrames,
        &capabilities.maxBufferFrames, &capabilities.preferredBufferFrames,
        &capabilities.bufferGranularity);
    if (result != ASE_OK) {
        error = "ASIO getBufferSize failed (code " + std::to_string(result) + ")";
        return false;
    }
    ASIOSampleRate currentRate = 0.0;
    result = pending->driver->getSampleRate(&currentRate);
    if (result != ASE_OK) {
        error = "ASIO getSampleRate failed (code " + std::to_string(result) + ")";
        return false;
    }
    capabilities.currentSampleRate = currentRate;

    constexpr std::array<double, 15> candidates{
        8000.0, 11025.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0,
        88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0, 768000.0
    };
    for (const auto rate : candidates)
        if (pending->driver->canSampleRate(rate) == ASE_OK) capabilities.supportedSampleRates.push_back(rate);

    session_ = std::move(pending);
    return true;
}

bool PhysicalAsioHost::setSampleRate(double sampleRate, std::string& error) {
    error.clear();
    if (!session_ || !session_->driver || !session_->initialized) {
        error = "No initialized ASIO driver session";
        return false;
    }
    if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
        session_->driver->canSampleRate(sampleRate) != ASE_OK) {
        error = "Selected ASIO driver does not support the requested sample rate";
        return false;
    }
    ASIOSampleRate current = 0.0;
    if (session_->driver->getSampleRate(&current) != ASE_OK) {
        error = "ASIO getSampleRate failed before setting the engine clock";
        return false;
    }
    if (current != sampleRate && session_->driver->setSampleRate(sampleRate) != ASE_OK) {
        error = "ASIO driver rejected the requested sample rate";
        return false;
    }
    return true;
}

bool PhysicalAsioHost::refreshCapabilities(PhysicalAsioCapabilities& capabilities, std::string& error) {
    error.clear();
    if (!session_ || !session_->driver || !session_->initialized) {
        error = "No initialized ASIO driver session";
        return false;
    }
    auto& driver = *session_->driver;
    PhysicalAsioCapabilities refreshed;
    if (driver.getChannels(&refreshed.inputChannels, &refreshed.outputChannels) != ASE_OK ||
        refreshed.inputChannels < 0 || refreshed.outputChannels < 0) {
        error = "ASIO getChannels failed after applying the sample rate";
        return false;
    }
    readChannelNames(driver, refreshed.inputChannels, refreshed.outputChannels,
                     refreshed.inputChannelNames, refreshed.outputChannelNames);
    resolveKnownPhysicalChannelNames(session_->physicalDriverName, refreshed.inputChannels,
                                     refreshed.outputChannels, refreshed.outputChannelNames);
    if (driver.getBufferSize(&refreshed.minBufferFrames, &refreshed.maxBufferFrames,
            &refreshed.preferredBufferFrames, &refreshed.bufferGranularity) != ASE_OK) {
        error = "ASIO getBufferSize failed after applying the sample rate";
        return false;
    }
    ASIOSampleRate currentRate = 0.0;
    if (driver.getSampleRate(&currentRate) != ASE_OK || !std::isfinite(currentRate) || currentRate <= 0.0) {
        error = "ASIO getSampleRate failed after applying the sample rate";
        return false;
    }
    refreshed.currentSampleRate = currentRate;
    constexpr std::array<double, 15> candidates{
        8000.0, 11025.0, 16000.0, 22050.0, 24000.0, 32000.0, 44100.0, 48000.0,
        88200.0, 96000.0, 176400.0, 192000.0, 352800.0, 384000.0, 768000.0
    };
    for (const auto rate : candidates)
        if (driver.canSampleRate(rate) == ASE_OK) refreshed.supportedSampleRates.push_back(rate);
    capabilities = std::move(refreshed);
    return true;
}

bool PhysicalAsioHost::consumeSampleRateChange() noexcept {
    return session_ && session_->sampleRateChanged.exchange(false, std::memory_order_acq_rel);
}

PhysicalAsioTimingCounters PhysicalAsioHost::consumeTimingCounters() noexcept {
    if (!session_ || !session_->started) return {};
    auto& session = *session_;
    PhysicalAsioTimingCounters counters;
    counters.callbacks = session.timingCallbacks.exchange(0, std::memory_order_relaxed);
    counters.delayedStarts = session.timingDelayedStarts.exchange(0, std::memory_order_relaxed);
    counters.processingOverruns = session.timingOverruns.exchange(0, std::memory_order_relaxed);
    const auto worstTicks = session.timingWorstTicks.exchange(0, std::memory_order_relaxed);
    if (session.timingFrequency)
        counters.worstProcessingNs = static_cast<std::uint64_t>(
            static_cast<long double>(worstTicks) * 1000000000.0L / session.timingFrequency);
    return counters;
}

void PhysicalAsioHost::setStageMeasurementsEnabled(bool enabled) noexcept {
    if (!session_) return;
    session_->stageMeasurementsEnabled.store(enabled, std::memory_order_release);
    if (!enabled) session_->outputCopyTiming.Consume();
}

StageTimingCounters PhysicalAsioHost::consumeOutputCopyStageCounters() noexcept {
    return session_ && session_->started ? session_->outputCopyTiming.Consume() : StageTimingCounters{};
}

bool PhysicalAsioHost::start(std::uint32_t bufferFrames,
                             const std::vector<std::uint32_t>& routedInputChannels,
                             const std::vector<std::uint32_t>& routedOutputChannels,
                             PhysicalAsioProcessCallback callback, void* context, std::string& error) {
    error.clear();
    if (!session_ || !session_->driver || !session_->initialized) {
        error = "No initialized ASIO driver session";
        return false;
    }
    auto& session = *session_;
    if (session.started) {
        error = "ASIO driver is already started";
        return false;
    }
    if (!callback) {
        error = "ASIO audio callback is required";
        return false;
    }
    long minFrames = 0, maxFrames = 0, preferredFrames = 0, granularity = 0;
    if (session.driver->getBufferSize(&minFrames, &maxFrames, &preferredFrames, &granularity) != ASE_OK ||
        bufferFrames < static_cast<std::uint32_t>(minFrames) ||
        bufferFrames > static_cast<std::uint32_t>(maxFrames) ||
        (granularity > 0 && (bufferFrames - static_cast<std::uint32_t>(minFrames)) % static_cast<std::uint32_t>(granularity) != 0) ||
        (granularity == -1 && (bufferFrames & (bufferFrames - 1)) != 0)) {
        error = "Requested ASIO buffer size is outside the driver's supported range";
        return false;
    }

    long inputCount = 0, outputCount = 0;
    if (session.driver->getChannels(&inputCount, &outputCount) != ASE_OK || inputCount < 0 || outputCount < 0) {
        error = "ASIO getChannels failed while preparing buffers";
        return false;
    }
    auto validateSelection = [&](const std::vector<std::uint32_t>& selected, long available,
                                 const char* direction) {
        std::vector<bool> seen(static_cast<std::size_t>(available), false);
        for (const auto channel : selected) {
            if (channel >= static_cast<std::uint32_t>(available) || seen[channel]) {
                error = std::string("Invalid or duplicate routed physical ") + direction + " channel";
                return false;
            }
            seen[channel] = true;
        }
        return true;
    };
    if (!validateSelection(routedInputChannels, inputCount, "input") ||
        !validateSelection(routedOutputChannels, outputCount, "output")) return false;

    // ASIO hosts commonly reject createBuffers with zero channel descriptors.
    // Keep the selected physical driver as the master clock even before the
    // first route exists, without exposing or routing this private clock buffer.
    const bool needsClockOnlyBuffer = routedInputChannels.empty() && routedOutputChannels.empty();
    bool clockOnlyIsOutput = false;
    std::uint32_t clockOnlyBytesPerSample = 0;
    long clockOnlyChannel = 0;
    if (needsClockOnlyBuffer) {
        if (inputCount > 0) {
            clockOnlyIsOutput = false;
        } else if (outputCount > 0) {
            clockOnlyIsOutput = true;
            ASIOChannelInfo info{};
            info.channel = 0;
            info.isInput = ASIOFalse;
            if (session.driver->getChannelInfo(&info) != ASE_OK ||
                !AsioSampleConversion::IsSupported(info.type) ||
                !(clockOnlyBytesPerSample = AsioSampleConversion::BytesPerSample(info.type))) {
                error = "Physical ASIO driver has no usable channel for its master clock";
                return false;
            }
        } else {
            error = "Physical ASIO driver exposes no channel for its master clock";
            return false;
        }
    }

    session.inputChannels.resize(routedInputChannels.size());
    session.outputChannels.resize(routedOutputChannels.size());
    session.inputDecoders.resize(routedInputChannels.size());
    session.outputEncoders.resize(routedOutputChannels.size());
    for (std::size_t slot = 0; slot < routedInputChannels.size() + routedOutputChannels.size(); ++slot) {
        const bool isInput = slot < routedInputChannels.size();
        const auto channel = isInput ? routedInputChannels[slot] :
            routedOutputChannels[slot - routedInputChannels.size()];
        ASIOChannelInfo info{};
        info.channel = static_cast<long>(channel);
        info.isInput = isInput ? ASIOTrue : ASIOFalse;
        const ASIOError result = session.driver->getChannelInfo(&info);
        if (result != ASE_OK || !AsioSampleConversion::IsSupported(info.type)) {
            error = "ASIO channel has an unsupported sample format (channel " + std::to_string(info.channel) + ")";
            return false;
        }
        if (isInput) {
            session.inputChannels[slot] = info;
            session.inputDecoders[slot] = AsioSampleConversion::SelectDecoder(info.type);
        } else {
            const auto outputSlot = slot - routedInputChannels.size();
            session.outputChannels[outputSlot] = info;
            session.outputEncoders[outputSlot] = AsioSampleConversion::SelectEncoder(info.type);
        }
    }

    session.bufferFrames = bufferFrames;
    LARGE_INTEGER frequency{};
    ASIOSampleRate currentRate = 0.0;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        session.driver->getSampleRate(&currentRate) != ASE_OK || !std::isfinite(currentRate) ||
        currentRate <= 0.0) {
        error = "Unable to initialize physical ASIO callback timing";
        return false;
    }
    session.timingFrequency = static_cast<std::uint64_t>(frequency.QuadPart);
    session.timingPeriodTicks = static_cast<std::uint64_t>(
        static_cast<long double>(session.timingFrequency) * bufferFrames / currentRate);
    session.previousCallbackStart = 0;
    session.process = callback;
    session.processContext = context;
    session.inputSamples.assign(routedInputChannels.size() * bufferFrames, 0.0f);
    session.outputSamples.assign(routedOutputChannels.size() * bufferFrames, 0.0f);
    session.inputPointers.resize(static_cast<std::size_t>(inputCount), nullptr);
    session.outputPointers.resize(static_cast<std::size_t>(outputCount), nullptr);
    session.hasClockOnlyBuffer = needsClockOnlyBuffer;
    session.clockOnlyBufferIsOutput = clockOnlyIsOutput;
    session.clockOnlyBufferIndex = routedInputChannels.size() + routedOutputChannels.size();
    session.clockOnlyBytesPerSample = clockOnlyBytesPerSample;
    session.buffers.resize(routedInputChannels.size() + routedOutputChannels.size() +
        (needsClockOnlyBuffer ? 1u : 0u));
    for (std::size_t slot = 0; slot < routedInputChannels.size() + routedOutputChannels.size(); ++slot) {
        auto& buffer = session.buffers[slot];
        const bool isInput = slot < routedInputChannels.size();
        buffer.isInput = isInput ? ASIOTrue : ASIOFalse;
        buffer.channelNum = isInput ? static_cast<long>(routedInputChannels[slot]) :
            static_cast<long>(routedOutputChannels[slot - routedInputChannels.size()]);
        buffer.buffers[0] = buffer.buffers[1] = nullptr;
    }
    if (needsClockOnlyBuffer) {
        auto& buffer = session.buffers[session.clockOnlyBufferIndex];
        buffer.isInput = clockOnlyIsOutput ? ASIOFalse : ASIOTrue;
        buffer.channelNum = clockOnlyChannel;
        buffer.buffers[0] = buffer.buffers[1] = nullptr;
    }

    session.callbacks.bufferSwitch = asioBufferSwitch;
    session.callbacks.sampleRateDidChange = asioSampleRateChanged;
    session.callbacks.asioMessage = asioMessage;
    session.callbacks.bufferSwitchTimeInfo = asioBufferSwitchTimeInfo;
    void* noActiveSession = nullptr;
    if (!gActiveSession.compare_exchange_strong(noActiveSession, &session,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        error = "Another physical ASIO callback session is already active";
        return false;
    }
    gBufferDispatch = [](void* state, long index) noexcept {
        static_cast<Session*>(state)->Process(index);
    };
    gRateChanged = &session.sampleRateChanged;
    const ASIOError createResult = session.driver->createBuffers(session.buffers.data(),
        static_cast<long>(session.buffers.size()), static_cast<long>(bufferFrames), &session.callbacks);
    if (createResult != ASE_OK) {
        error = DescribeAsioFailure(*session.driver, "createBuffers", createResult);
        gActiveSession.store(nullptr, std::memory_order_release);
        gRateChanged = nullptr;
        return false;
    }
    session.buffersCreated = true;
    for (const auto& buffer : session.buffers) {
        if (!buffer.buffers[0] || !buffer.buffers[1]) {
            error = "ASIO createBuffers returned a null audio buffer";
            session.driver->disposeBuffers();
            session.buffersCreated = false;
            gActiveSession.store(nullptr, std::memory_order_release);
            gRateChanged = nullptr;
            return false;
        }
    }
    const ASIOError startResult = session.driver->start();
    if (startResult != ASE_OK) {
        error = DescribeAsioFailure(*session.driver, "start", startResult);
        session.driver->disposeBuffers();
        session.buffersCreated = false;
        gActiveSession.store(nullptr, std::memory_order_release);
        gRateChanged = nullptr;
        return false;
    }
    session.started = true;
    return true;
}

void PhysicalAsioHost::stop() noexcept {
    if (!session_ || !session_->driver) return;
    auto& session = *session_;
    if (session.started) {
        session.driver->stop();
        session.started = false;
    }
    if (session.buffersCreated) {
        session.driver->disposeBuffers();
        session.buffersCreated = false;
    }
    if (gActiveSession.load(std::memory_order_acquire) == &session)
        gActiveSession.store(nullptr, std::memory_order_release);
    gRateChanged = nullptr;
}

void PhysicalAsioHost::close() noexcept { session_.reset(); }
