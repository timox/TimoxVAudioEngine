#pragma once

#include "routing_graph.h"
#include "application_profile.h"
#include "vasio_client_manager.h"
#include "audio_routing_runtime.h"
#include "native_audio_backend.h"

#include <windows.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

// API endpoint IDs identify a running process. Stored routes identify its executable,
// so a route can wait for its client and survive a change of PID.
namespace PersistentRoutes {
namespace Detail {
constexpr char kPrefix[] = "virtual:TimoxVasio:";
constexpr char kAppPrefix[] = "virtual:TimoxVasio:app:";

inline std::string executableName(const std::wstring& name) {
    if (name.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
        static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string encoded(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(),
            static_cast<int>(name.size()), encoded.data(), length, nullptr, nullptr)) return {};
    std::transform(encoded.begin(), encoded.end(), encoded.begin(),
        [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return encoded;
}

inline bool parseChannel(const std::string& value, std::uint32_t& channel) {
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), channel);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size() &&
        channel >= 1 && channel <= AudioClientMapping::kChannelCount;
}

inline bool active(const VasioClientSnapshot& client, const std::string& direction,
                   std::uint32_t channel) {
    if (!client.mapping || !client.mapping->EngineAttached()) return false;
    AudioClientMapping::ChannelMask inputs{}, outputs{};
    if (!client.mapping->GetActiveChannels(inputs, outputs)) return false;
    const auto& mask = direction == "input" ? inputs : outputs;
    const auto zeroBased = channel - 1;
    return (mask[zeroBased / 64] & (std::uint64_t{1} << (zeroBased % 64))) != 0;
}

inline std::optional<std::string> encodeEndpoint(const std::string& id,
        const std::vector<VasioClientSnapshot>& clients, std::string& error) {
    if (id.rfind(kPrefix, 0) != 0 || id.rfind(kAppPrefix, 0) == 0) return id;
    const auto pidEnd = id.find(':', sizeof(kPrefix) - 1);
    if (pidEnd == std::string::npos) { error = "Invalid virtual endpoint ID"; return {}; }
    std::uint32_t pid = 0;
    const auto pidText = id.substr(sizeof(kPrefix) - 1, pidEnd - (sizeof(kPrefix) - 1));
    const auto parsed = std::from_chars(pidText.data(), pidText.data() + pidText.size(), pid);
    if (parsed.ec != std::errc{} || parsed.ptr != pidText.data() + pidText.size()) {
        error = "Invalid virtual client PID"; return {};
    }
    for (const auto& client : clients) {
        if (client.info.processId != pid || client.info.driverId != 1) continue;
        const auto name = executableName(client.info.processName);
        if (name.empty()) break;
        return std::string(kAppPrefix) + name + id.substr(pidEnd);
    }
    error = "Cannot save a virtual route without its application name";
    return {};
}

inline std::optional<std::string> resolveEndpoint(const std::string& id,
        const std::vector<VasioClientSnapshot>& clients) {
    if (id.rfind(kAppPrefix, 0) != 0) {
        if (id.rfind(kPrefix, 0) != 0) return id;
        // Legacy PID routes may be used only while that exact client is present.
        const auto pidEnd = id.find(':', sizeof(kPrefix) - 1);
        if (pidEnd == std::string::npos) return {};
        std::uint32_t pid = 0;
        const auto text = id.substr(sizeof(kPrefix) - 1, pidEnd - (sizeof(kPrefix) - 1));
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), pid);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return {};
        const auto rest = id.substr(pidEnd + 1);
        const auto separator = rest.find(':');
        if (separator == std::string::npos) return {};
        const auto direction = rest.substr(0, separator);
        std::uint32_t channel = 0;
        if ((direction != "input" && direction != "output") ||
            !parseChannel(rest.substr(separator + 1), channel)) return {};
        for (const auto& client : clients)
            if (client.info.driverId == 1 && client.info.processId == pid &&
                active(client, direction, channel)) return id;
        return {};
    }
    const auto nameEnd = id.find(':', sizeof(kAppPrefix) - 1);
    if (nameEnd == std::string::npos) return {};
    const auto name = id.substr(sizeof(kAppPrefix) - 1, nameEnd - (sizeof(kAppPrefix) - 1));
    const auto rest = id.substr(nameEnd + 1);
    const auto separator = rest.find(':');
    if (separator == std::string::npos) return {};
    const auto direction = rest.substr(0, separator);
    std::uint32_t channel = 0;
    if ((direction != "input" && direction != "output") ||
        !parseChannel(rest.substr(separator + 1), channel)) return {};
    const VasioClientSnapshot* match = nullptr;
    for (const auto& client : clients) {
        if (client.info.driverId != 1 || executableName(client.info.processName) != name) continue;
        if (match) return {}; // Two instances need an explicit, unambiguous route.
        match = &client;
    }
    if (!match || !active(*match, direction, channel)) return {};
    return std::string(kPrefix) + std::to_string(match->info.processId) + ":" + rest;
}
}

