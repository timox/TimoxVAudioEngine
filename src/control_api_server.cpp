#include <httplib.h>
#include <nlohmann/json.hpp>

#include "control_api_server.h"
#include "native_audio_api.h"
#include "audio_controller.h"
#include "routing_graph.h"
#include "persistent_routes.h"
#include "vasio_client_manager.h"
#include "engine_diagnostics.h"
#include "pwar_configuration_store.h"
#include "pwar_client_supervisor.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr std::uint32_t kVasioId = 1;
constexpr char kSubprotocol[] = "vasio.api.v1";

bool supportsWasapiProcessLoopback() noexcept {
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    const auto ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto getVersion = ntdll ? reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
    if (!getVersion) return false;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (getVersion(&version) != 0) return false;
    return version.dwMajorVersion > 10 ||
        (version.dwMajorVersion == 10 && version.dwBuildNumber >= 20348);
}

const char* asioClientStateName(AudioClientMapping::ClientState state) noexcept {
    switch (state) {
    case AudioClientMapping::ClientState::Initialized: return "initialized";
    case AudioClientMapping::ClientState::BuffersReady: return "buffers-ready";
    case AudioClientMapping::ClientState::Running: return "running";
    case AudioClientMapping::ClientState::Stopped: return "stopped";
    case AudioClientMapping::ClientState::Detached: return "detached";
    }
    return "unknown";
}

std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr) != bytes) return {};
    return result;
}

bool wide(const std::string& value, std::wstring& result) {
    if (value.empty()) { result.clear(); return true; }
    const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (characters <= 0) return false;
    result.resize(static_cast<std::size_t>(characters));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), characters) == characters;
}

bool validOrigin(const std::string& origin) {
    return origin.empty() || origin == "null" || origin == "file://" ||
           origin == "http://localhost:3000" || origin == "http://127.0.0.1:3000";
}

bool offersSubprotocol(const httplib::Request& request) {
    const auto value = request.get_header_value("Sec-WebSocket-Protocol");
    std::size_t begin = 0;
    while (begin < value.size()) {
        const auto comma = value.find(',', begin);
        const auto end = comma == std::string::npos ? value.size() : comma;
        const auto first = value.find_first_not_of(" \t", begin);
        if (first != std::string::npos && first < end) {
            const auto last = value.find_last_not_of(" \t", end - 1);
            if (value.substr(first, last - first + 1) == kSubprotocol) return true;
        }
        if (comma == std::string::npos) break;
        begin = comma + 1;
    }
    return false;
}

void setCors(const httplib::Request& request, httplib::Response& response) {
    const auto origin = request.get_header_value("Origin");
    if (!origin.empty()) {
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Vary", "Origin");
    }
}

Json errorEnvelope(const std::string& id, const char* code, const std::string& message,
                   const std::string& operation = {}) {
    Json error{{"code", code}, {"message", message}};
    if (!operation.empty()) error["operation"] = operation;
    return Json{{"id", id}, {"success", false}, {"error", std::move(error)}};
}

Json clockSourceJson(const AudioControllerSnapshot& snapshot) {
    return Json{{"kind", snapshot.clockSource == AudioClockSource::Software ? "software" : "asio"}};
}

Json clockCapabilitiesJson(const AudioControllerSnapshot& snapshot) {
    const auto& caps = snapshot.clockCapabilities;
    if (!snapshot.sampleRate || caps.preferredBufferFrames <= 0) return nullptr;
    return Json{{"sampleRates", caps.supportedSampleRates}, {"bufferSizes", {
        {"minimumFrames", caps.minBufferFrames}, {"maximumFrames", caps.maxBufferFrames},
        {"preferredFrames", caps.preferredBufferFrames}, {"granularity", caps.bufferGranularity}}}};
}

Json statusEvent(const AudioControllerSnapshot& snapshot) {
    Json physicalId = snapshot.physicalDriverId ? Json(*snapshot.physicalDriverId) : Json(nullptr);
    Json sampleRate = snapshot.sampleRate ? Json(*snapshot.sampleRate) : Json(nullptr);
    Json bufferFrames = snapshot.bufferFrames ? Json(*snapshot.bufferFrames) : Json(nullptr);
    Json lastError = snapshot.lastError.empty() ? Json(nullptr) :
        Json{{"code", "AUDIO_CONFIGURATION_FAILED"}, {"message", snapshot.lastError}, {"operation", "apply"}};
    return Json{{"event", "engine.status"},
        {"payload", {{"state", snapshot.state}, {"clockSource", clockSourceJson(snapshot)},
                      {"clockCapabilities", clockCapabilitiesJson(snapshot)}, {"physicalDriverId", std::move(physicalId)},
                      {"sampleRate", std::move(sampleRate)}, {"bufferFrames", std::move(bufferFrames)},
                      {"lastError", std::move(lastError)}}}};
}

Json routeJson(const AudioRoute& route) {
    return Json{{"id", route.id}, {"sourceEndpointId", route.sourceEndpointId},
        {"destinationEndpointId", route.destinationEndpointId}, {"gainDb", route.gainDb},
        {"mute", route.mute}};
}

