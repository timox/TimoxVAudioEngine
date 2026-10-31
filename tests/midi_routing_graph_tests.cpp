#include "midi_routing_graph.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
struct Sent { std::size_t destination; std::vector<std::uint32_t> words; };

class RecordingSink final : public MidiOutputSink {
public:
    void Send(std::size_t destinationIndex, const std::uint32_t* words, std::size_t count) noexcept override {
        sent.push_back({destinationIndex, std::vector<std::uint32_t>(words, words + count)});
    }
    std::vector<Sent> sent;
};

// MIDI 1.0 channel voice UMP: type 2, group, status nibble, channel, data.
constexpr std::uint32_t midi1(std::uint8_t group, std::uint8_t status, std::uint8_t channel,
                              std::uint8_t data1, std::uint8_t data2) {
    return 0x20000000u | (std::uint32_t(group) << 24) | (std::uint32_t(status) << 20) |
           (std::uint32_t(channel) << 16) | (std::uint32_t(data1) << 8) | data2;
}

const std::vector<std::string> kPorts{"midi:virtual:daw", "midi:endpoint:synth", "midi:endpoint:drums"};

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); return false; } } while (0)

bool testForwardsUnchangedToEveryRoute() {
    std::vector<MidiRoute> routes(2);
    routes[0].id = "a"; routes[0].sourcePortId = "midi:virtual:daw"; routes[0].destinationPortId = "midi:endpoint:synth";
    routes[1].id = "b"; routes[1].sourcePortId = "midi:virtual:daw"; routes[1].destinationPortId = "midi:endpoint:drums";
    std::string error;
    auto graph = MidiRoutingGraph::Create(kPorts, routes, error);
    CHECK(graph);
    RecordingSink sink;
    const std::uint32_t note = midi1(0, 0x9, 3, 60, 100);
    graph->Process(0, &note, 1, sink);
    CHECK(sink.sent.size() == 2);
    CHECK(sink.sent[0].destination == 1 && sink.sent[0].words[0] == note);
    CHECK(sink.sent[1].destination == 2 && sink.sent[1].words[0] == note);
    CHECK(graph->Forwarded(0) == 1 && graph->Forwarded(1) == 1);
    // Nothing is routed from a port without routes.
    graph->Process(1, &note, 1, sink);
    CHECK(sink.sent.size() == 2);
    return true;
}

bool testChannelFilterRemapAndTranspose() {
    MidiRoute route;
    route.id = "split"; route.sourcePortId = "midi:virtual:daw"; route.destinationPortId = "midi:endpoint:synth";
    route.channelMask = 1u << 0;   // channel 1 only
    route.outputChannel = 9;       // to channel 10
    route.transpose = 12;
    std::string error;
    auto graph = MidiRoutingGraph::Create(kPorts, {route}, error);
    CHECK(graph);
    RecordingSink sink;
    const std::uint32_t ch1 = midi1(0, 0x9, 0, 60, 100);
    const std::uint32_t ch2 = midi1(0, 0x9, 1, 60, 100);
    graph->Process(0, &ch1, 1, sink);
    graph->Process(0, &ch2, 1, sink);
    CHECK(sink.sent.size() == 1);
    CHECK(sink.sent[0].words[0] == midi1(0, 0x9, 9, 72, 100));
    CHECK(graph->Filtered(0) == 1);
    // Transposed out of range: dropped for note on and note off alike.
    const std::uint32_t high[] = {midi1(0, 0x9, 0, 120, 1), midi1(0, 0x8, 0, 120, 0)};
    graph->Process(0, high, 2, sink);
    CHECK(sink.sent.size() == 1);
    // Control change is not transposed.
    const std::uint32_t cc = midi1(0, 0xB, 0, 120, 5);
    graph->Process(0, &cc, 1, sink);
    CHECK(sink.sent.size() == 2 && sink.sent[1].words[0] == midi1(0, 0xB, 9, 120, 5));
    return true;
}

