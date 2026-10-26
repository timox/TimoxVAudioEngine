#include "native_audio_installed_transport_win32.h"
#include "native_audio_authenticated_channel_win32.h"
#include "native_audio_broker_transport.h"
#include "socket_discovery_root_win32.h"
#include "socket_discovery_win32.h"
#include <Windows.h>
#include <ncrypt.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

namespace {
class Handle final {
public:
    explicit Handle(HANDLE value=INVALID_HANDLE_VALUE) noexcept:value_(value){}
    ~Handle(){if(value_&&value_!=INVALID_HANDLE_VALUE)CloseHandle(value_);}
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    HANDLE get()const noexcept{return value_;}
private: HANDLE value_;
};
class Store final {
public:
    explicit Store(HCERTSTORE value=nullptr) noexcept:value_(value){}
    ~Store(){if(value_)CertCloseStore(value_,0);}
    Store(const Store&)=delete;
    Store& operator=(const Store&)=delete;
    HCERTSTORE get()const noexcept{return value_;}
private: HCERTSTORE value_;
};
class Certificate final {
public:
    explicit Certificate(PCCERT_CONTEXT value=nullptr) noexcept:value_(value){}
    ~Certificate(){if(value_)CertFreeCertificateContext(value_);}
    Certificate(const Certificate&)=delete;
    Certificate& operator=(const Certificate&)=delete;
    Certificate(Certificate&& other)noexcept:value_(other.release()){}
    Certificate& operator=(Certificate&& other)noexcept{
        if(this!=&other){if(value_)CertFreeCertificateContext(value_);value_=other.release();}return *this;
    }
    PCCERT_CONTEXT get()const noexcept{return value_;}
    PCCERT_CONTEXT release()noexcept{auto* value=value_;value_=nullptr;return value;}
private: PCCERT_CONTEXT value_;
};

class FailedTransport final:public INativeAudioTransport {
public:
    FailedTransport(NativeTransferCode code,DWORD error)noexcept:result_{code,0,error}{}
    NativeTransferResult Open(NativeSessionInfo& info)noexcept override{info={};return result_;}
    NativeTransferResult ReadRender(std::uint32_t,void* packet,std::uint32_t capacity)noexcept override{
        if(packet&&capacity<=32832)std::memset(packet,0,capacity);return result_;
    }
    NativeTransferResult WriteCapture(const void*,std::uint32_t)noexcept override{return result_;}
    NativeTransferResult ReadStatus(std::uint64_t (&counters)[3])noexcept override{
        counters[0]=counters[1]=counters[2]=0;return result_;
    }
    void RequestCancel()noexcept override{}
    void Close()noexcept override{}
private:NativeTransferResult result_;
};

std::unique_ptr<INativeAudioTransport> Failure(NativeTransferCode code,DWORD error){
    return std::make_unique<FailedTransport>(code,error);
}

DWORD ReadEnrolledClientHash(std::array<BYTE,32>& hash) noexcept {
    HKEY key=nullptr;
    const auto opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Timox\\NativeAudioBroker\\Runtime",0,
        KEY_QUERY_VALUE|KEY_WOW64_64KEY,&key);
    if(opened!=ERROR_SUCCESS)return opened;
    DWORD type=0,bytes=static_cast<DWORD>(hash.size());
    const auto queried=RegQueryValueExW(key,L"ClientCertificateSha256",nullptr,&type,hash.data(),&bytes);
    RegCloseKey(key);
    if(queried!=ERROR_SUCCESS)return queried;
    if(type!=REG_BINARY||bytes!=hash.size()||
        std::all_of(hash.begin(),hash.end(),[](BYTE value){return value==0;}))return ERROR_INVALID_DATA;
    return ERROR_SUCCESS;
}

bool HasClientAuthEku(PCCERT_CONTEXT certificate) {
    std::array<BYTE,4096> storage{};DWORD bytes=static_cast<DWORD>(storage.size());
    auto* usage=reinterpret_cast<PCERT_ENHKEY_USAGE>(storage.data());
    if(!CertGetEnhancedKeyUsage(certificate,CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,usage,&bytes))return false;
    for(DWORD i=0;i<usage->cUsageIdentifier;++i)
        if(usage->rgpszUsageIdentifier[i]&&std::strcmp(usage->rgpszUsageIdentifier[i],szOID_PKIX_KP_CLIENT_AUTH)==0)return true;
    return false;
}

bool HasPrivateKey(PCCERT_CONTEXT certificate) noexcept {
    HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key=0;DWORD keySpec=0;BOOL release=false;
    if(!CryptAcquireCertificatePrivateKey(certificate,CRYPT_ACQUIRE_SILENT_FLAG|
        CRYPT_ACQUIRE_PREFER_NCRYPT_KEY_FLAG,nullptr,&key,&keySpec,&release))return false;
    if(release){
        if(keySpec==CERT_NCRYPT_KEY_SPEC)NCryptFreeObject(key);
        else CryptReleaseContext(static_cast<HCRYPTPROV>(key),0);
    }
    return true;
}

