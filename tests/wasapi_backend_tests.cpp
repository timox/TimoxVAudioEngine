#include "wasapi_backend.h"
#include <audioclient.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

void Validation() {
    auto backend = CreateWasapiBackend();
    Require(backend && !backend->Info(), "new backend unexpectedly open");
    WasapiOpenConfig config;
    Require(backend->Open(config)==E_INVALIDARG, "missing selector accepted");
    config.endpointId=L"opaque-id";
    config.defaultRole=WasapiDefaultRole::Console;
    Require(backend->Open(config)==E_INVALIDARG, "ambiguous selector accepted");
    config.defaultRole.reset();
    config.kind=WasapiStreamKind::ProcessLoopback;
    Require(backend->Open(config)==E_NOTIMPL, "process loopback not explicitly unsupported");
    config.kind=static_cast<WasapiStreamKind>(99);
    Require(backend->Open(config)==E_INVALIDARG, "unknown mode accepted");
    config.kind=WasapiStreamKind::Capture;
    config.endpointId.clear();
    config.defaultRole=static_cast<WasapiDefaultRole>(99);
    Require(backend->Open(config)==E_INVALIDARG, "invalid role accepted");
    config.defaultRole=WasapiDefaultRole::Multimedia;
    config.bufferDuration100ns=-1;
    Require(backend->Open(config)==E_INVALIDARG, "negative duration accepted");
    config.bufferDuration100ns=10000001;
    Require(backend->Open(config)==E_INVALIDARG, "unbounded duration accepted");
    Require(!backend->Info(), "failed open publishes format");

    WasapiMixFormat format;
    format.sampleRate=48000; format.channels=2; format.containerBits=32;
    format.validBits=32; format.blockAlign=8; format.encoding=WasapiSampleEncoding::IeeeFloat;
    Require(ValidateWasapiStreamFormat(format,480)==S_OK, "normal format rejected");
    auto invalid=format; invalid.channels=257;
    Require(FAILED(ValidateWasapiStreamFormat(invalid,480)), "unbounded channels");
    invalid=format; invalid.sampleRate=7999;
    Require(FAILED(ValidateWasapiStreamFormat(invalid,480)), "low sample rate accepted");
    invalid=format; invalid.sampleRate=384001;
    Require(FAILED(ValidateWasapiStreamFormat(invalid,480)), "high sample rate accepted");
    invalid=format; invalid.blockAlign=1;
    Require(FAILED(ValidateWasapiStreamFormat(invalid,480)), "bad alignment accepted");
    Require(FAILED(ValidateWasapiStreamFormat(format,0)), "empty buffer accepted");
    Require(FAILED(ValidateWasapiStreamFormat(format,48001)), "buffer over one second accepted");
    invalid=format; invalid.channels=256; invalid.sampleRate=384000; invalid.blockAlign=1024;
    Require(FAILED(ValidateWasapiStreamFormat(invalid,384000)), "huge byte buffer accepted");
}

void ResolverDiagnostic() {
    // No COM apartment on this thread: resolution fails before any device opens.
    auto backend = CreateWasapiBackend();
    WasapiOpenConfig config;
    config.endpointId=L"TimoxVasio-deterministic-missing-endpoint";
    Require(backend->Open(config)==E_FAIL, "resolver failure changed status");
    Require(std::strlen(backend->OpenErrorDetail())>0, "resolver diagnostic discarded");
    config.endpointId.clear();
    Require(backend->Open(config)==E_INVALIDARG, "invalid retry accepted");
    Require(std::strlen(backend->OpenErrorDetail())==0, "new Open retained stale diagnostic");
}

