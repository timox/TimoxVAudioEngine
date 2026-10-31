#include "windows_midi_services_backend.h"

#if !defined(TIMOX_ENABLE_WINDOWS_MIDI_SERVICES)

std::unique_ptr<MidiBackend> CreateWindowsMidiServicesBackend(std::string& unavailableReason) {
    unavailableReason = "This engine was built without Windows MIDI Services support";
    return nullptr;
}

#else

// MIDI 1.0 only: only native MIDI 1.0 (byte format) endpoints are listed, each
// presented as its MIDI 1.0 ports (one per UMP group); native MIDI 2.0
// endpoints are ignored. Virtual ports are declared as MIDI 1.0 devices.
// Built against the Windows.Devices.Midi2 NuGet metadata 0.99.92-preview.11,
// which is the API for the Windows MIDI Services preview 12 runtime
// (Tools 0.99.94-preview.12, Transports 1.0.43-preview.12). The using
// directives below are the only place to change if a later SDK renames them.
#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Midi2.h>
#include <winrt/Windows.Devices.Midi2.Enumeration.h>
#include <winrt/Windows.Devices.Midi2.Transports.Virtual.h>

#include <condition_variable>
#include <cstdio>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <array>

namespace {
// Block and port-name types are documented under both the root and the
// Enumeration namespace depending on the page; using directives let the
// compiler find them in whichever namespace the metadata declares.
using namespace winrt::Windows::Devices::Midi2;
using namespace winrt::Windows::Devices::Midi2::Enumeration;
using namespace winrt::Windows::Devices::Midi2::Transports::Virtual;

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), bytes,
        nullptr, nullptr);
    return result;
}

std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const int characters = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (characters <= 0) return {};
    std::wstring result(static_cast<std::size_t>(characters), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), characters);
    return result;
}

std::string describe(const winrt::hresult_error& error) {
    char code[16];
    std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned>(error.code().value));
    const auto message = utf8(error.message());
    return message.empty() ? std::string("Windows MIDI Services error ") + code : message + " (" + code + ")";
}

// All control calls run on one MTA thread so WinRT objects are never created
// on a caller thread that COM initialized differently (WASAPI, HTTP threads).
class MtaWorker final {
public:
    MtaWorker() : thread_([this] { Loop(); }) {}
    ~MtaWorker() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
        ready_.notify_one();
        thread_.join();
    }
    void Run(const std::function<void()>& work) {
        std::packaged_task<void()> task([&work] { work(); });
        auto done = task.get_future();
        { std::lock_guard<std::mutex> lock(mutex_); queue_.push_back(std::move(task)); }
        ready_.notify_one();
        done.get();
    }
private:
    void Loop() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            std::packaged_task<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) break;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            task();
        }
        winrt::uninit_apartment();
    }
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::packaged_task<void()>> queue_;
    bool stopping_ = false;
    std::thread thread_;
};

class WindowsMidiServicesBackend final : public MidiBackend {
public:
    ~WindowsMidiServicesBackend() override {
        worker_.Run([this] {
            std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> remaining;
            { std::lock_guard<std::mutex> lock(mutex_); remaining.swap(connections_); }
            for (auto& entry : remaining) Disconnect(*entry.second);
            try { if (session_) session_.Close(); } catch (...) {}
            session_ = nullptr;
        });
    }

    bool Initialize(std::string& reason) {
        bool ok = false;
        worker_.Run([&] {
            try {
                if (!MidiApi::EnsureServiceAvailable()) {
                    reason = "The Windows MIDI Services service is not running or not installed";
                    return;
                }
                virtualAvailable_ = MidiVirtualDeviceManager::IsTransportAvailable();
                session_ = MidiSession::Create(L"Timox MIDI patchbay");
                ok = session_ != nullptr;
                if (!ok) reason = "Unable to create a Windows MIDI Services session";
            } catch (const winrt::hresult_error& error) {
                reason = "Windows MIDI Services is unavailable: " + describe(error);
            }
        });
        return ok;
    }

    std::string Name() const override { return "windows-midi-services"; }

