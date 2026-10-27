#include "wasapi_device_notifications.h"
#include <iostream>
#include <stdexcept>
#include <thread>

void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
int main(){
    try {
        auto changed=std::make_shared<std::atomic_bool>(false);
        auto* sink=CreateWasapiDeviceChangeSink(changed);
        Check(sink!=nullptr,"create notification sink");
        void* object=reinterpret_cast<void*>(1);
        Check(sink->QueryInterface(IID_IUnknown,&object)==S_OK&&object,"IUnknown query");
        static_cast<IUnknown*>(object)->Release();
        Check(sink->QueryInterface(__uuidof(IMMDeviceEnumerator),&object)==E_NOINTERFACE&&object==nullptr,"unexpected interface");
        Check(sink->QueryInterface(IID_IUnknown,nullptr)==E_POINTER,"null interface output");
        auto consume=[&]{Check(changed->exchange(false),"notification lost");Check(!changed->exchange(false),"notification not consumed");};
        sink->OnDeviceAdded(nullptr);consume();
        sink->OnDeviceRemoved(L"opaque");consume();
        sink->OnDeviceStateChanged(L"opaque",DEVICE_STATE_DISABLED);consume();
        sink->OnDefaultDeviceChanged(eCapture,eCommunications,nullptr);consume();
        PROPERTYKEY key{};sink->OnPropertyValueChanged(L"opaque",key);consume();
        std::thread producer([&]{for(unsigned i=0;i<10000;++i)sink->OnDeviceAdded(nullptr);});
        producer.join();consume();
        // Callback state survives the consumer's ownership; no controller pointer.
        changed.reset();sink->OnDeviceRemoved(nullptr);sink->Release();
        WasapiDeviceNotifications notifications;
        Check(FAILED(notifications.Start()),"registration without COM unexpectedly succeeded");
        const auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        Check(SUCCEEDED(apartment),"initialize COM");
        const auto registered=notifications.Start();
        const auto second=notifications.Start();
        const auto stopped=notifications.Stop();
        CoUninitialize();
        Check(SUCCEEDED(registered)&&second==S_FALSE&&SUCCEEDED(stopped),"real registration/unregistration failed");
        Check(!notifications.ConsumeChange(),"stopped observer reports change");
        std::cout<<"WASAPI notification callbacks and real registration lifecycle: PASS\n";
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