DWORD FindEnrolledClientCertificate(const std::array<BYTE,32>& expected,PCCERT_CONTEXT& output) {
    if(output)return ERROR_INVALID_PARAMETER;
    Store store(CertOpenStore(CERT_STORE_PROV_SYSTEM_W,X509_ASN_ENCODING|PKCS_7_ASN_ENCODING,
        0,CERT_SYSTEM_STORE_CURRENT_USER|CERT_STORE_READONLY_FLAG,L"MY"));
    if(!store.get())return GetLastError();
    PCCERT_CONTEXT match=nullptr;
    PCCERT_CONTEXT current=nullptr;
    while((current=CertEnumCertificatesInStore(store.get(),current))!=nullptr){
        std::array<BYTE,32> hash{};DWORD hashBytes=static_cast<DWORD>(hash.size());
        if(!CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,current->pbCertEncoded,
            current->cbCertEncoded,hash.data(),&hashBytes)||hashBytes!=hash.size()||hash!=expected)continue;
        if(match){CertFreeCertificateContext(match);CertFreeCertificateContext(current);return ERROR_INVALID_DATA;}
        if(CertVerifyTimeValidity(nullptr,current->pCertInfo)!=0||!HasClientAuthEku(current)||!HasPrivateKey(current)){
            CertFreeCertificateContext(current);return ERROR_ACCESS_DENIED;
        }
        match=CertDuplicateCertificateContext(current);
        if(!match){CertFreeCertificateContext(current);return ERROR_NOT_ENOUGH_MEMORY;}
    }
    const auto enumerationError=GetLastError();
    if(!match)return enumerationError==CRYPT_E_NOT_FOUND?CRYPT_E_NOT_FOUND:ERROR_NOT_FOUND;
    output=match;return ERROR_SUCCESS;
}

DWORD BuildInstalledSocketPath(std::array<char,108>& output,std::array<BYTE,32>& pin) noexcept {
    try {
    HANDLE raw=INVALID_HANDLE_VALUE;
    auto error=timox_broker::OpenInstalledSocketDiscoveryRoot(raw);if(error)return error;
    Handle root(raw);
    timox_broker::SocketDiscovery discovery;
    error=timox_broker::ReadSocketDiscoveryDirectory(root.get(),discovery);if(error)return error;
    timox_broker::SocketBasename basename{};
    if(!timox_broker::SocketDiscoveryBasename(discovery,basename))return ERROR_INVALID_DATA;
    std::array<wchar_t,512> wide{};
    const auto characters=GetFinalPathNameByHandleW(root.get(),wide.data(),static_cast<DWORD>(wide.size()),
        FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    if(!characters)return GetLastError();
    if(characters>=wide.size())return ERROR_BUFFER_OVERFLOW;
    std::wstring directory(wide.data(),characters);
    if(directory.rfind(L"\\\\?\\UNC\\",0)==0)return ERROR_NOT_SUPPORTED;
    if(directory.rfind(L"\\\\?\\",0)==0)directory.erase(0,4);
    directory.push_back(L'\\');
    for(const auto value:basename){if(!value)break;directory.push_back(static_cast<wchar_t>(value));}
    const auto required=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,directory.c_str(),-1,nullptr,0,nullptr,nullptr);
    if(required<=0)return GetLastError();
    if(static_cast<std::size_t>(required)>output.size())return ERROR_BUFFER_OVERFLOW;
    std::array<char,108> encoded{};
    if(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,directory.c_str(),-1,encoded.data(),required,nullptr,nullptr)!=required)
        return GetLastError();
    output=encoded;pin=discovery.serverPin;return ERROR_SUCCESS;
    } catch(...) {return ERROR_NOT_ENOUGH_MEMORY;}
}
}

std::unique_ptr<INativeAudioTransport> CreateInstalledNativeAudioTransport(){
    std::array<BYTE,32> clientHash{};
    auto error=ReadEnrolledClientHash(clientHash);
    if(error)return Failure(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?NativeTransferCode::Unavailable:
        NativeTransferCode::AccessDenied,error);
    Certificate certificate;
    PCCERT_CONTEXT found=nullptr;
    error=FindEnrolledClientCertificate(clientHash,found);
    certificate=Certificate(found);
    if(error)return Failure(error==CRYPT_E_NOT_FOUND?NativeTransferCode::Unavailable:NativeTransferCode::AccessDenied,error);
    std::array<char,108> path{};std::array<BYTE,32> serverPin{};
    error=BuildInstalledSocketPath(path,serverPin);
    if(error)return Failure(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND?NativeTransferCode::Unavailable:
        NativeTransferCode::AccessDenied,error);
    NativeAudioChannelCredentials credentials;credentials.clientCertificate=certificate.get();
    credentials.serverCertificateSha256=serverPin;
    auto channel=CreateWindowsNativeAudioSocketChannel(credentials,path.data());
    return CreateBrokerNativeAudioTransport(std::move(channel));
}
