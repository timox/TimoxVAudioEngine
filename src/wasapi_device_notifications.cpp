#include "wasapi_device_notifications.h"
#include <new>
#include <utility>

namespace {
class ChangeSink final:public IMMNotificationClient {
public:
    explicit ChangeSink(std::shared_ptr<std::atomic_bool> changed):changed_(std::move(changed)){}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** output) override {
        if(!output)return E_POINTER;
        *output=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(IMMNotificationClient))return E_NOINTERFACE;
        *output=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override{return references_.fetch_add(1)+1;}
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining=references_.fetch_sub(1)-1;
        if(!remaining)delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD) override{return Change();}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override{return Change();}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override{return Change();}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) override{return Change();}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY) override{return Change();}
private:
    HRESULT Change() noexcept{changed_->store(true,std::memory_order_release);return S_OK;}
    std::atomic<ULONG> references_{1};
    const std::shared_ptr<std::atomic_bool> changed_;
};
}
IMMNotificationClient* CreateWasapiDeviceChangeSink(std::shared_ptr<std::atomic_bool> changed) noexcept {
    return changed ? new(std::nothrow) ChangeSink(std::move(changed)) : nullptr;
}
HRESULT WasapiDeviceNotifications::Start() noexcept {
    if(registered_)return S_FALSE;
    auto hr=CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator_));
    if(FAILED(hr))return hr;
    try{changed_=std::make_shared<std::atomic_bool>(true);}catch(...){Stop();return E_OUTOFMEMORY;}
    sink_=CreateWasapiDeviceChangeSink(changed_);
    if(!sink_){Stop();return E_OUTOFMEMORY;}
    hr=enumerator_->RegisterEndpointNotificationCallback(sink_);
    if(FAILED(hr)){Stop();return hr;}
    registered_=true;return S_OK;
}
HRESULT WasapiDeviceNotifications::Stop() noexcept {
    const auto hr=registered_ ? enumerator_->UnregisterEndpointNotificationCallback(sink_) : S_OK;
    registered_=false;
    if(enumerator_){enumerator_->Release();enumerator_=nullptr;}
    // On an unexpected unregister failure retain the self-contained sink rather
    // than leave Windows a dangling callback. It holds only its atomic state,
    // never a controller/handle. No registration retry uses that old sink.
    if(sink_ && (SUCCEEDED(hr)||hr==E_NOTFOUND))sink_->Release();
    sink_=nullptr;changed_.reset();return hr;
}
bool WasapiDeviceNotifications::ConsumeChange() noexcept {
    return changed_ && changed_->exchange(false,std::memory_order_acq_rel);
}
