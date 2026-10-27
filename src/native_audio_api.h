#pragma once
#include "native_audio_manager.h"
#include <nlohmann/json.hpp>
#include <string>

// Snapshot projection only. No transport, installation or activation occurs here.
inline const char* NativeAudioApiState(const NativeAudioManagerSnapshot& s) noexcept {
    if (!s.enabled || s.state == NativeAudioManagerState::Disabled) return "disabled";
    if (s.state == NativeAudioManagerState::WaitingForMaster) return "starting";
    if (s.state == NativeAudioManagerState::Attached && s.attached &&
        s.health.state == NativeAudioState::Running) return "running";
    const auto error = s.health.state == NativeAudioState::Failed ? s.health.error : s.lastResult.code;
    switch (error) {
    case NativeTransferCode::Unavailable: return "unavailable";
    case NativeTransferCode::AccessDenied: return "denied";
    case NativeTransferCode::OwnedElsewhere: return "ownedElsewhere";
    case NativeTransferCode::Removed: return "removed";
    case NativeTransferCode::Cancelled:
    case NativeTransferCode::InvalidPacket:
    case NativeTransferCode::Failed: return "failed";
    default: break;
    }
    return s.state == NativeAudioManagerState::Failed ? "failed" : "starting";
}

inline nlohmann::json NativeAudioApiJson(const NativeAudioManagerSnapshot& s) {
    using Json = nlohmann::json;
    const auto& h = s.health;
    const std::string state = NativeAudioApiState(s);
    Json graph = nullptr;
    if (state == "running" && s.graphRate && s.graphQuantum)
        graph = {{"sampleRate", s.graphRate}, {"maxQuantum", s.graphQuantum}, {"channels", 2}};
    return {{"enabled", s.enabled}, {"state", state}, {"masterRunning", s.masterRunning},
        {"attached", s.attached}, {"epoch", std::to_string(s.epoch)},
        {"wireGeneration", std::to_string(s.wireGeneration)},
        {"nativeFormat", {{"sampleRate", 48000}, {"channels", 2}, {"sampleType", "pcm32"}}},
        {"graphFormat", std::move(graph)}, {"retryAttempt", s.retryAttempt}, {"retryInMs", s.retryInMs},
        {"systemError", h.state == NativeAudioState::Failed ? h.systemError : s.lastResult.systemError},
        {"health", {{"renderNativeFrames", h.renderNativeFrames}, {"captureNativeFrames", h.captureNativeFrames},
            {"captureSilencedNativeFrames", h.captureSilencedNativeFrames}, {"captureSilencedGraphFrames", h.captureSilencedGraphFrames},
            {"skippedDeadlines", h.skippedDeadlines}, {"discardedPacingFrames", h.discardedPacingFrames},
            {"controlBacklogUpdates", h.controlBacklogUpdates}, {"discontinuities", h.discontinuities},
            {"kernelRenderDroppedNativeFrames", h.kernelRenderDroppedNativeFrames},
            {"kernelCaptureDroppedNativeFrames", h.kernelCaptureDroppedNativeFrames},
            {"kernelCaptureSilencedNativeFrames", h.kernelCaptureSilencedNativeFrames}}}};
}