const char* wasapiKind(WasapiStreamKind kind) {
    switch (kind) {
    case WasapiStreamKind::Capture: return "capture";
    case WasapiStreamKind::Render: return "render";
    case WasapiStreamKind::EndpointLoopback: return "endpointLoopback";
    default: return "processLoopback";
    }
}
Json wasapiFormat(const WasapiMixFormat& f) {
    return {{"sampleRate", f.sampleRate}, {"channels", f.channels}, {"containerBits", f.containerBits},
        {"validBits", f.validBits}, {"blockAlign", f.blockAlign}, {"channelMask", f.channelMask},
        {"encoding", f.encoding == WasapiSampleEncoding::IeeeFloat ? "ieeeFloat" : "pcmInteger"}};
}
Json wasapiDevicesJson(const AudioControllerSnapshot& state) {
    Json result = Json::array();
    for (const auto& device : state.wasapiDevices)
        result.push_back({{"id", utf8(device.id)}, {"name", utf8(device.name)},
            {"containerId", device.containerId.empty() ? Json(nullptr) : Json(utf8(device.containerId))},
            {"flow", device.flow == WasapiDataFlow::Capture ? "capture" : "render"}, {"state", device.state},
            {"mixFormat", device.mixFormat ? wasapiFormat(*device.mixFormat) : Json(nullptr)},
            {"error", device.formatError.empty() ? Json(nullptr) : Json(device.formatError)}});
    return result;
}
Json windowsAudioApplicationsJson(const AudioControllerSnapshot& state) {
    Json result = Json::array();
    for (const auto& process : state.wasapiProcesses) {
        Json endpoints = Json::array();
        for (const auto& endpoint : process.renderEndpoints) endpoints.push_back(utf8(endpoint));
        result.push_back({{"processId", process.processId}, {"creationTime", std::to_string(process.creationTime)},
            {"imagePath", utf8(process.imagePath)}, {"displayName", utf8(process.displayName)},
            {"renderEndpoints", std::move(endpoints)}});
    }
    return result;
}
Json wasapiConfigurationsJson(const std::vector<WasapiBridgeConfiguration>& bridges) {
    Json result = Json::array();
    for (const auto& bridge : bridges) {
        Json item{{"id", bridge.id}, {"enabled", bridge.enabled}, {"kind", wasapiKind(bridge.open.kind)},
            {"bufferDuration100ns", bridge.open.bufferDuration100ns}};
        if (bridge.open.defaultRole)
            item["defaultRole"] = *bridge.open.defaultRole == WasapiDefaultRole::Console ? "console" :
                *bridge.open.defaultRole == WasapiDefaultRole::Multimedia ? "multimedia" : "communications";
        else if (bridge.open.kind == WasapiStreamKind::ProcessLoopback) {
            item["processId"] = bridge.open.processId;
            item["processCreationTime"] = std::to_string(bridge.open.processCreationTime);
            item["processImagePath"] = utf8(bridge.open.processImagePath);
        } else item["endpointId"] = utf8(bridge.open.endpointId);
        result.push_back(std::move(item));
    }
    return result;
}
bool parseWasapiConfigurations(const Json& value, std::vector<WasapiBridgeConfiguration>& result) {
    if (!value.is_array() || value.size() > 32) return false;
    std::vector<WasapiBridgeConfiguration> decoded;
    for (const auto& item : value) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string() ||
            !item.contains("kind") || !item["kind"].is_string()) return false;
        for (auto field = item.begin(); field != item.end(); ++field)
            if (field.key() != "id" && field.key() != "kind" && field.key() != "enabled" &&
                field.key() != "endpointId" && field.key() != "defaultRole" && field.key() != "bufferDuration100ns" &&
                field.key() != "processId" && field.key() != "processCreationTime" && field.key() != "processImagePath") return false;
        WasapiBridgeConfiguration bridge;
        bridge.id = item["id"].get<std::string>();
        if (item["kind"] == "capture") bridge.open.kind = WasapiStreamKind::Capture;
        else if (item["kind"] == "render") bridge.open.kind = WasapiStreamKind::Render;
        else if (item["kind"] == "endpointLoopback") bridge.open.kind = WasapiStreamKind::EndpointLoopback;
        else if (item["kind"] == "processLoopback") bridge.open.kind = WasapiStreamKind::ProcessLoopback;
        else return false;
        if (item.contains("enabled")) {
            if (!item["enabled"].is_boolean()) return false;
            bridge.enabled = item["enabled"].get<bool>();
        }
        if (bridge.open.kind == WasapiStreamKind::ProcessLoopback) {
            if (item.contains("endpointId") || item.contains("defaultRole") || !item.contains("processId") ||
                !item["processId"].is_number_unsigned() || item["processId"].get<std::uint64_t>() == 0 ||
                item["processId"].get<std::uint64_t>() > UINT32_MAX || !item.contains("processCreationTime") ||
                !item["processCreationTime"].is_string() || !item.contains("processImagePath") ||
                !item["processImagePath"].is_string()) return false;
            const auto creation = item["processCreationTime"].get<std::string>();
            if (creation.empty() || creation.size() > 20 || !std::all_of(creation.begin(), creation.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) return false;
            try { bridge.open.processCreationTime = std::stoull(creation); } catch (...) { return false; }
            bridge.open.processId = item["processId"].get<std::uint32_t>();
            if (!WasapiEndpointFromUtf8(item["processImagePath"].get<std::string>(), bridge.open.processImagePath)) return false;
        } else if (item.contains("endpointId")) {
            if (item.contains("defaultRole") || item.contains("processId") || item.contains("processCreationTime") || item.contains("processImagePath")) return false;
            if (!item["endpointId"].is_string() ||
                !WasapiEndpointFromUtf8(item["endpointId"].get<std::string>(), bridge.open.endpointId)) return false;
        } else {
            if (item.contains("endpointId") || !item.contains("defaultRole") || item.contains("processId") || item.contains("processCreationTime") || item.contains("processImagePath")) return false;
            if (!item["defaultRole"].is_string()) return false;
            if (item["defaultRole"] == "console") bridge.open.defaultRole = WasapiDefaultRole::Console;
            else if (item["defaultRole"] == "multimedia") bridge.open.defaultRole = WasapiDefaultRole::Multimedia;
            else if (item["defaultRole"] == "communications") bridge.open.defaultRole = WasapiDefaultRole::Communications;
            else return false;
        }
        if (item.contains("bufferDuration100ns")) {
            const auto& duration = item["bufferDuration100ns"];
            if (!duration.is_number_integer() ||
                (duration.is_number_unsigned() && duration.get<std::uint64_t>() > 10000000)) return false;
            bridge.open.bufferDuration100ns = duration.get<std::int64_t>();
        }
        decoded.push_back(std::move(bridge));
    }
    std::string error;
    if (!ValidateWasapiBridgeConfigurations(decoded, error)) return false;
    result = std::move(decoded);
    return true;
}
Json wasapiBridgesJson(const AudioControllerSnapshot& state) {
    Json result = Json::array();
    for (const auto& bridge : state.wasapiBridges) {
        Json effective = nullptr, graph = nullptr, nominal = nullptr, ppm = nullptr;
        Json inputs = Json::array(), outputs = Json::array();
        if (bridge.effectiveInfo) {
            const auto& info = *bridge.effectiveInfo;
            effective = {{"endpointId", utf8(info.endpointId)}, {"kind", wasapiKind(info.kind)},
                {"format", wasapiFormat(info.format)}, {"bufferFrames", info.bufferFrames},
                {"streamLatency100ns", info.streamLatency100ns}, {"defaultPeriod100ns", info.defaultPeriod100ns},
                {"minimumPeriod100ns", info.minimumPeriod100ns}};
        }
        if (bridge.graphFormat) {
            const auto& f = *bridge.graphFormat;
            graph = {{"sampleRate", f.graphSampleRate}, {"channels", f.channels},
                {"maxQuantum", f.maxQuantum}, {"capacityFrames", f.capacityFrames}};
            if (bridge.effectiveInfo && bridge.generation) {
                const bool render = f.direction == WasapiBridgeDirection::Render;
                const auto rate = bridge.effectiveInfo->format.sampleRate;
                if (rate && f.graphSampleRate) {
                    const double n = render ? static_cast<double>(rate) / f.graphSampleRate :
                        static_cast<double>(f.graphSampleRate) / rate;
                    nominal = n;
                    ppm = (bridge.health.ratio / n - 1.0) * 1000000.0;
                }
                for (std::size_t channel = 1; channel <= f.channels; ++channel) {
                    const auto direction = render ? "output" : "input";
                    Json port{{"id", "wasapi:" + bridge.id + ":" + direction + ":" + std::to_string(channel)},
                        {"name", bridge.id + " " + std::to_string(channel)}, {"direction", direction}, {"channel", channel}};
                    (render ? outputs : inputs).push_back(std::move(port));
                }
            }
        }
        const auto& h = bridge.health;
        result.push_back({{"id", bridge.id}, {"state", bridge.state}, {"generationId", std::to_string(bridge.generation)},
            {"retryAttempt", bridge.retryAttempt}, {"retryInMs", bridge.retryInMs},
            {"effectiveInfo", std::move(effective)}, {"graphFormat", std::move(graph)},
            {"inputEndpoints", std::move(inputs)}, {"outputEndpoints", std::move(outputs)},
            {"health", {{"error", static_cast<std::int32_t>(h.error)}, {"mmcssError", static_cast<std::int32_t>(h.mmcssError)},
                {"mmcssActive", h.mmcssActive}, {"primingTimedOut", h.primingTimedOut},
                {"capturedFrames", h.capturedFrames}, {"renderedFrames", h.renderedFrames},
                {"droppedCaptureFrames", h.droppedCaptureFrames}, {"renderSilencedInputFrames", h.renderSilencedInputFrames},
                {"renderSilencedDeviceFrames", h.renderSilencedDeviceFrames}, {"discontinuities", h.discontinuities},
                {"timestampErrors", h.timestampErrors}, {"controlBacklogUpdates", h.controlBacklogUpdates},
                {"ratio", h.ratio}, {"nominalRatio", nominal}, {"correctionPpm", ppm},
                {"bufferedFrames", bridge.bufferedFrames}, {"rejectedFrames", bridge.rejectedFrames}, {"silencedFrames", bridge.silencedFrames}}}});
    }
    return result;
}

Json applicationProfileJson(const ApplicationProfile& profile) {
    return Json{{"processName", utf8(profile.processName)},
        {"inputChannels", profile.inputChannels}, {"outputChannels", profile.outputChannels}};
}

Json applicationProfilesJson(const std::vector<ApplicationProfile>& profiles) {
    Json result = Json::array();
    for (const auto& profile : profiles) result.push_back(applicationProfileJson(profile));
    return Json{{"profiles", std::move(result)}};
}

Json endpoint(const std::string& id, const std::string& name,
              const char* direction, long channel) {
    return Json{{"id", id}, {"name", name}, {"direction", direction}, {"channel", channel}};
}

bool matchesSsl12ChannelLayout(const PhysicalAsioCapabilities& capabilities) {
    if (capabilities.inputChannels != 16 || capabilities.outputChannels != 8 ||
        capabilities.inputChannelNames.size() != 16 || capabilities.outputChannelNames.size() != 8)
        return false;

    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (capabilities.inputChannelNames[channel] != L"Analogue " + std::to_wstring(channel + 1))
            return false;
    }
    if (capabilities.inputChannelNames[4] != L"Talkback" ||
        capabilities.inputChannelNames[5] != L"Talkback" ||
        capabilities.inputChannelNames[6] != L"Loopback L" ||
        capabilities.inputChannelNames[7] != L"Loopback R")
        return false;
    for (std::size_t channel = 8; channel < 16; ++channel) {
        if (capabilities.inputChannelNames[channel] != L"ADAT " + std::to_wstring(channel - 7))
            return false;
    }
    if (capabilities.outputChannelNames[0] != L"Mon L" ||
        capabilities.outputChannelNames[1] != L"Mon R")
        return false;
    for (std::size_t channel = 2; channel < 8; ++channel) {
        if (capabilities.outputChannelNames[channel] != L"Out " + std::to_wstring(channel + 1))
            return false;
    }
    return true;
}

std::string physicalOutputChannelName(const PhysicalAsioDriverInfo& driver,
                                      const PhysicalAsioCapabilities& capabilities,
                                      long channel, const std::string& asioName) {
    static constexpr std::array<const char*, 8> ssl12Names{
        "Monitor L", "Monitor R", "Line 3", "Line 4",
        "Headphone A L", "Headphone A R", "Headphone B L", "Headphone B R"
    };
    const bool sslAsioSlot = driver.name.rfind("SSL ASIO Driver ", 0) == 0;
    if (sslAsioSlot && matchesSsl12ChannelLayout(capabilities) && channel >= 1 && channel <= 8)
        return ssl12Names[static_cast<std::size_t>(channel - 1)];
    return asioName;
}

Json meterEvent(const AudioMeterSnapshot& meter) {
    const double peakDbfs = meter.peakLinear > 0.0f
        ? std::max(-120.0, 20.0 * std::log10(static_cast<double>(meter.peakLinear)))
        : -120.0;
    return Json{{"event", "audio.meter"},
        {"payload", {{"endpointId", meter.endpointId}, {"peakDbfs", peakDbfs},
            {"shortTermLufs", meter.shortTermLufsReady ? Json(meter.shortTermLufs) : Json(nullptr)},
            {"lufsWindowSeconds", 3.0},
            {"underruns", meter.underruns}, {"overruns", meter.overruns}}}};
}

