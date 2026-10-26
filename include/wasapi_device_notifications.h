#pragma once
#include <Windows.h>
#include <mmdeviceapi.h>
#include <atomic>
#include <memory>

// Returns one owning COM reference. Callbacks only set an atomic flag; they do
// not inspect endpoint IDs, enumerate, allocate, wait or access the controller.
IMMNotificationClient* CreateWasapiDeviceChangeSink(std::shared_ptr<std::atomic_bool>) noexcept;

// Start/ConsumeChange/Stop/destruction belong to the same COM-initialized worker.
// Keep that apartment alive until Stop. No audio callback uses this object.
class WasapiDeviceNotifications {
public:
    WasapiDeviceNotifications()=default;
    ~WasapiDeviceNotifications(){Stop();}
    WasapiDeviceNotifications(const WasapiDeviceNotifications&)=delete;
    WasapiDeviceNotifications& operator=(const WasapiDeviceNotifications&)=delete;
    HRESULT Start() noexcept;
    HRESULT Stop() noexcept;
    bool ConsumeChange() noexcept;
private:
    IMMDeviceEnumerator* enumerator_=nullptr;
    IMMNotificationClient* sink_=nullptr;
    std::shared_ptr<std::atomic_bool> changed_;
    bool registered_=false;
};