void ClosedState() {
    auto backend=CreateWasapiBackend();
    Require(backend->Start()==AUDCLNT_E_NOT_INITIALIZED, "closed Start succeeded");
    Require(backend->Stop()==S_OK, "closed Stop not idempotent");
    std::uint32_t frames=77;
    Require(backend->NextCapturePacketFrames(frames)==AUDCLNT_E_NOT_INITIALIZED && frames==0,
            "closed next packet retains stale output");
    WasapiCapturePacket packet;
    packet.frames=77;
    Require(backend->AcquireCapture(packet)==AUDCLNT_E_NOT_INITIALIZED && packet.frames==0 && !packet.data,
            "closed capture retains stale packet");
    Require(backend->ReleaseCapture(0)==AUDCLNT_E_NOT_INITIALIZED, "closed capture release succeeded");
    frames=77;
    Require(backend->RenderWritableFrames(frames)==AUDCLNT_E_NOT_INITIALIZED && frames==0,
            "closed render retains stale availability");
    std::uint8_t sentinel=0;
    std::uint8_t* data=&sentinel;
    Require(backend->AcquireRender(1,data)==AUDCLNT_E_NOT_INITIALIZED && !data,
            "closed render retains stale pointer");
    Require(backend->ReleaseRender(1,true)==AUDCLNT_E_NOT_INITIALIZED, "closed render release succeeded");
    const auto wait=backend->Wait(nullptr,0);
    Require(wait.kind==WasapiWaitKind::Failed && wait.status==AUDCLNT_E_NOT_INITIALIZED,
            "closed wait succeeded");
    backend->Close(); backend->Close();
    Require(!backend->Info(), "closed backend exposes stream info");
}

void Probe() {
    const HRESULT hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Require(SUCCEEDED(hr), "probe COM initialization failed");
    struct Apartment { ~Apartment(){ CoUninitialize(); } } apartment;
    WasapiDeviceEnumerator enumerator;
    std::vector<WasapiDeviceInfo> devices;
    std::string error;
    if (!enumerator.enumerate(devices,error)) {
        std::cout << "Inventory unavailable: " << error << '\n';
        return;
    }
    unsigned opened=0, unavailable=0;
    for (const auto& device : devices) {
        if (!(device.state & 1)) continue;
        for (unsigned attempt=0; attempt<(device.flow==WasapiDataFlow::Render ? 2U : 1U); ++attempt) {
            auto backend=CreateWasapiBackend();
            WasapiOpenConfig config;
            config.endpointId=device.id;
            config.kind=device.flow==WasapiDataFlow::Capture ? WasapiStreamKind::Capture :
                attempt==0 ? WasapiStreamKind::Render : WasapiStreamKind::EndpointLoopback;
            const HRESULT result=backend->Open(config);
            if (FAILED(result)) {
                ++unavailable;
                std::cout << "Open unavailable kind=" << static_cast<int>(config.kind)
                          << " HRESULT=0x" << std::hex << static_cast<unsigned long>(result) << std::dec
                          << " detail=" << backend->OpenErrorDetail() << '\n';
                continue;
            }
            ++opened;
            const auto* info=backend->Info();
            Require(info && info->endpointId==device.id && info->kind==config.kind, "probe identity mismatch");
            Require(SUCCEEDED(ValidateWasapiStreamFormat(info->format,info->bufferFrames)), "probe invalid effective format");
            std::cout << "Opened kind=" << static_cast<int>(info->kind) << " rate=" << info->format.sampleRate
                      << " channels=" << info->format.channels << " buffer=" << info->bufferFrames
                      << " latency100ns=" << info->streamLatency100ns << '\n';
            Require(backend->Open(config)==AUDCLNT_E_ALREADY_INITIALIZED && backend->Info()==info,
                    "second open replaces live stream");
            struct StopEvent {
                HANDLE handle=CreateEventW(nullptr,TRUE,TRUE,nullptr);
                ~StopEvent(){ if(handle) CloseHandle(handle); }
            } stop;
            Require(stop.handle!=nullptr, "probe event creation failed");
            const auto wait=backend->Wait(stop.handle,0);
            Require(wait.kind==WasapiWaitKind::Stop && SUCCEEDED(wait.status), "stop event not honored");
            if (config.kind==WasapiStreamKind::Render) {
                std::uint32_t writable=0;
                Require(SUCCEEDED(backend->RenderWritableFrames(writable)) && writable>=2, "fresh render has no space");
                std::uint8_t* data=nullptr;
                Require(backend->AcquireRender(2,data)==S_OK && data, "probe render acquire failed");
                Require(backend->ReleaseRender(1,false)==AUDCLNT_E_INVALID_SIZE, "partial release accepted");
                Require(backend->Stop()==AUDCLNT_E_OUT_OF_ORDER, "Stop ignores outstanding packet");
                Require(backend->Start()==AUDCLNT_E_OUT_OF_ORDER, "Start ignores outstanding packet");
                Require(backend->AcquireRender(2,data)==AUDCLNT_E_OUT_OF_ORDER && !data,
                        "invalid release lost packet ownership");
                Require(backend->ReleaseRender(0,false)==S_OK, "render cancel lost lease after invalid release");
                Require(backend->Stop()==S_OK, "stopped Stop not idempotent");
            }
            // No Start, capture data read, or audio submission in this opt-in probe.
            backend->Close();
            Require(!backend->Info(), "probe close retains stream");
        }
    }
    std::cout << "Probe inventory=" << devices.size() << " opened=" << opened << " unavailable=" << unavailable << '\n';
}