std::uint64_t fileTimeTicks(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
}

struct ProcessCpuReading {
    std::optional<double> percent;
    std::optional<std::uint64_t> windowMs;
};

class ProcessCpuMonitor {
public:
    ProcessCpuReading Read(std::uint32_t pid) {
        const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return {};
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool measured = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
        CloseHandle(process);
        if (!measured) return {};

        const auto now = std::chrono::steady_clock::now();
        const auto creationTicks = fileTimeTicks(created);
        const auto cpuTicks = fileTimeTicks(kernel) + fileTimeTicks(user);
        std::lock_guard<std::mutex> lock(mutex_);
        auto& sample = samples_[pid];
        if (sample.creationTicks != creationTicks || cpuTicks < sample.cpuTicks ||
            sample.at == std::chrono::steady_clock::time_point{}) {
            sample = {creationTicks, cpuTicks, now, {}, {}};
            return {};
        }
        const auto elapsedNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now - sample.at).count();
        if (elapsedNs < 1000000000LL) return {sample.percent, sample.windowMs};
        const auto processors = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
        if (!processors) return {};
        const auto deltaTicks = cpuTicks - sample.cpuTicks;
        sample.percent = std::clamp(100.0 * static_cast<double>(deltaTicks) * 100.0 /
            (static_cast<double>(elapsedNs) * processors), 0.0, 100.0);
        sample.windowMs = static_cast<std::uint64_t>(elapsedNs / 1000000);
        sample.cpuTicks = cpuTicks;
        sample.at = now;
        return {sample.percent, sample.windowMs};
    }

    void Retain(const std::set<std::uint32_t>& activePids) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = samples_.begin(); it != samples_.end();) {
            if (!activePids.count(it->first)) it = samples_.erase(it);
            else ++it;
        }
    }

private:
    struct Sample {
        std::uint64_t creationTicks = 0;
        std::uint64_t cpuTicks = 0;
        std::chrono::steady_clock::time_point at{};
        std::optional<double> percent;
        std::optional<std::uint64_t> windowMs;
    };
    std::mutex mutex_;
    std::unordered_map<std::uint32_t, Sample> samples_;
};
}

struct ControlApiServer::Impl {
    VasioClientManager& clients;
    AudioController& controller;
    PwarConfigurationStore pwarConfiguration;
    PwarClientSupervisor pwarClient;
    EngineDiagnostics& diagnostics;
    std::function<void()> stopRequest;
    std::vector<PhysicalAsioDriverInfo> physicalInventory;
    httplib::Server server;
    std::atomic<bool> running{false};
    bool configured = false;
    std::thread listenerThread;
    std::uint16_t port = 0;
    std::string openApiDocument;
    std::string schemaDocument;
    struct CorrelationSelection { std::string id, leftEndpointId, rightEndpointId; };
    std::mutex correlationMutex;
    std::optional<CorrelationSelection> correlationSelection;
    mutable ProcessCpuMonitor processCpu;

    Impl(VasioClientManager& manager, AudioController& audioController, EngineDiagnostics& engineDiagnostics,
         std::function<void()> requestStop)
        : clients(manager), controller(audioController), diagnostics(engineDiagnostics), stopRequest(std::move(requestStop)) {}
    ~Impl() { Stop(); }

    Json physicalDriversJson() const {
        Json result = Json::array();
        const auto state = controller.Snapshot();
        for (const auto& driver : physicalInventory) {
            const auto id = utf8(driver.id);
            const bool selected = state.state != "error" && state.physicalDriverId &&
                *state.physicalDriverId == id && state.sampleRate && state.bufferFrames;
            Json inputs = Json::array();
            Json outputs = Json::array();
            Json rates = Json::array();
            Json bufferSizes = nullptr;
            if (selected) {
                for (long channel = 1; channel <= state.physicalCapabilities.inputChannels; ++channel) {
                    const auto index = static_cast<std::size_t>(channel - 1);
                    const auto channelName = index < state.physicalCapabilities.inputChannelNames.size()
                        ? utf8(state.physicalCapabilities.inputChannelNames[index]) : std::string{};
                    inputs.push_back(endpoint("physical:" + id + ":input:" + std::to_string(channel),
                        channelName.empty() ? driver.name + " In " + std::to_string(channel)
                            : driver.name + " · " + channelName, "input", channel));
                }
                for (long channel = 1; channel <= state.physicalCapabilities.outputChannels; ++channel) {
                    const auto index = static_cast<std::size_t>(channel - 1);
                    const auto channelName = index < state.physicalCapabilities.outputChannelNames.size()
                        ? utf8(state.physicalCapabilities.outputChannelNames[index]) : std::string{};
                    const auto resolvedName = physicalOutputChannelName(driver,
                        state.physicalCapabilities, channel, channelName);
                    outputs.push_back(endpoint("physical:" + id + ":output:" + std::to_string(channel),
                        resolvedName.empty() ? driver.name + " Out " + std::to_string(channel)
                            : driver.name + " · " + resolvedName, "output", channel));
                }
                for (const auto rate : state.physicalCapabilities.supportedSampleRates)
                    rates.push_back(static_cast<std::uint32_t>(rate));
                bufferSizes = {{"minimumFrames", state.physicalCapabilities.minBufferFrames},
                    {"maximumFrames", state.physicalCapabilities.maxBufferFrames},
                    {"preferredFrames", state.physicalCapabilities.preferredBufferFrames},
                    {"granularity", state.physicalCapabilities.bufferGranularity}};
            }
            result.push_back({{"id", id}, {"name", driver.name},
                {"inputEndpoints", std::move(inputs)}, {"outputEndpoints", std::move(outputs)},
                {"sampleRates", std::move(rates)}, {"bufferSizes", std::move(bufferSizes)},
                {"capabilitiesKnown", selected}});
        }
        return result;
    }

    Json virtualDriversJson() const {
        const auto attachedClients = clients.GetClientSnapshots();
        const auto state = controller.Snapshot();
        Json clientArray = Json::array();
        Json inputs = Json::array();
        Json outputs = Json::array();
        std::set<std::uint32_t> activePids;
        for (const auto& client : attachedClients) {
            if (client.info.driverId != kVasioId || !client.mapping) continue;
            AudioClientMapping::ChannelMask activeInputs{};
            AudioClientMapping::ChannelMask activeOutputs{};
            if (!client.mapping->GetActiveChannels(activeInputs, activeOutputs)) continue;
            activePids.insert(client.info.processId);
            const auto cpu = processCpu.Read(client.info.processId);
            Json inputChannels = Json::array();
            Json outputChannels = Json::array();
            const auto prefix = "virtual:TimoxVasio:" + std::to_string(client.info.processId) + ":";
            for (std::uint32_t channel = 0; channel < AudioClientMapping::kChannelCount; ++channel) {
                const auto apiChannel = static_cast<long>(channel + 1);
                const auto mask = std::uint64_t{1} << (channel % 64);
                if (activeInputs[channel / 64] & mask) {
                    inputChannels.push_back(apiChannel);
                    inputs.push_back(endpoint(prefix + "input:" + std::to_string(apiChannel),
                        "TimoxVasio " + utf8(client.info.processName) + " In " + std::to_string(apiChannel),
                        "input", apiChannel));
                }
                if (activeOutputs[channel / 64] & mask) {
                    outputChannels.push_back(apiChannel);
                    outputs.push_back(endpoint(prefix + "output:" + std::to_string(apiChannel),
                        "TimoxVasio " + utf8(client.info.processName) + " Out " + std::to_string(apiChannel),
                        "output", apiChannel));
                }
            }
            AudioClientMapping::StatusSnapshot clientStatus;
            const bool clientStatusMapped = AudioClientMapping::ReadStatusReadOnly(
                kVasioId, client.info.processId, clientStatus);
            const bool activityAvailable = clientStatusMapped && clientStatus.available;
            const bool activityStable = activityAvailable && clientStatus.stableExchange;
            Json lastCallbackAgeMs = nullptr;
            if (activityStable && clientStatus.callbackCount && clientStatus.qpcFrequency) {
                LARGE_INTEGER now{};
                if (QueryPerformanceCounter(&now) && now.QuadPart >=
                    static_cast<LONG64>(clientStatus.lastCallbackQpc)) {
                    lastCallbackAgeMs = static_cast<std::uint64_t>(
                        (static_cast<long double>(now.QuadPart -
                            static_cast<LONG64>(clientStatus.lastCallbackQpc)) * 1000.0L) /
                        clientStatus.qpcFrequency);
                }
            }
            Json activity{
                {"available", activityAvailable},
                {"stable", activityStable},
                {"state", activityAvailable ? Json(asioClientStateName(clientStatus.state)) : Json(nullptr)},
                {"callbackCount", activityStable ? Json(clientStatus.callbackCount) : Json(nullptr)},
                {"successfulExchangeCount", activityStable ? Json(clientStatus.successfulExchangeCount) : Json(nullptr)},
                {"exchangedFrames", activityStable ? Json(clientStatus.exchangedFrames) : Json(nullptr)},
                {"lastCallbackAgeMs", std::move(lastCallbackAgeMs)},
                {"requestedSampleRate", activityAvailable ? Json(clientStatus.requestedSampleRate) : Json(nullptr)},
                {"requestedBufferFrames", activityAvailable ? Json(clientStatus.requestedBufferFrames) : Json(nullptr)}
            };
            clientArray.push_back({{"pid", client.info.processId},
                {"processName", utf8(client.info.processName)},
                {"processCpuPercent", cpu.percent ? Json(*cpu.percent) : Json(nullptr)},
                {"processCpuWindowMs", cpu.windowMs ? Json(*cpu.windowMs) : Json(nullptr)},
                {"inputChannels", std::move(inputChannels)},
                {"outputChannels", std::move(outputChannels)},
                {"activity", std::move(activity)}});
        }
        processCpu.Retain(activePids);
        Json rate = state.sampleRate ? Json(*state.sampleRate) : Json(nullptr);
        Json bufferFrames = state.bufferFrames ? Json(*state.bufferFrames) : Json(nullptr);
        return Json::array({{{"id", "TimoxVasio"}, {"channelsPerDirection", AudioClientMapping::kChannelCount},
            {"sampleRate", std::move(rate)}, {"bufferFrames", std::move(bufferFrames)},
            {"clients", std::move(clientArray)}, {"inputEndpoints", std::move(inputs)},
            {"outputEndpoints", std::move(outputs)}}});
    }

