#include "midi_routing_graph.h"

#include <array>
#include <set>
#include <unordered_map>

namespace {
bool validIdentifier(const std::string& value) {
    if (value.empty() || value.size() > 512) return false;
    for (const unsigned char c : value)
        if (c < 0x20 || c == 0x7F) return false;
    return true;
}
}

bool MidiRoutingGraph::ValidateRoute(const MidiRoute& route, std::string& error) {
    if (!validIdentifier(route.id)) { error = "MIDI route id must be 1 to 512 printable characters"; return false; }
    if (!validIdentifier(route.sourcePortId) || !validIdentifier(route.destinationPortId)) {
        error = "MIDI route " + route.id + " requires source and destination port ids";
        return false;
    }
    if (route.sourcePortId == route.destinationPortId) {
        error = "MIDI route " + route.id + " cannot send a port back to itself";
        return false;
    }
    if (route.channelMask == 0) {
        error = "MIDI route " + route.id + " must accept at least one channel";
        return false;
    }
    if (route.messages == 0 || (route.messages & ~static_cast<std::uint32_t>(midi_ump::AllCategories)) != 0) {
        error = "MIDI route " + route.id + " has an invalid message filter";
        return false;
    }
    if (route.outputChannel && *route.outputChannel > 15) {
        error = "MIDI route " + route.id + " output channel must be 1 to 16";
        return false;
    }
    if (route.transpose < -127 || route.transpose > 127) {
        error = "MIDI route " + route.id + " transpose must be between -127 and 127";
        return false;
    }
    return true;
}

MidiRoutingGraph::MidiRoutingGraph(std::size_t portCount, std::size_t routeCount)
    : portCount_(portCount), active_(routeCount, false), bySource_(portCount),
      forwarded_(new std::atomic<std::uint64_t>[routeCount == 0 ? 1 : routeCount]),
      filtered_(new std::atomic<std::uint64_t>[routeCount == 0 ? 1 : routeCount]) {
    for (std::size_t i = 0; i < routeCount; ++i) {
        forwarded_[i].store(0, std::memory_order_relaxed);
        filtered_[i].store(0, std::memory_order_relaxed);
    }
}

std::unique_ptr<MidiRoutingGraph> MidiRoutingGraph::Create(const std::vector<std::string>& ports,
    const std::vector<MidiRoute>& routes, std::string& error) {
    error.clear();
    std::unordered_map<std::string, std::size_t> portIndices;
    for (std::size_t i = 0; i < ports.size(); ++i) {
        if (!validIdentifier(ports[i]) || !portIndices.emplace(ports[i], i).second) {
            error = "MIDI port ids must be unique printable strings";
            return nullptr;
        }
    }
    std::set<std::string> routeIds;
    for (const auto& route : routes) {
        if (!ValidateRoute(route, error)) return nullptr;
        if (!routeIds.insert(route.id).second) { error = "Duplicate MIDI route id " + route.id; return nullptr; }
    }

    std::unique_ptr<MidiRoutingGraph> graph(new MidiRoutingGraph(ports.size(), routes.size()));
    graph->routeIds_.reserve(routes.size());
    for (std::size_t i = 0; i < routes.size(); ++i) {
        const auto& route = routes[i];
        graph->routeIds_.push_back(route.id);
        const auto source = portIndices.find(route.sourcePortId);
        const auto destination = portIndices.find(route.destinationPortId);
        if (!route.enabled || source == portIndices.end() || destination == portIndices.end()) continue;
        graph->active_[i] = true;
        graph->bySource_[source->second].push_back({i, destination->second,
            route.channelMask, route.messages,
            route.outputChannel ? static_cast<int>(*route.outputChannel) : -1,
            route.transpose});
    }
    return graph;
}

void MidiRoutingGraph::Process(std::size_t sourceIndex, const std::uint32_t* words, std::size_t count,
    MidiOutputSink& sink) const noexcept {
    if (sourceIndex >= portCount_ || !words) return;
    std::size_t offset = 0;
    while (offset < count) {
        const auto length = midi_ump::WordCount(words[offset]);
        if (offset + length > count) return;
        ProcessMessage(sourceIndex, words + offset, length, sink);
        offset += length;
    }
}

void MidiRoutingGraph::ProcessMessage(std::size_t sourceIndex, const std::uint32_t* words, std::size_t count,
    MidiOutputSink& sink) const noexcept {
    const auto& routes = bySource_[sourceIndex];
    if (routes.empty()) return;
    const std::uint32_t word0 = words[0];
    const std::uint32_t category = midi_ump::Category(word0);
    if (category == 0) return;
    const bool channelVoice = midi_ump::IsChannelVoice(word0);
    const bool hasNote = midi_ump::CarriesNoteNumber(word0);

    std::array<std::uint32_t, 4> output{};
    for (const auto& route : routes) {
        bool pass = (route.messages & category) != 0;
        if (pass && channelVoice) pass = (route.channelMask >> midi_ump::Channel(word0)) & 1u;
        std::uint32_t first = word0;
        if (pass && hasNote && route.transpose != 0) {
            const int note = static_cast<int>(midi_ump::NoteNumber(word0)) + route.transpose;
            // Out-of-range notes are dropped for both note on and note off,
            // so no note is left hanging.
            if (note < 0 || note > 127) pass = false;
            else first = midi_ump::WithNoteNumber(first, static_cast<std::uint8_t>(note));
        }
        if (!pass) {
            filtered_[route.routeIndex].fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (channelVoice && route.outputChannel >= 0)
            first = midi_ump::WithChannel(first, static_cast<std::uint8_t>(route.outputChannel));
        output[0] = first;
        for (std::size_t i = 1; i < count; ++i) output[i] = words[i];
        sink.Send(route.destinationIndex, output.data(), count);
        forwarded_[route.routeIndex].fetch_add(1, std::memory_order_relaxed);
    }
}

bool MidiRoutingGraph::RouteActive(std::size_t routeIndex) const noexcept {
    return routeIndex < active_.size() && active_[routeIndex];
}

std::uint64_t MidiRoutingGraph::Forwarded(std::size_t routeIndex) const noexcept {
    return routeIndex < routeIds_.size() ? forwarded_[routeIndex].load(std::memory_order_relaxed) : 0;
}

std::uint64_t MidiRoutingGraph::Filtered(std::size_t routeIndex) const noexcept {
    return routeIndex < routeIds_.size() ? filtered_[routeIndex].load(std::memory_order_relaxed) : 0;
}
