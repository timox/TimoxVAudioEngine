#include "schannel_stream_win32.h"
#include <security.h>
#include <winternl.h>
#include <bcrypt.h>
#define SCHANNEL_USE_BLACKLISTS
#include <schannel.h>
#include <algorithm>
#include <atomic>
#include <cstring>

namespace timox_broker {
namespace {
using Code=NativeChannelCode;
NativeChannelResult SspiError(SECURITY_STATUS status) noexcept {
    return {Code::AccessDenied,0,static_cast<std::uint32_t>(status)};
}
}
struct SchannelStream::Impl {
    const std::unique_ptr<ByteStream> pipe;
    const bool server;
    const Pin expected;
    PCCERT_CONTEXT certificate=nullptr;
    CredHandle credential{};CtxtHandle context{};
    bool started=false,ready=false,closed=false;
    std::atomic<bool> cancelled{false};
    unsigned long attributes=0;
    SecPkgContext_StreamSizes sizes{};
    // 67589 retained buffer bytes; handshake output accepted up to 32768.
    // Provider-owned internal allocations (including rejected larger output)
    // are opaque and are not claimed to have this application-memory bound.
    std::array<unsigned char,CipherBufferBytes> encrypted{};
    std::array<unsigned char,PlainBufferBytes> plaintext{};
    std::array<unsigned char,OutputBufferBytes> output{};
    std::size_t encryptedBytes=0,plainOffset=0,plainBytes=0;
    Impl(std::unique_ptr<ByteStream> stream,bool isServer,PCCERT_CONTEXT own,const Pin& pin)
        :pipe(std::move(stream)),server(isServer),expected(pin),certificate(own?CertDuplicateCertificateContext(own):nullptr){
        SecInvalidateHandle(&credential);SecInvalidateHandle(&context);
    }
    ~Impl(){Close();if(certificate)CertFreeCertificateContext(certificate);}
    void Close() noexcept {
        ready=false;closed=true;
        if(pipe)pipe->Close();
        if(SecIsValidHandle(&context)){DeleteSecurityContext(&context);SecInvalidateHandle(&context);}
        if(SecIsValidHandle(&credential)){FreeCredentialsHandle(&credential);SecInvalidateHandle(&credential);}
        SecureZeroMemory(encrypted.data(),encrypted.size());SecureZeroMemory(plaintext.data(),plaintext.size());
        SecureZeroMemory(output.data(),output.size());encryptedBytes=plainOffset=plainBytes=0;
    }
    NativeChannelResult Fail(SECURITY_STATUS error) noexcept {Close();return SspiError(error);}
    NativeChannelResult Receive(Deadline deadline) noexcept {
        if(encryptedBytes==encrypted.size())return Fail(SEC_E_BUFFER_TOO_SMALL);
        auto result=pipe->ReadSome(encrypted.data()+encryptedBytes,encrypted.size()-encryptedBytes,deadline);
        if(!detail::RetainCiphertextCompletion(result,encrypted.size(),encryptedBytes))return Fail(SEC_E_INVALID_TOKEN);
        if(result.code==Code::Ok && !result.bytes)return Fail(SEC_E_INVALID_TOKEN);
        if(result.code!=Code::Ok && result.code!=Code::TimedOut)Close();
        return result;
    }
    bool Verify() noexcept {
        if(!(attributes&(server?ASC_RET_CONFIDENTIALITY:ISC_RET_CONFIDENTIALITY)))return false;
        PCCERT_CONTEXT peer=nullptr;
        if(QueryContextAttributesW(&context,SECPKG_ATTR_REMOTE_CERT_CONTEXT,&peer)!=SEC_E_OK || !peer)return false;
        Pin hash{};DWORD hashBytes=static_cast<DWORD>(hash.size());
        bool valid=CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM,0,nullptr,peer->pbCertEncoded,peer->cbCertEncoded,hash.data(),&hashBytes)!=FALSE &&
            hashBytes==hash.size() && hash==expected && CertVerifyTimeValidity(nullptr,peer->pCertInfo)==0;
        DWORD bytes=0;
        alignas(CERT_ENHKEY_USAGE) std::array<unsigned char,4096> storage{};
        if(valid){
            valid=CertGetEnhancedKeyUsage(peer,CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,nullptr,&bytes)!=FALSE && bytes<=storage.size() && bytes>=sizeof(CERT_ENHKEY_USAGE);
            if(valid){
                auto* usage=reinterpret_cast<PCERT_ENHKEY_USAGE>(storage.data());
                valid=CertGetEnhancedKeyUsage(peer,CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG,usage,&bytes)!=FALSE;
                bool found=false;
                if(valid)for(DWORD i=0;i<usage->cUsageIdentifier;++i)
                    if(std::strcmp(usage->rgpszUsageIdentifier[i],server?szOID_PKIX_KP_CLIENT_AUTH:szOID_PKIX_KP_SERVER_AUTH)==0)found=true;
                valid=valid && found;
            }
        }
        CertFreeCertificateContext(peer);if(!valid)return false;
        SecPkgContext_ConnectionInfo connection{};SecPkgContext_CipherInfo cipher{};cipher.dwVersion=SECPKGCONTEXT_CIPHERINFO_V1;
        if(QueryContextAttributesW(&context,SECPKG_ATTR_CONNECTION_INFO,&connection)!=SEC_E_OK ||
            QueryContextAttributesW(&context,SECPKG_ATTR_CIPHER_INFO,&cipher)!=SEC_E_OK)return false;
        // Initial profile matches the qualified prototype: TLS1.2 ECDHE ECDSA
        // AES-GCM. Additional TLS versions/suites need their own interop tests.
        if(!(connection.dwProtocol&(SP_PROT_TLS1_2_CLIENT|SP_PROT_TLS1_2_SERVER)) ||
            (cipher.dwCipherSuite!=0xc02b && cipher.dwCipherSuite!=0xc02c))return false;
        if(QueryContextAttributesW(&context,SECPKG_ATTR_STREAM_SIZES,&sizes)!=SEC_E_OK)return false;
        return sizes.cbMaximumMessage>0 && sizes.cbMaximumMessage<=plaintext.size() &&
            static_cast<std::size_t>(sizes.cbHeader)+sizes.cbMaximumMessage+sizes.cbTrailer<=output.size();
    }
    NativeChannelResult Handshake(Deadline deadline) noexcept {
        if(cancelled.load(std::memory_order_acquire))return {Code::Cancelled,0,ERROR_OPERATION_ABORTED};
        if(!pipe || started || closed)return {Code::Failed,0,ERROR_INVALID_STATE};
        started=true;
        TLS_PARAMETERS tls{};tls.grbitDisabledProtocols=SP_PROT_SSL2|SP_PROT_SSL3|SP_PROT_TLS1|SP_PROT_TLS1_1|SP_PROT_TLS1_3;
        SCH_CREDENTIALS credentials{};credentials.dwVersion=SCH_CREDENTIALS_VERSION;
        credentials.cCreds=certificate?1:0;credentials.paCred=certificate?&certificate:nullptr;
        credentials.dwFlags=SCH_CRED_NO_DEFAULT_CREDS|SCH_CRED_MANUAL_CRED_VALIDATION|SCH_CRED_NO_SYSTEM_MAPPER;
        credentials.cTlsParameters=1;credentials.pTlsParameters=&tls;TimeStamp expiry{};
        auto status=AcquireCredentialsHandleW(nullptr,const_cast<wchar_t*>(UNISP_NAME_W),server?SECPKG_CRED_INBOUND:SECPKG_CRED_OUTBOUND,
            nullptr,&credentials,nullptr,nullptr,&credential,&expiry);
        if(status!=SEC_E_OK)return Fail(status);
        bool initial=true,needRead=server;
        for(;;){
            if(cancelled.load(std::memory_order_acquire)){Close();return {Code::Cancelled,0,ERROR_OPERATION_ABORTED};}
            if(std::chrono::steady_clock::now()>=deadline){Close();return {Code::TimedOut,0,WAIT_TIMEOUT};}
            if(needRead){const auto received=Receive(deadline);if(received.code!=Code::Ok){Close();return received;}}
            SecBuffer inputBuffers[2]={{static_cast<unsigned long>(encryptedBytes),SECBUFFER_TOKEN,encrypted.data()},{0,SECBUFFER_EMPTY,nullptr}};
            SecBufferDesc input{SECBUFFER_VERSION,2,inputBuffers};
            SecBuffer outputBuffers[2]={{0,SECBUFFER_TOKEN,nullptr},{0,SECBUFFER_ALERT,nullptr}};
            SecBufferDesc out{SECBUFFER_VERSION,2,outputBuffers};
            const bool hasContext=SecIsValidHandle(&context)!=FALSE;
            if(server)status=AcceptSecurityContext(&credential,hasContext?&context:nullptr,&input,
                ASC_REQ_STREAM|ASC_REQ_CONFIDENTIALITY|ASC_REQ_INTEGRITY|ASC_REQ_MUTUAL_AUTH|ASC_REQ_ALLOCATE_MEMORY,
                0,&context,&out,&attributes,&expiry);
            else status=InitializeSecurityContextW(&credential,hasContext?&context:nullptr,const_cast<wchar_t*>(L"Timox.NativeAudio"),
                ISC_REQ_STREAM|ISC_REQ_CONFIDENTIALITY|ISC_REQ_INTEGRITY|ISC_REQ_ALLOCATE_MEMORY|
                ISC_REQ_MANUAL_CRED_VALIDATION|ISC_REQ_USE_SUPPLIED_CREDS,0,0,
                initial?nullptr:&input,0,&context,&out,&attributes,&expiry);
            NativeChannelResult sent{Code::Ok};
            const bool outputBounded=static_cast<std::size_t>(outputBuffers[0].cbBuffer)+outputBuffers[1].cbBuffer<=MaxHandshakeTokenBytes;
            if(!outputBounded)sent={Code::Failed,0,ERROR_BUFFER_OVERFLOW};
            if(outputBuffers[0].pvBuffer){
                if(outputBounded && outputBuffers[0].cbBuffer)sent=pipe->WriteAll(outputBuffers[0].pvBuffer,outputBuffers[0].cbBuffer,deadline);
                FreeContextBuffer(outputBuffers[0].pvBuffer);
            }
            if(outputBuffers[1].pvBuffer)FreeContextBuffer(outputBuffers[1].pvBuffer);
            if(sent.code!=Code::Ok){Close();return sent;}
            if(status==SEC_E_INCOMPLETE_MESSAGE){needRead=true;continue;}
            initial=false;
            if(status!=SEC_E_OK && status!=SEC_I_CONTINUE_NEEDED)return Fail(status);
            std::size_t extra=0;
            if(inputBuffers[1].BufferType==SECBUFFER_EXTRA)extra=inputBuffers[1].cbBuffer;
            if(extra>encryptedBytes)return Fail(SEC_E_INVALID_TOKEN);
            if(extra)std::memmove(encrypted.data(),encrypted.data()+encryptedBytes-extra,extra);
            encryptedBytes=extra;
            if(status==SEC_E_OK){if(!Verify())return Fail(SEC_E_CERT_UNKNOWN);ready=true;return {Code::Ok};}
            needRead=encryptedBytes==0;
        }
    }
    NativeChannelResult ReadSome(void* buffer,std::size_t capacity,Deadline deadline) noexcept {
        if(cancelled.load(std::memory_order_acquire))return {Code::Cancelled,0,ERROR_OPERATION_ABORTED};
        if(!ready || !buffer || !capacity || capacity>ByteStream::MaxTransferBytes)return {Code::Failed,0,ERROR_INVALID_PARAMETER};
        for(;;){
            if(cancelled.load(std::memory_order_acquire)){Close();return {Code::Cancelled,0,ERROR_OPERATION_ABORTED};}
            if(std::chrono::steady_clock::now()>=deadline)return {Code::TimedOut,0,WAIT_TIMEOUT};
            if(plainBytes){const auto bytes=(std::min)(capacity,plainBytes);std::memcpy(buffer,plaintext.data()+plainOffset,bytes);
                plainOffset+=bytes;plainBytes-=bytes;return {Code::Ok,static_cast<std::uint32_t>(bytes)};}
            if(!encryptedBytes){const auto received=Receive(deadline);if(received.code!=Code::Ok)return received;}
            SecBuffer buffers[4]={{static_cast<unsigned long>(encryptedBytes),SECBUFFER_DATA,encrypted.data()},
                {0,SECBUFFER_EMPTY,nullptr},{0,SECBUFFER_EMPTY,nullptr},{0,SECBUFFER_EMPTY,nullptr}};
            SecBufferDesc description{SECBUFFER_VERSION,4,buffers};unsigned long qop=0;
            const auto status=DecryptMessage(&context,&description,0,&qop);
            if(status==SEC_E_INCOMPLETE_MESSAGE){const auto received=Receive(deadline);if(received.code!=Code::Ok)return received;continue;}
            if(status==SEC_I_CONTEXT_EXPIRED){Close();return {Code::Removed};}
            if(status!=SEC_E_OK)return Fail(status);
            if(qop==SECQOP_WRAP_NO_ENCRYPT)return Fail(SEC_E_MESSAGE_ALTERED);
            std::size_t extra=0;plainOffset=plainBytes=0;
            for(const auto& part:buffers){
                if(part.BufferType==SECBUFFER_EXTRA)extra=part.cbBuffer;
                if(part.BufferType==SECBUFFER_DATA && part.cbBuffer){
                    if(!part.pvBuffer || part.cbBuffer>plaintext.size()-plainBytes)return Fail(SEC_E_BUFFER_TOO_SMALL);
                    std::memcpy(plaintext.data()+plainBytes,part.pvBuffer,part.cbBuffer);plainBytes+=part.cbBuffer;
                }
            }
            if(extra>=encryptedBytes && extra!=0)return Fail(SEC_E_INVALID_TOKEN);
            if(extra)std::memmove(encrypted.data(),encrypted.data()+encryptedBytes-extra,extra);encryptedBytes=extra;
        }
    }
    NativeChannelResult WriteAll(const void* buffer,std::size_t bytes,Deadline deadline) noexcept {
        if(cancelled.load(std::memory_order_acquire))return {Code::Cancelled,0,ERROR_OPERATION_ABORTED};
        if(!ready || !buffer || !bytes || bytes>ByteStream::MaxTransferBytes)return {Code::Failed,0,ERROR_INVALID_PARAMETER};
        std::size_t total=0;
        while(total<bytes){
            if(cancelled.load(std::memory_order_acquire)){Close();return {Code::Cancelled,static_cast<std::uint32_t>(total),ERROR_OPERATION_ABORTED};}
            const auto count=(std::min)(bytes-total,static_cast<std::size_t>(sizes.cbMaximumMessage));
            std::memcpy(output.data()+sizes.cbHeader,static_cast<const unsigned char*>(buffer)+total,count);
            SecBuffer buffers[4]={{sizes.cbHeader,SECBUFFER_STREAM_HEADER,output.data()},
                {static_cast<unsigned long>(count),SECBUFFER_DATA,output.data()+sizes.cbHeader},
                {sizes.cbTrailer,SECBUFFER_STREAM_TRAILER,output.data()+sizes.cbHeader+count},{0,SECBUFFER_EMPTY,nullptr}};
            SecBufferDesc description{SECBUFFER_VERSION,4,buffers};const auto status=EncryptMessage(&context,0,&description,0);
            if(status!=SEC_E_OK){auto result=Fail(status);result.bytes=static_cast<std::uint32_t>(total);return result;}
            const auto encoded=static_cast<std::size_t>(buffers[0].cbBuffer)+buffers[1].cbBuffer+buffers[2].cbBuffer;
            if(encoded>output.size())return Fail(SEC_E_BUFFER_TOO_SMALL);
            auto result=pipe->WriteAll(output.data(),encoded,deadline);
            if(result.code!=Code::Ok){Close();result.bytes=static_cast<std::uint32_t>(total);return result;}
            total+=count;
        }
        return {Code::Ok,static_cast<std::uint32_t>(total)};
    }
};
SchannelStream::SchannelStream(std::unique_ptr<ByteStream> pipe,bool server,PCCERT_CONTEXT certificate,const Pin& pin)
    :impl_(std::make_unique<Impl>(std::move(pipe),server,certificate,pin)){}
SchannelStream::~SchannelStream()=default;
std::size_t SchannelStream::RetainedApplicationBytes() noexcept{return sizeof(SchannelStream)+sizeof(Impl);}
NativeChannelResult SchannelStream::Handshake(Deadline deadline) noexcept{return impl_->Handshake(deadline);}
NativeChannelResult SchannelStream::ReadSome(void* buffer,std::size_t capacity,Deadline deadline) noexcept{return impl_->ReadSome(buffer,capacity,deadline);}
NativeChannelResult SchannelStream::WriteAll(const void* buffer,std::size_t bytes,Deadline deadline) noexcept{return impl_->WriteAll(buffer,bytes,deadline);}
void SchannelStream::RequestCancel() noexcept{impl_->cancelled.store(true,std::memory_order_release);if(impl_->pipe)impl_->pipe->RequestCancel();}
void SchannelStream::Close() noexcept{impl_->Close();}
}