inline std::vector<AudioRoute> Encode(const std::vector<AudioRoute>& routes,
        const std::vector<VasioClientSnapshot>& clients, std::string& error) {
    error.clear();
    std::vector<AudioRoute> result;
    result.reserve(routes.size());
    for (auto route : routes) {
        const auto source = Detail::encodeEndpoint(route.sourceEndpointId, clients, error);
        if (!source) return {};
        const auto destination = Detail::encodeEndpoint(route.destinationEndpointId, clients, error);
        if (!destination) return {};
        route.sourceEndpointId = *source;
        route.destinationEndpointId = *destination;
        result.push_back(std::move(route));
    }
    return result;
}

inline std::vector<AudioRoute> Resolve(const std::vector<AudioRoute>& routes,
        const std::vector<VasioClientSnapshot>& clients) {
    std::vector<AudioRoute> result;
    result.reserve(routes.size());
    for (auto route : routes) {
        const auto source = Detail::resolveEndpoint(route.sourceEndpointId, clients);
        const auto destination = Detail::resolveEndpoint(route.destinationEndpointId, clients);
        if (!source || !destination) continue;
        route.sourceEndpointId = *source;
        route.destinationEndpointId = *destination;
        result.push_back(std::move(route));
    }
    return result;
}

// The initial native device has two fixed stereo endpoints in each direction.
// Persistent IDs identify ports, never a worker epoch or a device generation.
inline std::optional<RoutingEndpointType> NativeEndpointType(const std::string& id) {
    if (id == "native:windows:input:1" || id == "native:windows:input:2")
        return RoutingEndpointType::NativeSource;
    if (id == "native:windows:output:1" || id == "native:windows:output:2")
        return RoutingEndpointType::NativeDestination;
    return {};
}

inline std::vector<AudioRoute> ResolveForClock(const std::vector<AudioRoute>& routes,
        const std::vector<VasioClientSnapshot>& clients,
        const std::optional<std::string>& physicalDriverId,
        const std::vector<WasapiRoutingBinding>& wasapiBindings = {},
        const std::vector<NativeRoutingBinding>& nativeBindings = {}) {
    auto active = Resolve(routes, clients);
    const auto available = [&physicalDriverId,&wasapiBindings,&nativeBindings](const std::string& endpoint) {
        if (endpoint.rfind("native:", 0) == 0) {
            const auto type = NativeEndpointType(endpoint);
            if (!type) return false;
            for (const auto& binding : nativeBindings) {
                const auto& generation = binding.generation;
                if (binding.id != "windows" || !generation || !generation->epoch || !generation->wireGeneration)
                    continue;
                const bool source = *type == RoutingEndpointType::NativeSource;
                const auto& io = source ? generation->windowsRenderSource : generation->windowsCaptureDestination;
                if (io && io->Format().generationId == generation->epoch && io->Format().channels == 2 &&
                    io->Format().direction == (source ? WasapiBridgeDirection::Capture : WasapiBridgeDirection::Render))
                    return true;
            }
            return false;
        }
        if (endpoint.rfind("physical:", 0) == 0)
            return physicalDriverId && endpoint.rfind("physical:" + *physicalDriverId + ":", 0) == 0;
        if (endpoint.rfind("wasapi:",0)!=0)return true;
        const auto separator=endpoint.find(':',7);
        if(separator==std::string::npos)return false;
        const auto id=endpoint.substr(7,separator-7);
        const auto channelSeparator=endpoint.find(':',separator+1);
        if(channelSeparator==std::string::npos)return false;
        const auto direction=endpoint.substr(separator+1,channelSeparator-separator-1);
        std::uint32_t channel=0;
        if(!Detail::parseChannel(endpoint.substr(channelSeparator+1),channel))return false;
        for(const auto& binding:wasapiBindings)if(binding.bridgeId==id && binding.io) {
            const auto& format=binding.io->Format();
            return channel<=format.channels && direction==(format.direction==WasapiBridgeDirection::Capture?"input":"output");
        }
        return false;
    };
    active.erase(std::remove_if(active.begin(), active.end(), [&](const AudioRoute& route) {
        return !available(route.sourceEndpointId) || !available(route.destinationEndpointId);
    }), active.end());
    return active;
}