    Json driversJson() const {
        return Json{{"physicalDrivers", physicalDriversJson()}, {"virtualDrivers", virtualDriversJson()},
            {"stereoPairs", stereoPairsJson()}};
    }

    Json stereoPairsJson() const {
        Json result = Json::array();
        const auto virtualDrivers = virtualDriversJson();
        if (virtualDrivers.empty()) return result;
        std::set<std::string> routedSources;
        for (const auto& route : controller.Snapshot().routes) routedSources.insert(route.sourceEndpointId);
        for (const auto& client : virtualDrivers[0]["clients"]) {
            const auto pid = client["pid"].get<std::uint32_t>();
            const auto processName = client["processName"].get<std::string>();
            std::set<std::uint32_t> activeOutputs;
            for (const auto& channel : client["outputChannels"]) activeOutputs.insert(channel.get<std::uint32_t>());
            const auto prefix = "virtual:TimoxVasio:" + std::to_string(pid) + ":output:";
            for (std::uint32_t left = 1; left < AudioClientMapping::kChannelCount; left += 2) {
                const auto right = left + 1;
                if (!activeOutputs.count(left) || !activeOutputs.count(right)) continue;
                const auto leftId = prefix + std::to_string(left);
                const auto rightId = prefix + std::to_string(right);
                if (!routedSources.count(leftId) || !routedSources.count(rightId)) continue;
                result.push_back({{"id", "stereo:TimoxVasio:" + std::to_string(pid) + ":output:" +
                        std::to_string(left) + "-" + std::to_string(right)},
                    {"leftEndpointId", leftId}, {"rightEndpointId", rightId},
                    {"label", processName + " · Sorties " + std::to_string(left) + "–" + std::to_string(right)}});
            }
        }
        return result;
    }

    Json correlationEvent(const std::optional<CorrelationSelection>& selection) {
        if (!selection) return Json{{"event", "audio.correlation"}, {"payload", {
            {"stereoPairId", nullptr}, {"leftEndpointId", nullptr}, {"rightEndpointId", nullptr},
            {"correlation", nullptr}, {"state", "stopped"}}}};
        const auto sample = controller.ReadCorrelation();
        const bool hasSignal = sample.active && sample.hasSignal;
        return Json{{"event", "audio.correlation"}, {"payload", {
            {"stereoPairId", selection->id}, {"leftEndpointId", selection->leftEndpointId},
            {"rightEndpointId", selection->rightEndpointId},
            {"correlation", hasSignal ? Json(sample.correlation) : Json(nullptr)},
            {"state", hasSignal ? "measuring" : "no_signal"}}}};
    }

    Json stateJson() const {
        const auto state = controller.Snapshot();
        const auto bufferHealth = controller.ReadBufferHealth();
        const auto measurements = controller.ReadPerformanceMeasurements();
        const auto durationJson = [](const std::optional<PerformanceDuration>& value) -> Json {
            if (!value) return nullptr;
            return Json{{"samples", value->samples}, {"meanNs", value->meanNs},
                {"worstNs", value->worstNs}};
        };
        Json measurementWindow = nullptr;
        if (measurements.window) {
            Json clientsJson = Json::array();
            for (const auto& client : measurements.window->clients)
                clientsJson.push_back({{"processId", client.processId}, {"driverId", client.driverId},
                    {"hostCallback", durationJson(client.hostCallback)},
                    {"ringWrite", durationJson(client.ringWrite)},
                    {"ringWait", durationJson(client.ringWait)},
                    {"ringWaitDiscardedBlocks", client.ringWaitDiscardedBlocks}});
            const auto& physical = measurements.window->physical;
            measurementWindow = {{"durationMs", 5000}, {"clients", std::move(clientsJson)},
                {"physical", {{"clientRead", durationJson(physical.clientRead)},
                    {"routing", durationJson(physical.routing)},
                    {"outputCopy", durationJson(physical.outputCopy)}}}};
        }
        Json bufferHealthJson = nullptr;
        if (bufferHealth.available) {
            const char* healthState = bufferHealth.assessment.state == AsioBufferHealthState::Warning
                ? "warning" : bufferHealth.assessment.state == AsioBufferHealthState::Insufficient
                    ? "insufficient" : "healthy";
            bufferHealthJson = {{"state", healthState},
                {"sampleRate", bufferHealth.window.sampleRate},
                {"bufferFrames", bufferHealth.window.bufferFrames},
                {"windowDurationMs", bufferHealth.window.durationMs},
                {"callbackCount", bufferHealth.window.callbacks},
                {"delayedStartCount", bufferHealth.window.delayedStarts},
                {"processingOverrunCount", bufferHealth.window.processingOverruns},
                {"blockPeriodNs", bufferHealth.assessment.blockPeriodNs},
                {"worstProcessingNs", bufferHealth.window.worstProcessingNs},
                {"worstMarginNs", bufferHealth.assessment.worstMarginNs}};
        }
        Json physicalId = state.physicalDriverId ? Json(*state.physicalDriverId) : Json(nullptr);
        Json sampleRate = state.sampleRate ? Json(*state.sampleRate) : Json(nullptr);
        Json bufferFrames = state.bufferFrames ? Json(*state.bufferFrames) : Json(nullptr);
        Json lastError = state.lastError.empty() ? Json(nullptr) :
            Json{{"code", "AUDIO_CONFIGURATION_FAILED"}, {"message", state.lastError}, {"operation", "apply"}};
        Json routes = Json::array();
        for (const auto& route : state.routes) routes.push_back(routeJson(route));
        Json configuredRoutes = Json::array();
        for (const auto& route : state.configuredRoutes)
            configuredRoutes.push_back(routeJson(route));
        return Json{{"apiVersion", "1.1"}, {"configurationRevision", state.configurationRevision},
            {"capabilities", {{"wasapiProcessLoopback", supportsWasapiProcessLoopback()}}},
            {"engine", {{"state", state.state}, {"clockSource", clockSourceJson(state)},
                         {"clockCapabilities", clockCapabilitiesJson(state)}, {"physicalDriverId", std::move(physicalId)},
                         {"sampleRate", std::move(sampleRate)}, {"bufferFrames", std::move(bufferFrames)},
                         {"lastError", std::move(lastError)}}},
            {"physicalDrivers", physicalDriversJson()}, {"virtualDrivers", virtualDriversJson()},
            {"wasapiDevices", wasapiDevicesJson(state)},
            {"wasapiInventoryError", state.wasapiInventoryError.empty() ? Json(nullptr) : Json(state.wasapiInventoryError)},
            {"configuredWasapiBridges", wasapiConfigurationsJson(state.configuredWasapiBridges)},
            {"wasapiBridges", wasapiBridgesJson(state)},
            {"nativeAudio", NativeAudioApiJson(state.nativeAudio)},
            {"bufferHealth", std::move(bufferHealthJson)},
            {"performanceMeasurements", {{"enabled", measurements.enabled},
                {"sampleEveryCallbacks", 16}, {"window", std::move(measurementWindow)}}},
            {"routes", std::move(routes)}, {"configuredRoutes", std::move(configuredRoutes)},
            {"stereoPairs", stereoPairsJson()}};
    }