    void SetReceiveHandler(ReceiveHandler handler) override {
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = std::make_shared<ReceiveHandler>(std::move(handler));
    }

    bool Enumerate(std::vector<MidiEndpointInfo>& endpoints, std::string& error) override {
        bool ok = false;
        worker_.Run([&] {
            try {
                std::set<std::wstring> ownClients;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    for (const auto& entry : connections_)
                        if (!entry.second->clientEndpointId.empty()) ownClients.insert(entry.second->clientEndpointId);
                }
                endpoints.clear();
                for (const auto& device : MidiEndpointDeviceInformation::FindAll()) {
                    const std::wstring id{device.EndpointDeviceId()};
                    if (ownClients.count(id)) continue;  // our own virtual ports
                    // MIDI 1.0 devices only: native UMP (MIDI 2.0) endpoints are skipped.
                    if (!MidiEndpointDeviceInformation::DeviceMatchesFilter(device,
                            MidiEndpointDeviceInformationFilters::StandardNativeMidi1ByteFormat)) continue;
                    MidiEndpointInfo endpoint{utf8(id), utf8(device.Name()), "UMP", Midi1Ports(device)};
                    if (!endpoint.ports.empty()) endpoints.push_back(std::move(endpoint));
                }
                ok = true;
            } catch (const winrt::hresult_error& failure) {
                error = describe(failure);
            }
        });
        return ok;
    }

    bool OpenEndpoint(std::uint64_t cookie, const std::string& endpointId, std::string& error) override {
        bool ok = false;
        worker_.Run([&] {
            auto connection = std::make_shared<Connection>();
            try {
                connection->connection = session_.CreateEndpointConnection(wide(endpointId));
                Subscribe(cookie, *connection);
                if (!connection->connection.Open()) {
                    error = "The MIDI endpoint could not be opened";
                    Disconnect(*connection);
                    return;
                }
            } catch (const winrt::hresult_error& failure) {
                error = describe(failure);
                Disconnect(*connection);
                return;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            connections_[cookie] = std::move(connection);
            ok = true;
        });
        return ok;
    }

    bool CreateVirtualPort(std::uint64_t cookie, const std::string& stableId, const std::string& displayName,
                           std::string& clientEndpointId, std::string& error) override {
        bool ok = false;
        worker_.Run([&] {
            if (!virtualAvailable_) {
                error = "The Windows MIDI Services virtual device transport is not available";
                return;
            }
            auto connection = std::make_shared<Connection>();
            try {
                const auto name = wide(displayName);
                MidiDeclaredEndpointInfo endpointInfo;
                endpointInfo.Name(name);
                // Stable across restarts so applications find the same port again.
                endpointInfo.ProductInstanceId(wide("TimoxVasio-" + stableId));
                endpointInfo.HasStaticFunctionBlocks(true);
                // MIDI 1.0 only: applications get a plain MIDI 1.0 input and output.
                endpointInfo.SupportsMidi10Protocol(true);
                endpointInfo.SupportsMidi20Protocol(false);
                endpointInfo.SupportsReceivingJitterReductionTimestamps(false);
                endpointInfo.SupportsSendingJitterReductionTimestamps(false);
                endpointInfo.SpecificationVersionMajor(1);
                endpointInfo.SpecificationVersionMinor(1);

                MidiVirtualDeviceCreationConfig config(name,
                    L"TimoxVasio MIDI patchbay port", L"TimoxVasio", endpointInfo);
                // One bidirectional MIDI 1.0 block on group 1. MIDI 1.0
                // applications see one input and one output, named from the
                // port name and the block name ("<name> MIDI").
                MidiFunctionBlock block;
                block.Number(0);
                block.IsActive(true);
                block.Name(L"MIDI");
                block.FirstGroup(MidiGroup(static_cast<std::uint8_t>(0)));
                block.GroupCount(1);
                block.Direction(MidiFunctionBlockDirection::Bidirectional);
                block.RepresentsMidi10Connection(MidiFunctionBlockRepresentsMidi10Connection::YesBandwidthUnrestricted);
                config.FunctionBlocks().Append(block);

                connection->device = MidiVirtualDeviceManager::CreateVirtualDevice(config);
                if (!connection->device) {
                    error = "Windows MIDI Services refused to create the virtual device";
                    return;
                }
                connection->connection = session_.CreateEndpointConnection(connection->device.DeviceEndpointDeviceId());
                Subscribe(cookie, *connection);
                if (connection->connection.AddMessageProcessingPlugin(connection->device) !=
                    MidiMessageProcessingPluginAddResult::Succeeded) {
                    error = "Unable to attach the virtual device to its connection";
                    Disconnect(*connection);
                    return;
                }
                if (!connection->connection.Open()) {
                    error = "The virtual MIDI device could not be opened";
                    Disconnect(*connection);
                    return;
                }
                connection->clientEndpointId = std::wstring{
                    MidiVirtualDeviceManager::GetAssociatedClientEndpointDeviceId(config.AssociationId())};
            } catch (const winrt::hresult_error& failure) {
                error = describe(failure);
                Disconnect(*connection);
                return;
            }
            clientEndpointId = utf8(connection->clientEndpointId);
            std::lock_guard<std::mutex> lock(mutex_);
            connections_[cookie] = std::move(connection);
            ok = true;
        });
        return ok;
    }

    void Close(std::uint64_t cookie) noexcept override {
        try {
            worker_.Run([&] {
                std::shared_ptr<Connection> connection;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto found = connections_.find(cookie);
                    if (found == connections_.end()) return;
                    connection = std::move(found->second);
                    connections_.erase(found);
                }
                Disconnect(*connection);
            });
        } catch (...) {}
    }

    bool Send(std::uint64_t cookie, const std::uint32_t* words, std::size_t count) noexcept override {
        std::shared_ptr<Connection> connection;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto found = connections_.find(cookie);
            if (found == connections_.end()) return false;
            connection = found->second;
        }
        try {
            const auto now = MidiClock::TimestampConstantSendImmediately();
            MidiSendMessageResults result{};
            switch (count) {
            case 1: result = connection->connection.SendSingleMessageWords(now, words[0]); break;
            case 2: result = connection->connection.SendSingleMessageWords(now, words[0], words[1]); break;
            case 3: result = connection->connection.SendSingleMessageWords(now, words[0], words[1], words[2]); break;
            case 4: result = connection->connection.SendSingleMessageWords(now, words[0], words[1], words[2], words[3]); break;
            default: return false;
            }
            return MidiEndpointConnection::SendMessageSucceeded(result);
        } catch (...) {
            return false;
        }
    }