inline std::optional<RoutingEndpointType> ProfiledEndpointType(const std::string& id,
        const std::vector<ApplicationProfile>& profiles) {
    if (id.rfind(Detail::kAppPrefix, 0) != 0) return {};
    const auto nameEnd = id.find(':', sizeof(Detail::kAppPrefix) - 1);
    if (nameEnd == std::string::npos) return {};
    const auto name = id.substr(sizeof(Detail::kAppPrefix) - 1,
        nameEnd - (sizeof(Detail::kAppPrefix) - 1));
    const auto rest = id.substr(nameEnd + 1);
    const auto separator = rest.find(':');
    if (separator == std::string::npos) return {};
    const auto direction = rest.substr(0, separator);
    std::uint32_t channel = 0;
    if ((direction != "input" && direction != "output") ||
        !Detail::parseChannel(rest.substr(separator + 1), channel)) return {};
    for (const auto& profile : profiles) {
        if (Detail::executableName(profile.processName) != name) continue;
        if (channel > (direction == "input" ? profile.inputChannels : profile.outputChannels))
            return {};
        return direction == "input" ? RoutingEndpointType::VirtualInput
            : RoutingEndpointType::VirtualOutput;
    }
    return {};
}

// A connection may be prepared while an application has not opened that channel yet.
// Its executable must already be known from a profile, a connected client, or a
// saved connection; otherwise arbitrary application names cannot enter the draft.
inline std::optional<RoutingEndpointType> KnownApplicationEndpointType(const std::string& id,
        const std::vector<ApplicationProfile>& profiles,
        const std::vector<VasioClientSnapshot>& clients,
        const std::vector<AudioRoute>& savedRoutes) {
    if (id.rfind(Detail::kAppPrefix, 0) != 0) return {};
    const auto nameEnd = id.find(':', sizeof(Detail::kAppPrefix) - 1);
    if (nameEnd == std::string::npos) return {};
    const auto name = id.substr(sizeof(Detail::kAppPrefix) - 1,
        nameEnd - (sizeof(Detail::kAppPrefix) - 1));
    if (name.empty()) return {};
    const auto rest = id.substr(nameEnd + 1);
    const auto separator = rest.find(':');
    if (separator == std::string::npos) return {};
    const auto direction = rest.substr(0, separator);
    std::uint32_t channel = 0;
    if ((direction != "input" && direction != "output") ||
        !Detail::parseChannel(rest.substr(separator + 1), channel)) return {};
    const auto type = direction == "input" ? RoutingEndpointType::VirtualInput
        : RoutingEndpointType::VirtualOutput;
    for (const auto& profile : profiles) {
        if (Detail::executableName(profile.processName) != name) continue;
        return channel <= (direction == "input" ? profile.inputChannels : profile.outputChannels)
            ? std::optional<RoutingEndpointType>(type) : std::nullopt;
    }
    for (const auto& client : clients)
        if (client.info.driverId == 1 && Detail::executableName(client.info.processName) == name)
            return type;
    const auto prefix = std::string(Detail::kAppPrefix) + name + ":";
    for (const auto& route : savedRoutes)
        if (route.sourceEndpointId.rfind(prefix, 0) == 0 ||
            route.destinationEndpointId.rfind(prefix, 0) == 0) return type;
    return {};
}
}