    Json applyCommand(const Json& request) {
        std::string id;
        if (request.is_object() && request.contains("id") && request["id"].is_string())
            id = request["id"].get<std::string>();
        if (id.empty() || id.size() > 128 || !request.is_object() ||
            !request.contains("command") || !request["command"].is_string())
            return errorEnvelope(id, "INVALID_CONFIGURATION", "Invalid request envelope", "validate");
        diagnostics.Write(EngineDiagnostics::Level::Debug, "api", "Received command " + request["command"].get<std::string>());
        if (request["command"] == "pwar.client.start") {
            if (request.size() != 3 || !request.contains("payload") || !request["payload"].is_object() ||
                request["payload"].size() != 2 || !request["payload"].contains("peerAddress") ||
                !request["payload"]["peerAddress"].is_string() || !request["payload"].contains("maxQueueMs") ||
                !(request["payload"]["maxQueueMs"].is_number_integer() ||
                  request["payload"]["maxQueueMs"].is_number_unsigned()) ||
                request["payload"]["maxQueueMs"].get<std::uint64_t>() < 1 ||
                request["payload"]["maxQueueMs"].get<std::uint64_t>() > 2000)
                return errorEnvelope(id, "INVALID_PWAR_CLIENT_CONFIGURATION",
                    "Expected payload.peerAddress and payload.maxQueueMs", "validate");
            if (controller.Snapshot().state != "running")
                return errorEnvelope(id, "AUDIO_ENGINE_NOT_RUNNING",
                    "Start the Timox audio engine before starting the PWAR ASIO client", "start");
            std::string error;
            if (!pwarClient.Start(request["payload"]["peerAddress"].get<std::string>(),
                    request["payload"]["maxQueueMs"].get<std::uint32_t>(), error))
                return errorEnvelope(id, "PWAR_CLIENT_START_FAILED", error, "start");
            diagnostics.Write(EngineDiagnostics::Level::Info, "pwar-client", "Supervised PWAR ASIO client started");
            return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}, {"client", pwarClient.Snapshot()}}}};
        }
        if (request["command"] == "pwar.client.stop") {
            if (request.size() != 2)
                return errorEnvelope(id, "INVALID_PWAR_CLIENT_COMMAND", "pwar.client.stop accepts only id and command", "validate");
            std::string error;
            if (!pwarClient.Stop(error))
                return errorEnvelope(id, "PWAR_CLIENT_STOP_FAILED", error, "stop");
            diagnostics.Write(EngineDiagnostics::Level::Info, "pwar-client", "Supervised PWAR ASIO client stopped");
            return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}, {"client", pwarClient.Snapshot()}}}};
        }
        if (request["command"] == "performance.measurements.set") {
            if (request.size() != 3 || !request.contains("payload") ||
                !request["payload"].is_object() || request["payload"].size() != 1 ||
                !request["payload"].contains("enabled") ||
                !request["payload"]["enabled"].is_boolean())
                return errorEnvelope(id, "INVALID_COMMAND", "Expected payload.enabled boolean", "validate");
            const auto result = controller.SetPerformanceMeasurementsEnabled(
                request["payload"]["enabled"].get<bool>());
            if (!result.success)
                return errorEnvelope(id, "AUDIO_ENGINE_ERROR", result.error, "apply");
            return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}}}};
        }
        if (request["command"] == "engine.start" || request["command"] == "engine.stop" ||
            request["command"] == "engine.reset") {
            if (request.size() != 2)
                return errorEnvelope(id, "INVALID_ENGINE_COMMAND", "Engine lifecycle commands accept only id and command", "validate");
            if (request["command"] == "engine.stop" && !clients.GetClientSnapshots().empty())
                return errorEnvelope(id, "ENGINE_CLIENTS_CONNECTED", "Stop TimoxVasio clients before stopping audio", "stop");
            if (request["command"] == "engine.stop") {
                { std::lock_guard<std::mutex> lock(correlationMutex); correlationSelection.reset(); }
                controller.StopCorrelation();
            }
            const auto result = request["command"] == "engine.start" ? controller.StartAudio()
                : request["command"] == "engine.reset" ? controller.ResetAudio() : controller.StopAudio();
            if (!result.success)
                return errorEnvelope(id, request["command"] == "engine.reset" ? "AUDIO_ENGINE_RESET_FAILED" :
                    request["command"] == "engine.start" ? "AUDIO_ENGINE_START_FAILED" : "AUDIO_ENGINE_STOP_FAILED", result.error,
                    request["command"] == "engine.reset" ? "reset" :
                        request["command"] == "engine.start" ? "start" : "stop");
            diagnostics.Write(EngineDiagnostics::Level::Info, "engine",
                request["command"] == "engine.reset" ? "Audio driver reset requested through the API" :
                    request["command"] == "engine.start" ? "Audio engine start requested through the API" :
                        "Audio engine stop requested through the API");
            return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}}}};
        }
        if (request["command"] == "audio.correlation.start") {
            if (request.size() != 3 || !request.contains("payload") || !request["payload"].is_object() ||
                request["payload"].size() != 1 || !request["payload"].contains("stereoPairId") || !request["payload"]["stereoPairId"].is_string())
                return errorEnvelope(id, "INVALID_COMMAND", "Expected payload.stereoPairId", "validate");
            const auto pairId = request["payload"]["stereoPairId"].get<std::string>();
            const auto pairs = stereoPairsJson();
            const auto pair = std::find_if(pairs.begin(), pairs.end(), [&pairId](const Json& item) { return item["id"] == pairId; });
            if (pair == pairs.end()) return errorEnvelope(id, "UNKNOWN_STEREO_PAIR", "Stereo pair is not active and routed", "validate");
            CorrelationSelection selection{pairId, (*pair)["leftEndpointId"], (*pair)["rightEndpointId"]};
            const auto result = controller.StartCorrelation(selection.leftEndpointId, selection.rightEndpointId);
            if (!result.success) return errorEnvelope(id, "AUDIO_CORRELATION_START_FAILED", result.error, "start");
            { std::lock_guard<std::mutex> lock(correlationMutex); correlationSelection = std::move(selection); }
            return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}}}};
        }
        if (request["command"] == "audio.correlation.stop") {
            if (request.size() != 2) return errorEnvelope(id, "INVALID_COMMAND", "audio.correlation.stop accepts only id and command", "validate");
            { std::lock_guard<std::mutex> lock(correlationMutex); correlationSelection.reset(); }
            controller.StopCorrelation();
            return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}}}};
        }
        if (request["command"] != "configuration.apply")
            return errorEnvelope(id, "INVALID_CONFIGURATION", "Unsupported command", "validate");
        if (!request.contains("payload") || !request["payload"].is_object())
            return errorEnvelope(id, "INVALID_CONFIGURATION", "Payload must be an object", "validate");
        const auto& payload = request["payload"];
        if (!payload.contains("physicalDriverId") ||
            !(payload["physicalDriverId"].is_null() || payload["physicalDriverId"].is_string()) ||
            !payload.contains("sampleRate") || !(payload["sampleRate"].is_null() ||
                payload["sampleRate"].is_number_integer()) ||
            !payload.contains("bufferFrames") || !(payload["bufferFrames"].is_null() ||
                payload["bufferFrames"].is_number_integer()) ||
            !payload.contains("routes") || !payload["routes"].is_array())
            return errorEnvelope(id, "INVALID_CONFIGURATION",
                "Payload must contain the physical selection, sample rate, buffer size and routes", "validate");
        for (auto field = payload.begin(); field != payload.end(); ++field) {
            if (field.key() != "physicalDriverId" && field.key() != "sampleRate" &&
                field.key() != "bufferFrames" && field.key() != "routes" &&
                field.key() != "expectedConfigurationRevision" && field.key() != "clockSource" && field.key() != "wasapiBridges")
                return errorEnvelope(id, "INVALID_CONFIGURATION", "Unexpected configuration field", "validate");
        }
        std::optional<std::uint64_t> expectedConfigurationRevision;
        if (payload.contains("expectedConfigurationRevision")) {
            if (!payload["expectedConfigurationRevision"].is_number_unsigned())
                return errorEnvelope(id, "INVALID_CONFIGURATION",
                    "expectedConfigurationRevision must be a nonnegative integer", "validate");
            expectedConfigurationRevision = payload["expectedConfigurationRevision"].get<std::uint64_t>();
        }
        AudioControllerConfiguration configuration;
        if (payload.contains("wasapiBridges") && !parseWasapiConfigurations(payload["wasapiBridges"], configuration.wasapiBridges))
            return errorEnvelope(id, "INVALID_CONFIGURATION", "Invalid WASAPI bridge configuration", "validate");
        if (payload.contains("clockSource")) {
            const auto& source = payload["clockSource"];
            if (!source.is_object() || source.size() != 1 || !source.contains("kind") ||
                !source["kind"].is_string() || (source["kind"] != "asio" && source["kind"] != "software"))
                return errorEnvelope(id, "INVALID_CONFIGURATION", "Invalid clockSource kind", "validate");
            configuration.clockSource = source["kind"] == "software" ? AudioClockSource::Software : AudioClockSource::Asio;
        }
        if (payload["physicalDriverId"].is_string())
            configuration.physicalDriverId = payload["physicalDriverId"].get<std::string>();
        if (!payload["sampleRate"].is_null()) {
            const auto rate = payload["sampleRate"].get<std::int64_t>();
            if (rate <= 0 || rate > 768000)
                return errorEnvelope(id, "INVALID_CONFIGURATION", "Invalid physical sample rate", "validate");
            configuration.sampleRate = static_cast<std::uint32_t>(rate);
        }
        if (!payload["bufferFrames"].is_null()) {
            const auto frames = payload["bufferFrames"].get<std::int64_t>();
            if (frames <= 0 || frames > 65536)
                return errorEnvelope(id, "INVALID_CONFIGURATION", "Invalid physical buffer size", "validate");
            configuration.bufferFrames = static_cast<std::uint32_t>(frames);
        }

        configuration.routes.reserve(payload["routes"].size());
        for (const auto& item : payload["routes"]) {
            if (!item.is_object() || item.size() != 5 || !item.contains("id") || !item["id"].is_string() ||
                !item.contains("sourceEndpointId") || !item["sourceEndpointId"].is_string() ||
                !item.contains("destinationEndpointId") || !item["destinationEndpointId"].is_string() ||
                !item.contains("gainDb") || !item["gainDb"].is_number() ||
                !item.contains("mute") || !item["mute"].is_boolean())
                return errorEnvelope(id, "INVALID_CONFIGURATION", "Invalid route entry", "validate");
            configuration.routes.push_back({item["id"].get<std::string>(),
                item["sourceEndpointId"].get<std::string>(),
                item["destinationEndpointId"].get<std::string>(),
                item["gainDb"].get<double>(), item["mute"].get<bool>()});
        }

        std::unordered_map<std::string, RoutingEndpointType> endpointTypes;
        const auto inventory = driversJson();
        for (const auto& driver : inventory["physicalDrivers"]) {
            for (const auto& item : driver["inputEndpoints"])
                endpointTypes.emplace(item["id"].get<std::string>(), RoutingEndpointType::PhysicalInput);
            for (const auto& item : driver["outputEndpoints"])
                endpointTypes.emplace(item["id"].get<std::string>(), RoutingEndpointType::PhysicalOutput);
        }
        for (const auto& driver : inventory["virtualDrivers"]) {
            for (const auto& item : driver["inputEndpoints"])
                endpointTypes.emplace(item["id"].get<std::string>(), RoutingEndpointType::VirtualInput);
            for (const auto& item : driver["outputEndpoints"])
                endpointTypes.emplace(item["id"].get<std::string>(), RoutingEndpointType::VirtualOutput);
        }
        // An existing saved application route remains editable while that client
        // is disconnected. Only IDs already present in the confirmed configuration
        // are accepted outside the current endpoint inventory.
        const auto bridgeState = controller.Snapshot();
        const auto& savedRoutes = bridgeState.configuredRoutes;
        const auto& declaredBridges = payload.contains("wasapiBridges") ? configuration.wasapiBridges : bridgeState.configuredWasapiBridges;
        for (const auto& bridge : declaredBridges) {
            std::size_t channels = 0;
            const auto effective = std::find_if(bridgeState.wasapiBridges.begin(), bridgeState.wasapiBridges.end(),
                [&](const auto& b) { return b.id == bridge.id; });
            const auto saved = std::find_if(bridgeState.configuredWasapiBridges.begin(), bridgeState.configuredWasapiBridges.end(),
                [&](const auto& b) { return b.id == bridge.id; });
            if (saved != bridgeState.configuredWasapiBridges.end() && effective != bridgeState.wasapiBridges.end() &&
                effective->effectiveInfo && saved->open.kind == bridge.open.kind &&
                saved->open.endpointId == bridge.open.endpointId && saved->open.defaultRole == bridge.open.defaultRole)
                channels = effective->effectiveInfo->format.channels;
            if (!bridge.open.endpointId.empty()) {
                for (const auto& device : bridgeState.wasapiDevices)
                    if (device.id == bridge.open.endpointId && device.mixFormat &&
                        device.flow == (bridge.open.kind == WasapiStreamKind::Capture ? WasapiDataFlow::Capture : WasapiDataFlow::Render))
                        channels = device.mixFormat->channels;
            }
            const bool render = bridge.open.kind == WasapiStreamKind::Render;
            for (std::size_t channel = 1; channel <= channels && channel <= 256; ++channel)
                endpointTypes.emplace("wasapi:" + bridge.id + (render ? ":output:" : ":input:") + std::to_string(channel),
                    render ? RoutingEndpointType::WasapiRender : RoutingEndpointType::WasapiCapture);
        }
        for (const auto& saved : savedRoutes) {
            for (const auto& endpoint : {saved.sourceEndpointId, saved.destinationEndpointId}) {
                for (const auto& bridge : declaredBridges) {
                    const bool render = bridge.open.kind == WasapiStreamKind::Render;
                    const auto prefix = "wasapi:" + bridge.id + (render ? ":output:" : ":input:");
                    if (endpoint.rfind(prefix, 0) == 0)
                        endpointTypes.emplace(endpoint, render ? RoutingEndpointType::WasapiRender : RoutingEndpointType::WasapiCapture);
                }
                // Previously validated hardware routes remain editable when the
                // device is absent or the graph uses the software clock.
                if (endpoint.rfind("physical:", 0) == 0) {
                    if (endpoint.find(":input:") != std::string::npos)
                        endpointTypes.emplace(endpoint, RoutingEndpointType::PhysicalInput);
                    else if (endpoint.find(":output:") != std::string::npos)
                        endpointTypes.emplace(endpoint, RoutingEndpointType::PhysicalOutput);
                    continue;
                }
                if (endpoint.rfind("virtual:TimoxVasio:app:", 0) != 0) continue;
                if (endpoint.find(":input:") != std::string::npos)
                    endpointTypes.emplace(endpoint, RoutingEndpointType::VirtualInput);
                else if (endpoint.find(":output:") != std::string::npos)
                    endpointTypes.emplace(endpoint, RoutingEndpointType::VirtualOutput);
            }
        }
        const auto profiles = controller.GetApplicationProfiles().profiles;
        const auto attachedClients = clients.GetClientSnapshots();
        for (const auto& route : configuration.routes) {
            for (const auto& endpoint : {route.sourceEndpointId, route.destinationEndpointId}) {
                if (endpointTypes.find(endpoint) != endpointTypes.end()) continue;
                const auto type = PersistentRoutes::KnownApplicationEndpointType(
                    endpoint, profiles, attachedClients, savedRoutes);
                if (type) endpointTypes.emplace(endpoint, *type);
            }
            const auto source = endpointTypes.find(route.sourceEndpointId);
            const auto destination = endpointTypes.find(route.destinationEndpointId);
            if (source == endpointTypes.end() || destination == endpointTypes.end()) {
                const auto& missing = source == endpointTypes.end()
                    ? route.sourceEndpointId : route.destinationEndpointId;
                return errorEnvelope(id, "UNKNOWN_ENDPOINT",
                    "Connection " + route.id + " references an unavailable port: " + missing,
                    "validate");
            }
            if (!RoutingGraph::SupportsLink(source->second, destination->second))
                return errorEnvelope(id, "INVALID_ROUTE_DIRECTION",
                    "The route direction is not supported", "validate");
        }

        { std::lock_guard<std::mutex> lock(correlationMutex); correlationSelection.reset(); }
        controller.StopCorrelation();
        const auto applied = controller.ApplyConfiguration(configuration, expectedConfigurationRevision,
            !payload.contains("clockSource"), !payload.contains("wasapiBridges"));
        if (!applied.success) {
            diagnostics.Write(applied.conflict ? EngineDiagnostics::Level::Info :
                EngineDiagnostics::Level::Error, "audio", applied.error);
            return errorEnvelope(id, applied.conflict ? "CONFIGURATION_CONFLICT" :
                "AUDIO_CONFIGURATION_FAILED", applied.error, "apply");
        }
        diagnostics.Write(EngineDiagnostics::Level::Info, "audio", "Audio configuration applied with " + std::to_string(configuration.routes.size()) + " routes");
        const auto confirmed = controller.Snapshot();
        if (confirmed.physicalDriverId && confirmed.sampleRate && confirmed.bufferFrames)
            diagnostics.Write(EngineDiagnostics::Level::Info, "audio",
                "Physical driver " + *confirmed.physicalDriverId + " configured at " +
                std::to_string(*confirmed.sampleRate) + " Hz, " +
                std::to_string(*confirmed.bufferFrames) + " frames");
        return Json{{"id", id}, {"success", true}, {"result", {{"accepted", true}}}};
    }

    void configureApplicationProfileRoutes() {
        server.Get("/api/v1/application-profiles", [this](const httplib::Request& request,
                                                           httplib::Response& response) {
            setCors(request, response);
            const auto snapshot = controller.GetApplicationProfiles();
            if (!snapshot.error.empty()) {
                response.status = 500;
                response.set_content(Json{{"code", "APPLICATION_PROFILE_STORE_FAILED"},
                    {"message", snapshot.error}}.dump(), "application/json; charset=utf-8");
                return;
            }
            response.set_content(applicationProfilesJson(snapshot.profiles).dump(),
                "application/json; charset=utf-8");
        });
        server.Put("/api/v1/application-profiles", [this](const httplib::Request& request,
                                                           httplib::Response& response) {
            setCors(request, response);
            try {
                const auto body = Json::parse(request.body);
                if (!body.is_object() || body.size() != 1 || !body.contains("profiles") ||
                    !body["profiles"].is_array() || body["profiles"].size() > 1024) {
                    response.status = 400;
                    response.set_content(Json{{"code", "INVALID_APPLICATION_PROFILES"},
                        {"message", "Expected an object containing at most 1024 application profiles"}}.dump(),
                        "application/json; charset=utf-8");
                    return;
                }
                std::vector<ApplicationProfile> profiles;
                profiles.reserve(body["profiles"].size());
                for (const auto& item : body["profiles"]) {
                    if (!item.is_object() || item.size() != 3 || !item.contains("processName") ||
                        !item["processName"].is_string() || !item.contains("inputChannels") ||
                        !item["inputChannels"].is_number_integer() || !item.contains("outputChannels") ||
                        !item["outputChannels"].is_number_integer()) {
                        response.status = 400;
                        response.set_content(Json{{"code", "INVALID_APPLICATION_PROFILES"},
                            {"message", "Each profile requires processName, inputChannels and outputChannels"}}.dump(),
                            "application/json; charset=utf-8");
                        return;
                    }
                    const auto inputChannels = item["inputChannels"].get<std::int64_t>();
                    const auto outputChannels = item["outputChannels"].get<std::int64_t>();
                    std::wstring processName;
                    if (inputChannels < 1 || inputChannels > 256 || outputChannels < 1 ||
                        outputChannels > 256 || !wide(item["processName"].get<std::string>(), processName)) {
                        response.status = 400;
                        response.set_content(Json{{"code", "INVALID_APPLICATION_PROFILES"},
                            {"message", "Profile names must be UTF-8 executable basenames and channel counts must be 1 to 256"}}.dump(),
                            "application/json; charset=utf-8");
                        return;
                    }
                    profiles.push_back({std::move(processName),
                        static_cast<std::uint32_t>(inputChannels),
                        static_cast<std::uint32_t>(outputChannels)});
                }
                std::string validationError;
                if (!ApplicationProfileStore::NormalizeAndValidate(profiles, validationError)) {
                    response.status = 400;
                    response.set_content(Json{{"code", "INVALID_APPLICATION_PROFILES"},
                        {"message", validationError}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                const auto updated = controller.ReplaceApplicationProfiles(std::move(profiles));
                if (!updated.success) {
                    response.status = 500;
                    response.set_content(Json{{"code", "APPLICATION_PROFILE_STORE_FAILED"},
                        {"message", updated.error}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                Json clientsRequiringRestart = Json::array();
                for (const auto& client : updated.restartRequiredClients) {
                    clientsRequiringRestart.push_back({{"pid", client.processId},
                        {"processName", utf8(client.processName)}});
                }
                Json result = applicationProfilesJson(updated.profiles);
                result["restartRequiredClients"] = std::move(clientsRequiringRestart);
                response.set_content(result.dump(), "application/json; charset=utf-8");
            } catch (const Json::exception&) {
                response.status = 400;
                response.set_content(Json{{"code", "INVALID_APPLICATION_PROFILES"},
                    {"message", "Request body is not valid JSON"}}.dump(), "application/json; charset=utf-8");
            }
        });
    }

    void configureRoutes() {
        if (configured) return;
        configured = true;
        server.set_payload_max_length(1024 * 1024);
        server.Get("/api/v1/pwar", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            PwarSettings settings; std::string error;
            if (!pwarConfiguration.Load(settings, error)) {
                response.status = 500;
                response.set_content(Json{{"code", "PWAR_CONFIGURATION_FAILED"}, {"message", error}}.dump(), "application/json; charset=utf-8");
                return;
            }
            response.set_content(Json{{"address", settings.address}, {"port", settings.port}, {"exists", settings.exists}}.dump(), "application/json; charset=utf-8");
        });
        server.Put("/api/v1/pwar", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            try {
                const auto body = Json::parse(request.body);
                if (!body.is_object() || body.size() != 2 || !body.contains("address") || !body["address"].is_string() ||
                    !body.contains("port") || !body["port"].is_number_integer()) throw std::invalid_argument("Invalid PWAR settings");
                const auto port = body["port"].get<std::int64_t>();
                if (port < 1 || port > 65535) throw std::invalid_argument("Invalid PWAR port");
                const PwarSettings settings{body["address"].get<std::string>(), static_cast<int>(port), true};
                if (!PwarConfigurationStore::Valid(settings)) throw std::invalid_argument("Invalid PWAR address");
                std::string error;
                if (!pwarConfiguration.Save(settings, error)) {
                    response.status = 500;
                    response.set_content(Json{{"code", "PWAR_CONFIGURATION_FAILED"}, {"message", error}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                response.set_content(Json{{"address", settings.address}, {"port", settings.port}, {"exists", true}, {"reopenRequired", true}}.dump(), "application/json; charset=utf-8");
            } catch (const std::exception&) {
                response.status = 400;
                response.set_content(Json{{"code", "INVALID_PWAR_CONFIGURATION"}, {"message", "Expected an IPv4 address and an integer port from 1 to 65535"}}.dump(), "application/json; charset=utf-8");
            }
        });
        server.Get("/api/v1/pwar/client", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            response.set_content(Json{{"client", pwarClient.Snapshot()}}.dump(), "application/json; charset=utf-8");
        });
        server.Get("/api/v1/state", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            response.set_content(stateJson().dump(), "application/json; charset=utf-8");
        });
        server.Get("/api/v1/drivers", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            response.set_content(driversJson().dump(), "application/json; charset=utf-8");
        });
        server.Get("/api/v1/wasapi-devices", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            controller.EnumerateWasapiDevices();
            const auto state = controller.Snapshot();
            response.set_content(Json{{"wasapiDevices", wasapiDevicesJson(state)},
                {"wasapiInventoryError", state.wasapiInventoryError.empty() ? Json(nullptr) : Json(state.wasapiInventoryError)}}.dump(),
                "application/json; charset=utf-8");
        });
        server.Get("/api/v1/windows-audio-applications", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            controller.EnumerateWasapiProcesses();
            const auto state = controller.Snapshot();
            response.set_content(Json{{"windowsAudioApplications", windowsAudioApplicationsJson(state)},
                {"windowsAudioApplicationInventoryError", state.wasapiProcessInventoryError.empty() ? Json(nullptr) : Json(state.wasapiProcessInventoryError)}}.dump(),
                "application/json; charset=utf-8");
        });
        server.Get("/api/v1/openapi.json", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            response.set_content(openApiDocument, "application/json; charset=utf-8");
        });
        server.Get("/api/v1/schemas/api-v1.json", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            response.set_content(schemaDocument, "application/json; charset=utf-8");
        });
        server.Get("/api/v1/diagnostics", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            std::size_t limit = 200;
            if (request.has_param("limit")) {
                try {
                    const auto raw = request.get_param_value("limit");
                    std::size_t consumed = 0;
                    const auto parsed = std::stoul(raw, &consumed);
                    if (consumed != raw.size() || parsed < 1 || parsed > 500) throw std::invalid_argument("limit");
                    limit = parsed;
                } catch (...) {
                    response.status = 400;
                    response.set_content(Json{{"code", "INVALID_LIMIT"}, {"message", "limit must be between 1 and 500"}}.dump(), "application/json; charset=utf-8");
                    return;
                }
            }
            Json entries = Json::array();
            for (const auto& entry : diagnostics.ReadRecent(limit))
                entries.push_back({{"timestamp", entry.timestamp}, {"level", entry.level},
                    {"component", entry.component}, {"message", entry.message}});
            response.set_content(Json{{"level", EngineDiagnostics::LevelName(diagnostics.GetLevel())},
                {"entries", std::move(entries)}}.dump(), "application/json; charset=utf-8");
        });
        server.Put("/api/v1/diagnostics", [this](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            try {
                const auto body = Json::parse(request.body);
                EngineDiagnostics::Level level{};
                if (!body.is_object() || body.size() != 1 || !body.contains("level") ||
                    !body["level"].is_string() || !EngineDiagnostics::ParseLevel(body["level"].get<std::string>(), level)) {
                    response.status = 400;
                    response.set_content(Json{{"code", "INVALID_DIAGNOSTIC_LEVEL"}, {"message", "level must be info or debug"}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                if (level != EngineDiagnostics::Level::Info && level != EngineDiagnostics::Level::Debug) {
                    response.status = 400;
                    response.set_content(Json{{"code", "INVALID_DIAGNOSTIC_LEVEL"}, {"message", "level must be info or debug"}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                std::string error;
                if (!diagnostics.SetLevel(level, error)) {
                    response.status = 500;
                    response.set_content(Json{{"code", "DIAGNOSTICS_STORE_FAILED"}, {"message", error}}.dump(), "application/json; charset=utf-8");
                    return;
                }
                diagnostics.Write(EngineDiagnostics::Level::Info, "diagnostics", "Log level changed to " + std::string(EngineDiagnostics::LevelName(level)));
                response.set_content(Json{{"level", EngineDiagnostics::LevelName(level)}}.dump(), "application/json; charset=utf-8");
            } catch (const Json::exception&) {
                response.status = 400;
                response.set_content(Json{{"code", "INVALID_DIAGNOSTIC_LEVEL"}, {"message", "Request body is not valid JSON"}}.dump(), "application/json; charset=utf-8");
            }
        });
        configureApplicationProfileRoutes();
        const auto optionsHandler = [](const httplib::Request& request, httplib::Response& response) {
            setCors(request, response);
            response.set_header("Access-Control-Allow-Methods", "GET, PUT, OPTIONS");
            response.set_header("Access-Control-Allow-Headers", "Content-Type");
            response.status = 204;
        };
        server.Options("/api/v1/state", optionsHandler);
        server.Options("/api/v1/drivers", optionsHandler);
        server.Options("/api/v1/openapi.json", optionsHandler);
        server.Options("/api/v1/schemas/api-v1.json", optionsHandler);
        server.Options("/api/v1/application-profiles", optionsHandler);
        server.Options("/api/v1/pwar", optionsHandler);
        server.Options("/api/v1/diagnostics", optionsHandler);
        server.set_pre_request_handler([](const httplib::Request& request, httplib::Response& response) {
            const auto origin = request.get_header_value("Origin");
            if (!validOrigin(origin)) {
                response.status = 403;
                return httplib::Server::HandlerResponse::Handled;
            }
            if (request.matched_route == "/api/v1/ws" && !offersSubprotocol(request)) {
                response.status = 400;
                response.set_content("Missing WebSocket subprotocol vasio.api.v1", "text/plain; charset=utf-8");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });
        server.WebSocket("/api/v1/ws",
            [this](const httplib::Request&, httplib::ws::WebSocket& websocket) {
                websocket.set_read_timeout(std::chrono::milliseconds(100));
                auto initialState = controller.Snapshot();
                websocket.send(statusEvent(initialState).dump());
                auto lastRevision = initialState.revision;
                auto lastPwarRevision = pwarClient.Revision();
                websocket.send(Json{{"event", "pwar.client.status"},
                    {"payload", pwarClient.Snapshot()}}.dump());
                auto lastMeterTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(500);
                auto lastCorrelationTime = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
                const auto sendAudioMeters = [&] {
                    for (const auto& meter : controller.ReadMeters())
                        if (!websocket.send(meterEvent(meter).dump())) return false;
                    return true;
                };
                const auto sendStatusChanges = [&] {
                    for (const auto& change : controller.EventsSince(lastRevision)) {
                        if (!websocket.send(statusEvent(change).dump())) return false;
                        lastRevision = change.revision;
                    }
                    const auto pwarRevision = pwarClient.Revision();
                    if (pwarRevision != lastPwarRevision) {
                        if (!websocket.send(Json{{"event", "pwar.client.status"},
                                {"payload", pwarClient.Snapshot()}}.dump())) return false;
                        lastPwarRevision = pwarRevision;
                    }
                    return true;
                };
                auto devices = driversJson();
                auto lastDevices = devices.dump();
                websocket.send(Json{{"event", "devices.changed"}, {"payload", devices}}.dump());
                websocket.send(Json{{"event", "audio.stereoPairs.changed"},
                    {"payload", {{"stereoPairs", stereoPairsJson()}}}}.dump());
                while (websocket.is_open()) {
                    std::string message;
                    const auto result = websocket.read(message);
                    if (result == httplib::ws::Timeout) {
                        if (!sendStatusChanges()) break;
                        const auto now = std::chrono::steady_clock::now();
                        if (now - lastMeterTime >= std::chrono::milliseconds(500)) {
                            lastMeterTime = now;
                            if (!sendAudioMeters()) break;
                        }
                        devices = driversJson();
                        const auto currentDevices = devices.dump();
                        if (currentDevices != lastDevices) {
                            lastDevices = currentDevices;
                            if (!websocket.send(Json{{"event", "devices.changed"},
                                {"payload", devices}}.dump())) break;
                            if (!websocket.send(Json{{"event", "audio.stereoPairs.changed"},
                                {"payload", {{"stereoPairs", devices["stereoPairs"]}}}}.dump())) break;
                        }
                        if (now - lastCorrelationTime >= std::chrono::milliseconds(100)) {
                            lastCorrelationTime = now;
                            std::optional<CorrelationSelection> selection;
                            { std::lock_guard<std::mutex> lock(correlationMutex); selection = correlationSelection; }
                            if (selection) {
                                const auto pairs = stereoPairsJson();
                                const bool stillAvailable = std::any_of(pairs.begin(), pairs.end(), [&selection](const Json& item) {
                                    return item["id"] == selection->id;
                                });
                                if (!stillAvailable) {
                                    controller.StopCorrelation();
                                    { std::lock_guard<std::mutex> lock(correlationMutex); correlationSelection.reset(); }
                                    selection.reset();
                                }
                                if (!websocket.send(correlationEvent(selection).dump())) break;
                            }
                        }
                        continue;
                    }
                    if (result != httplib::ws::Text) break;
                    Json request;
                    Json reply;
                    try {
                        request = Json::parse(message);
                        auto pending = std::async(std::launch::async,
                            [this, request] { return applyCommand(request); });
                        while (pending.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
                            if (!sendStatusChanges()) return;
                        }
                        reply = pending.get();
                    }
                    catch (const Json::exception&) {
                        reply = errorEnvelope("", "INVALID_CONFIGURATION", "Malformed JSON command", "parse");
                    }
                    if (!websocket.send(reply.dump())) break;
                    if (!sendStatusChanges()) break;
                    if (reply.is_object() && reply.value("success", false)) {
                        const auto command = request.is_object()
                            ? request.value("command", std::string{}) : std::string{};
                        if (command == "configuration.apply") {
                            Json routes = stateJson()["routes"];
                            if (!websocket.send(Json{{"event", "routes.changed"},
                                {"payload", {{"routes", std::move(routes)}}}}.dump())) break;
                            if (!websocket.send(Json{{"event", "audio.stereoPairs.changed"},
                                {"payload", {{"stereoPairs", stereoPairsJson()}}}}.dump())) break;
                            if (!websocket.send(correlationEvent(std::nullopt).dump())) break;
                        } else if (command == "audio.correlation.start" || command == "audio.correlation.stop" || command == "engine.stop") {
                            std::optional<CorrelationSelection> selection;
                            { std::lock_guard<std::mutex> lock(correlationMutex); selection = correlationSelection; }
                            if (!websocket.send(correlationEvent(selection).dump())) break;
                        }
                    } else if (controller.Snapshot().state == "error") {
                        const auto state = controller.Snapshot();
                        if (!websocket.send(Json{{"event", "engine.error"},
                            {"payload", {{"code", "AUDIO_CONFIGURATION_FAILED"},
                                {"message", state.lastError}, {"operation", "apply"}}}}.dump())) break;
                    }
                }
            },
            [](const std::vector<std::string>& protocols) {
                return std::find(protocols.begin(), protocols.end(), kSubprotocol) == protocols.end()
                    ? std::string{} : std::string{kSubprotocol};
            });
    }

    bool Start(std::uint16_t requestedPort, std::uint16_t& actualPort, std::string& error) {
        if (running.load()) { actualPort = port; return true; }
        wchar_t modulePath[MAX_PATH]{};
        const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
        if (moduleLength == 0 || moduleLength >= MAX_PATH) {
            error = "Unable to locate local OpenAPI resources";
            return false;
        }
        const auto moduleDirectory = std::filesystem::path(modulePath).parent_path();
        const auto readResource = [&error](const std::filesystem::path& path, std::string& content) {
            std::ifstream file(path, std::ios::binary);
            if (!file) { error = "Required API document is missing: " + path.string(); return false; }
            content.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
            return !content.empty();
        };
        if (!readResource(moduleDirectory / L"openapi-v1.json", openApiDocument) ||
            !readResource(moduleDirectory / L"schemas" / L"api-v1.json", schemaDocument)) return false;
        configureRoutes();
        try { physicalInventory = controller.EnumeratePhysicalDrivers(); controller.EnumerateWasapiDevices(); }
        catch (const std::exception& exception) { error = exception.what(); return false; }
        const int selectedPort = requestedPort == 0
            ? server.bind_to_any_port("127.0.0.1")
            : (server.bind_to_port("127.0.0.1", requestedPort) ? requestedPort : -1);
        if (selectedPort < 0) {
            error = "cpp-httplib could not bind the local API port " + std::to_string(requestedPort);
            return false;
        }
        port = static_cast<std::uint16_t>(selectedPort);
        running.store(true);
        try {
            listenerThread = std::thread([this] {
                if (!server.listen_after_bind()) running.store(false);
            });
        } catch (...) {
            running.store(false);
            server.stop();
            error = "Unable to start the cpp-httplib listener thread";
            return false;
        }
        actualPort = port;
        return true;
    }

    void Stop() noexcept {
        if (!running.exchange(false) && !listenerThread.joinable()) return;
        server.stop();
        if (listenerThread.joinable() && listenerThread.get_id() != std::this_thread::get_id())
            listenerThread.join();
        port = 0;
    }
};

ControlApiServer::ControlApiServer(VasioClientManager& clients, AudioController& controller, EngineDiagnostics& diagnostics,
    std::function<void()> stopRequest)
    : impl_(std::make_unique<Impl>(clients, controller, diagnostics, std::move(stopRequest))) {}
ControlApiServer::~ControlApiServer() = default;

bool ControlApiServer::Start(std::uint16_t requestedPort, std::uint16_t& boundPort, std::string& error) {
    return impl_->Start(requestedPort, boundPort, error);
}

void ControlApiServer::Stop() noexcept {
    if (impl_) impl_->Stop();
}