private:
    // MIDI 1.0 ports of an endpoint: one per UMP group. Direction comes from
    // the function blocks, or from USB group terminal blocks when the device
    // declares none. Block directions are from the block's point of view:
    // BlockOutput sends (a source for us), BlockInput receives (a destination).
    static std::vector<Midi1PortInfo> Midi1Ports(const MidiEndpointDeviceInformation& device) {
        struct Group { bool source = false, destination = false; std::wstring blockName; };
        std::array<Group, 16> groups{};
        bool declared = false;
        const auto mark = [&](std::uint8_t first, std::uint32_t count, bool sends, bool receives,
                              const winrt::hstring& name) {
            for (std::uint32_t g = first; g < first + count && g < 16; ++g) {
                groups[g].source |= sends;
                groups[g].destination |= receives;
                if (groups[g].blockName.empty()) groups[g].blockName = name;
                declared = true;
            }
        };
        for (const auto& block : device.GetDeclaredFunctionBlocks()) {
            if (!block.IsActive()) continue;
            const auto direction = block.Direction();
            mark(block.FirstGroup().Index(), block.GroupCount(),
                direction == MidiFunctionBlockDirection::BlockOutput || direction == MidiFunctionBlockDirection::Bidirectional,
                direction == MidiFunctionBlockDirection::BlockInput || direction == MidiFunctionBlockDirection::Bidirectional,
                block.Name());
        }
        if (!declared)
            for (const auto& block : device.GetGroupTerminalBlocks()) {
                const auto direction = block.Direction();
                mark(block.FirstGroup().Index(), block.GroupCount(),
                    direction != MidiGroupTerminalBlockDirection::BlockInput,
                    direction != MidiGroupTerminalBlockDirection::BlockOutput,
                    block.Name());
            }
        if (!declared) groups[0] = {true, true, {}};  // plain single-port endpoint

        // Names Windows gives the MIDI 1.0 ports, as MIDI 1.0 applications show them.
        std::array<std::wstring, 16> names{};
        for (const auto& entry : device.GetNameTable()) {
            const auto group = entry.Group().Index();
            if (group >= 16 || !names[group].empty()) continue;
            names[group] = entry.CustomName().empty() ? std::wstring{entry.NewStyleName()} : std::wstring{entry.CustomName()};
        }

        std::vector<Midi1PortInfo> ports;
        const std::wstring endpointName{device.Name()};
        for (std::uint8_t g = 0; g < 16; ++g) {
            if (!groups[g].source && !groups[g].destination) continue;
            auto name = names[g];
            if (name.empty())
                name = groups[g].blockName.empty() ? endpointName + L" " + std::to_wstring(g + 1)
                                                   : endpointName + L" " + groups[g].blockName;
            ports.push_back({g, utf8(name), groups[g].source, groups[g].destination});
        }
        return ports;
    }

    struct Connection {
        MidiEndpointConnection connection{nullptr};
        winrt::event_token token{};
        MidiVirtualDevice device{nullptr};
        std::wstring clientEndpointId;
    };

    void Subscribe(std::uint64_t cookie, Connection& connection) {
        connection.token = connection.connection.MessageReceived(
            [this, cookie](const IMidiMessageReceivedEventSource&, const MidiMessageReceivedEventArgs& args) {
                OnMessage(cookie, args);
            });
    }

    void OnMessage(std::uint64_t cookie, const MidiMessageReceivedEventArgs& args) noexcept {
        try {
            std::shared_ptr<ReceiveHandler> handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                handler = handler_;
            }
            if (!handler || !*handler) return;
            std::uint32_t words[4]{};
            std::size_t count = 0;
            const auto packet = args.GetMessagePacket();
            switch (packet.PacketType()) {
            case MidiPacketType::UniversalMidiPacket32: {
                const auto message = packet.as<MidiMessage32>();
                words[0] = message.Word0();
                count = 1;
                break;
            }
            case MidiPacketType::UniversalMidiPacket64: {
                const auto message = packet.as<MidiMessage64>();
                words[0] = message.Word0(); words[1] = message.Word1();
                count = 2;
                break;
            }
            case MidiPacketType::UniversalMidiPacket96: {
                const auto message = packet.as<MidiMessage96>();
                words[0] = message.Word0(); words[1] = message.Word1(); words[2] = message.Word2();
                count = 3;
                break;
            }
            case MidiPacketType::UniversalMidiPacket128: {
                const auto message = packet.as<MidiMessage128>();
                words[0] = message.Word0(); words[1] = message.Word1();
                words[2] = message.Word2(); words[3] = message.Word3();
                count = 4;
                break;
            }
            default:
                return;
            }
            (*handler)(cookie, words, count);
        } catch (...) {
            // A malformed packet must never take the receive thread down.
        }
    }

    // Runs on the worker thread.
    void Disconnect(Connection& connection) noexcept {
        try {
            if (connection.connection) {
                if (connection.token) connection.connection.MessageReceived(connection.token);
                session_.DisconnectEndpointConnection(connection.connection.ConnectionId());
            }
        } catch (...) {}
        connection.connection = nullptr;
        connection.device = nullptr;  // the virtual device is torn down with its connection
    }

    MtaWorker worker_;
    MidiSession session_{nullptr};
    bool virtualAvailable_ = false;
    std::mutex mutex_;
    std::unordered_map<std::uint64_t, std::shared_ptr<Connection>> connections_;
    std::shared_ptr<ReceiveHandler> handler_;
};
}

std::unique_ptr<MidiBackend> CreateWindowsMidiServicesBackend(std::string& unavailableReason) {
    auto backend = std::make_unique<WindowsMidiServicesBackend>();
    if (!backend->Initialize(unavailableReason)) return nullptr;
    return backend;
}

#endif
