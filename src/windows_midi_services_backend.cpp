#include "windows_midi_services_backend.h"

#if !defined(TIMOX_ENABLE_WINDOWS_MIDI_SERVICES)

std::unique_ptr<MidiBackend> CreateWindowsMidiServicesBackend(std::string& unavailableReason) {
    unavailableReason = "This engine was built without Windows MIDI Services support";
    return nullptr;
}

#else

// Built against the Windows.Devices.Midi2 NuGet metadata 0.99.92-preview.11,
// which is the API for the Windows MIDI Services preview 12 runtime
// (Tools 0.99.94-preview.12, Transports 1.0.43-preview.12). The namespace
// aliases below are the only place to change if a later SDK renames them.
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

namespace {
namespace midi2 = winrt::Windows::Devices::Midi2;
namespace midi2enum = winrt::Windows::Devices::Midi2::Enumeration;
namespace midi2virtual = winrt::Windows::Devices::Midi2::Transports::Virtual;

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
                if (!midi2::MidiApi::EnsureServiceAvailable()) {
                    reason = "The Windows MIDI Services service is not running or not installed";
                    return;
                }
                virtualAvailable_ = midi2virtual::MidiVirtualDeviceManager::IsTransportAvailable();
                session_ = midi2::MidiSession::Create(L"Timox MIDI patchbay");
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
                for (const auto& device : midi2enum::MidiEndpointDeviceInformation::FindAll()) {
                    const std::wstring id{device.EndpointDeviceId()};
                    if (ownClients.count(id)) continue;  // our own virtual ports
                    endpoints.push_back({utf8(id), utf8(device.Name()), "UMP"});
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
                midi2::MidiDeclaredEndpointInfo endpointInfo;
                endpointInfo.Name(name);
                // Stable across restarts so applications find the same port again.
                endpointInfo.ProductInstanceId(wide("TimoxVasio-" + stableId));
                endpointInfo.HasStaticFunctionBlocks(true);
                endpointInfo.SupportsMidi10Protocol(true);
                endpointInfo.SupportsMidi20Protocol(true);
                endpointInfo.SupportsReceivingJitterReductionTimestamps(false);
                endpointInfo.SupportsSendingJitterReductionTimestamps(false);
                endpointInfo.SpecificationVersionMajor(1);
                endpointInfo.SpecificationVersionMinor(1);

                midi2virtual::MidiVirtualDeviceCreationConfig config(name,
                    L"TimoxVasio MIDI patchbay port", L"TimoxVasio", endpointInfo);
                // One bidirectional block on group 1. MIDI 1.0 applications see
                // it as an input and an output named after the port.
                midi2::MidiFunctionBlock block;
                block.Number(0);
                block.IsActive(true);
                block.Name(L"MIDI");
                block.FirstGroup(midi2::MidiGroup(static_cast<std::uint8_t>(0)));
                block.GroupCount(1);
                block.Direction(midi2::MidiFunctionBlockDirection::Bidirectional);
                block.RepresentsMidi10Connection(midi2::MidiFunctionBlockRepresentsMidi10Connection::Not10);
                config.FunctionBlocks().Append(block);

                connection->device = midi2virtual::MidiVirtualDeviceManager::CreateVirtualDevice(config);
                if (!connection->device) {
                    error = "Windows MIDI Services refused to create the virtual device";
                    return;
                }
                connection->connection = session_.CreateEndpointConnection(connection->device.DeviceEndpointDeviceId());
                Subscribe(cookie, *connection);
                if (connection->connection.AddMessageProcessingPlugin(connection->device) !=
                    midi2::MidiMessageProcessingPluginAddResult::Succeeded) {
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
                    midi2virtual::MidiVirtualDeviceManager::GetAssociatedClientEndpointDeviceId(config.AssociationId())};
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
            const auto now = midi2::MidiClock::TimestampConstantSendImmediately();
            midi2::MidiSendMessageResults result{};
            switch (count) {
            case 1: result = connection->connection.SendSingleMessageWords(now, words[0]); break;
            case 2: result = connection->connection.SendSingleMessageWords(now, words[0], words[1]); break;
            case 3: result = connection->connection.SendSingleMessageWords(now, words[0], words[1], words[2]); break;
            case 4: result = connection->connection.SendSingleMessageWords(now, words[0], words[1], words[2], words[3]); break;
            default: return false;
            }
            return midi2::MidiEndpointConnection::SendMessageSucceeded(result);
        } catch (...) {
            return false;
        }
    }

private:
    struct Connection {
        midi2::MidiEndpointConnection connection{nullptr};
        winrt::event_token token{};
        midi2virtual::MidiVirtualDevice device{nullptr};
        std::wstring clientEndpointId;
    };

    void Subscribe(std::uint64_t cookie, Connection& connection) {
        connection.token = connection.connection.MessageReceived(
            [this, cookie](const midi2::IMidiMessageReceivedEventSource&, const midi2::MidiMessageReceivedEventArgs& args) {
                OnMessage(cookie, args);
            });
    }

    void OnMessage(std::uint64_t cookie, const midi2::MidiMessageReceivedEventArgs& args) noexcept {
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
            case midi2::MidiPacketType::UniversalMidiPacket32: {
                const auto message = packet.as<midi2::MidiMessage32>();
                words[0] = message.Word0();
                count = 1;
                break;
            }
            case midi2::MidiPacketType::UniversalMidiPacket64: {
                const auto message = packet.as<midi2::MidiMessage64>();
                words[0] = message.Word0(); words[1] = message.Word1();
                count = 2;
                break;
            }
            case midi2::MidiPacketType::UniversalMidiPacket96: {
                const auto message = packet.as<midi2::MidiMessage96>();
                words[0] = message.Word0(); words[1] = message.Word1(); words[2] = message.Word2();
                count = 3;
                break;
            }
            case midi2::MidiPacketType::UniversalMidiPacket128: {
                const auto message = packet.as<midi2::MidiMessage128>();
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
    midi2::MidiSession session_{nullptr};
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
