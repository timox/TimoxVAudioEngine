#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "midi_ump.h"

struct MidiRoute {
    std::string id;
    std::string sourcePortId;
    std::string destinationPortId;
    bool enabled = true;
    std::uint16_t groupMask = 0xFFFF;    // bit n = source group n (0-based)
    std::uint16_t channelMask = 0xFFFF;  // bit n = source channel n (0-based)
    std::uint32_t messages = midi_ump::AllCategories;
    std::optional<std::uint8_t> outputGroup;    // 0-based
    std::optional<std::uint8_t> outputChannel;  // 0-based
    int transpose = 0;                          // semitones, -127..127
};

class MidiOutputSink {
public:
    virtual ~MidiOutputSink() = default;
    virtual void Send(std::size_t destinationIndex, const std::uint32_t* words, std::size_t count) noexcept = 0;
};

// Immutable after Create(); Process() performs no allocation or locking and may
// run concurrently from several receive threads.
class MidiRoutingGraph final {
public:
    static bool ValidateRoute(const MidiRoute& route, std::string& error);

    // ports: port identifiers; index in this vector is the port index used by
    // Process() and MidiOutputSink. Routes referencing an unknown port, or
    // disabled, are kept out of the compiled graph and reported inactive.
    static std::unique_ptr<MidiRoutingGraph> Create(const std::vector<std::string>& ports,
        const std::vector<MidiRoute>& routes, std::string& error);

    MidiRoutingGraph(const MidiRoutingGraph&) = delete;
    MidiRoutingGraph& operator=(const MidiRoutingGraph&) = delete;

    // words/count may contain several UMP messages; a truncated trailing
    // message is dropped.
    void Process(std::size_t sourceIndex, const std::uint32_t* words, std::size_t count,
        MidiOutputSink& sink) const noexcept;

    std::size_t PortCount() const noexcept { return portCount_; }
    std::size_t RouteCount() const noexcept { return routeIds_.size(); }
    // Statistics are indexed like the routes passed to Create().
    bool RouteActive(std::size_t routeIndex) const noexcept;
    std::uint64_t Forwarded(std::size_t routeIndex) const noexcept;
    std::uint64_t Filtered(std::size_t routeIndex) const noexcept;

private:
    struct CompiledRoute {
        std::size_t routeIndex;
        std::size_t destinationIndex;
        std::uint16_t groupMask;
        std::uint16_t channelMask;
        std::uint32_t messages;
        int outputGroup;    // -1 = unchanged
        int outputChannel;  // -1 = unchanged
        int transpose;
    };

    explicit MidiRoutingGraph(std::size_t portCount, std::size_t routeCount);
    void ProcessMessage(std::size_t sourceIndex, const std::uint32_t* words, std::size_t count,
        MidiOutputSink& sink) const noexcept;

    std::size_t portCount_;
    std::vector<std::string> routeIds_;
    std::vector<bool> active_;
    std::vector<std::vector<CompiledRoute>> bySource_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> forwarded_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> filtered_;
};