bool testMessageCategoriesAndMultiWordPackets() {
    MidiRoute route;
    route.id = "clock"; route.sourcePortId = "midi:virtual:daw"; route.destinationPortId = "midi:endpoint:drums";
    route.messages = midi_ump::SystemRealtime | midi_ump::SysEx;
    std::string error;
    auto graph = MidiRoutingGraph::Create(kPorts, {route}, error);
    CHECK(graph);
    RecordingSink sink;
    const std::uint32_t buffer[] = {
        0x10F80000u,               // timing clock
        midi1(0, 0x9, 0, 60, 1),   // note: filtered
        0x30160001u, 0x02030405u,  // SysEx7 complete, 2 words
        0x40903C00u, 0xFFFF0000u,  // MIDI 2.0 note on: not MIDI 1.0, never routed
        0xD0000000u, 0, 0, 0,      // flex data: never routed
        0xF0000000u, 0, 0, 0,      // UMP stream message: never routed
        0x00200000u,               // utility (JR timestamp): never routed
        0x30160001u,               // truncated SysEx7: dropped
    };
    graph->Process(0, buffer, sizeof(buffer) / sizeof(buffer[0]), sink);
    CHECK(sink.sent.size() == 2);
    CHECK(sink.sent[0].words.size() == 1 && sink.sent[0].words[0] == 0x10F80000u);
    CHECK(sink.sent[1].words.size() == 2 && sink.sent[1].words[1] == 0x02030405u);
    return true;
}

bool testMidi2ToMidi1Translation() {
    std::uint32_t out[4];
    const std::uint32_t noteOn[] = {0x40913C00u, 0x00000000u};  // velocity 0 on MIDI 2.0
    CHECK(midi_ump::Midi2ToMidi1(noteOn, out) == 1 && out[0] == midi1(0, 0x9, 1, 60, 1));
    const std::uint32_t loudNote[] = {0x42913C00u, 0xFFFF0000u};
    CHECK(midi_ump::Midi2ToMidi1(loudNote, out) == 1 && out[0] == midi1(2, 0x9, 1, 60, 127));
    const std::uint32_t cc[] = {0x40B00700u, 0x80000000u};
    CHECK(midi_ump::Midi2ToMidi1(cc, out) == 1 && out[0] == midi1(0, 0xB, 0, 7, 64));
    const std::uint32_t bend[] = {0x40E00000u, 0x80000000u};
    CHECK(midi_ump::Midi2ToMidi1(bend, out) == 1 && out[0] == midi1(0, 0xE, 0, 0, 64));
    const std::uint32_t program[] = {0x40C00001u, 0x05000203u};  // program 5, bank 2/3
    CHECK(midi_ump::Midi2ToMidi1(program, out) == 3);
    CHECK(out[0] == midi1(0, 0xB, 0, 0, 2) && out[1] == midi1(0, 0xB, 0, 32, 3) && out[2] == midi1(0, 0xC, 0, 5, 0));
    const std::uint32_t rpn[] = {0x40200001u, 0x80000000u};  // RPN 0/1
    CHECK(midi_ump::Midi2ToMidi1(rpn, out) == 4);
    CHECK(out[0] == midi1(0, 0xB, 0, 101, 0) && out[1] == midi1(0, 0xB, 0, 100, 1) &&
          out[2] == midi1(0, 0xB, 0, 6, 64) && out[3] == midi1(0, 0xB, 0, 38, 0));
    const std::uint32_t perNote[] = {0x40003C01u, 0x80000000u};
    CHECK(midi_ump::Midi2ToMidi1(perNote, out) == 0);
    return true;
}

bool testInactiveAndInvalidRoutes() {
    MidiRoute missing;
    missing.id = "missing"; missing.sourcePortId = "midi:virtual:daw"; missing.destinationPortId = "midi:endpoint:gone";
    MidiRoute disabled;
    disabled.id = "off"; disabled.sourcePortId = "midi:virtual:daw"; disabled.destinationPortId = "midi:endpoint:synth";
    disabled.enabled = false;
    std::string error;
    auto graph = MidiRoutingGraph::Create(kPorts, {missing, disabled}, error);
    CHECK(graph && !graph->RouteActive(0) && !graph->RouteActive(1));

    MidiRoute loop = disabled;
    loop.id = "loop"; loop.destinationPortId = loop.sourcePortId;
    CHECK(!MidiRoutingGraph::Create(kPorts, {loop}, error));
    MidiRoute duplicate = disabled;
    CHECK(!MidiRoutingGraph::Create(kPorts, {disabled, duplicate}, error));
    MidiRoute noChannels = disabled;
    noChannels.channelMask = 0;
    CHECK(!MidiRoutingGraph::Create(kPorts, {noChannels}, error));
    return true;
}
}

int main() {
    bool ok = true;
    ok &= testForwardsUnchangedToEveryRoute();
    ok &= testChannelFilterRemapAndTranspose();
    ok &= testMessageCategoriesAndMultiWordPackets();
    ok &= testMidi2ToMidi1Translation();
    ok &= testInactiveAndInvalidRoutes();
    if (!ok) return 1;
    std::puts("MidiRoutingGraphTests passed");
    return 0;
}