int ProbeProcessLoopback() {
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment)) {
        std::cout << "COM initialization failed (HRESULT 0x" << std::hex
                  << static_cast<unsigned long>(apartment) << std::dec << ")\n";
        return 2;
    }
    struct Apartment { ~Apartment() { CoUninitialize(); } } release;
    WasapiDeviceEnumerator enumerator;
    std::vector<WasapiProcessInfo> processes;
    std::string error;
    if (!enumerator.enumerateActiveRenderProcesses(processes, error)) {
        std::cout << "Process inventory failed: " << error << '\n';
        return 2;
    }
    bool opened = false;
    for (const auto& process : processes) {
        auto backend = CreateWasapiBackend();
        WasapiOpenConfig config;
        config.kind = WasapiStreamKind::ProcessLoopback;
        config.processId = process.processId;
        config.processCreationTime = process.creationTime;
        config.processImagePath = process.imagePath;
        const HRESULT result = backend->Open(config);
        std::cout << "Process-loopback PID=" << process.processId << " HRESULT=0x"
                  << std::hex << static_cast<unsigned long>(result) << std::dec
                  << " detail=" << backend->OpenErrorDetail();
        if (SUCCEEDED(result)) {
            const auto* info = backend->Info();
            std::cout << " rate=" << (info ? info->format.sampleRate : 0)
                      << " channels=" << (info ? info->format.channels : 0)
                      << " frames=" << (info ? info->bufferFrames : 0);
            const HRESULT started = backend->Start();
            std::cout << " start=0x" << std::hex << static_cast<unsigned long>(started) << std::dec;
            const HRESULT stopped = SUCCEEDED(started) ? backend->Stop() : started;
            std::cout << " stop=0x" << std::hex << static_cast<unsigned long>(stopped) << std::dec;
            opened = SUCCEEDED(started) && SUCCEEDED(stopped);
        }
        std::cout << '\n';
        backend->Close();
    }
    if (processes.empty()) std::cout << "No active shared-mode render processes found\n";
    std::cout << "Probe starts then stops each stream without reading audio packets or changing saved routes.\n";
    return opened ? 0 : 1;
}
}

int main(int argc,char** argv) {
    try {
        if (argc == 2 && std::strcmp(argv[1], "--probe-process-loopback") == 0)
            return ProbeProcessLoopback();
        Validation(); ClosedState(); ResolverDiagnostic();
        if (argc>1 && std::strcmp(argv[1],"--probe")==0) Probe();
        std::cout << "WASAPI backend deterministic tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
