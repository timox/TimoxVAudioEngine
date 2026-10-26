#pragma once

#include "audio_transport.h"

struct alignas(64) ClientStatusV2 {
    std::uint32_t magic, schemaVersion, size;
    volatile LONG ready;
    std::uint32_t dllVersion[4];
    wchar_t dllPath[1024];
    alignas(8) volatile LONG64 qpcFrequency;
    volatile LONG state, requestedSampleRate, requestedBufferFrames;
    alignas(8) volatile LONG64 exchangeGeneration;
    volatile LONG64 firstCallbackQpc, lastCallbackQpc, lastSuccessfulExchangeQpc;
    volatile LONG64 lastIntervalQpc, callbackCount, successfulExchangeCount;
    volatile LONG64 exchangedFrames, resetRequests;
};

struct StageCounterBank {
    volatile LONG inFlight;
    volatile LONG64 hostCallbackSamples, hostCallbackSumTicks, hostCallbackWorstTicks;
    volatile LONG64 ringWriteSamples, ringWriteSumTicks, ringWriteWorstTicks;
    volatile LONG64 ringWaitSamples, ringWaitSumTicks, ringWaitWorstTicks;
    volatile LONG64 ringWaitDiscardedBlocks;
};

struct alignas(64) AudioClientMapping::Shared {
    std::uint32_t magic, version, size, driverId, processId;
    alignas(8) std::uint64_t processCreationTime;
    volatile LONG sampleRate, blockFrames, preferredBufferFrames;
    alignas(8) volatile LONG64 generation, nextAudioEpoch, publishedAudioEpoch, acknowledgedAudioEpoch;
    volatile LONG acknowledgedSampleRate, acknowledgedBlockFrames;
    volatile LONG64 activeChannelsGeneration;
    volatile LONG64 activeInputs[kChannelMaskWords], activeOutputs[kChannelMaskWords];
    volatile LONG64 inputUnderruns, outputOverruns, inputOverruns, outputUnderruns;
    volatile LONG64 engineAttached;
    volatile LONG engineProcessId;
    volatile LONG stageMeasurementsEnabled;
    alignas(8) volatile LONG64 stageMeasurementGeneration;
    alignas(8) volatile LONG64 stageCounterGeneration;
    volatile LONG stageCounterConsumer;
    alignas(8) volatile LONG64 stageCounterEpoch;
    StageCounterBank stageCounterBanks[2];
    ClientStatusV2 status;
};

struct alignas(64) AudioClientMapping::Audio {
    struct SampledBlock {
        alignas(8) volatile LONG64 firstFrame, epoch, generation, publishQpc;
        volatile LONG frames;
    };
    std::uint32_t magic, version, size, driverId, processId;
    alignas(8) std::uint64_t processCreationTime, epoch;
    std::uint32_t capacityFrames, inputStride, outputStride;
    std::uint16_t inputChannels[kChannelCount], outputChannels[kChannelCount];
    alignas(8) volatile LONG64 clientWriteFrame, engineReadFrame;
    volatile LONG64 sampledWriteIndex, sampledReadIndex;
    SampledBlock sampledOutput[kRingCapacityFrames];
    volatile LONG64 engineWriteFrame, clientReadFrame;
    alignas(64) float clientToEngine[kRingCapacityFrames * kChannelCount];
    alignas(64) float engineToClient[kRingCapacityFrames * kChannelCount];
};

static_assert(sizeof(void*) == 8, "v9 transport requires x64");
