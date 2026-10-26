#include "native_audio_authenticated_channel_win32.h"
#include "unix_socket_stream_win32.h"
#include "schannel_stream_win32.h"
#include "pipe_identity_win32.h"
#include "channel_frames.h"
#include <algorithm>
#include <atomic>
#include <cstring>
namespace {
using namespace timox_broker;
using Code=NativeChannelCode;
class SocketChannel final:public INativeAudioAuthenticatedChannel {
public:
    SocketChannel(const NativeAudioChannelCredentials& credentials,const char* path){
        std::size_t length=0;
        if(path)while(length<name_.size()&&path[length])++length;
        valid_=length>0&&length<name_.size()&&credentials.clientCertificate&&
            std::any_of(credentials.serverCertificateSha256.begin(),credentials.serverCertificateSha256.end(),[](auto b){return b!=0;});
        if(length>0&&length<name_.size())std::copy_n(path,length,name_.data());
        auto socket=std::make_unique<UnixSocketStream>();socket_=socket.get();
        stream_=std::make_unique<SchannelStream>(std::move(socket),false,credentials.clientCertificate,credentials.serverCertificateSha256);
    }
    ~SocketChannel()override{RequestCancel();Close();}
    NativeChannelResult Connect(Deadline deadline)noexcept override{
        if(Cancelled())return Fail({Code::Cancelled});
        if(state_!=State::Disconnected)return Fail({Code::Failed,0,ERROR_INVALID_STATE});
        if(!valid_)return Fail({Code::AccessDenied,0,ERROR_INVALID_PARAMETER});
        const auto error=ObserveProcessIdentity(identity_);
        if(error)return Fail({Code::AccessDenied,0,error});
        auto result=socket_->Connect(name_.data(),deadline);if(result.code!=Code::Ok)return Fail(result);
        result=stream_->Handshake(deadline);if(result.code!=Code::Ok)return Fail(result);
        ChannelBinding binding;binding.identity=identity_;
        result=Bind(binding,BindingType::BindReply,deadline);if(result.code!=Code::Ok)return result;
        state_=State::BootstrapBound;return {Code::Ok};
    }
    NativeChannelResult ConfirmLease(std::uint64_t lease,std::uint64_t generation,Deadline deadline)noexcept override{
        if(Cancelled())return Fail({Code::Cancelled});
        if(state_!=State::LeasePending||lease!=lease_||generation!=generation_)return Fail({Code::Failed,0,ERROR_INVALID_STATE});
        ChannelBinding offer;
        auto result=ReadBindingFrame(*stream_,offer,deadline);if(result.code!=Code::Ok)return Fail(result);
        if(offer.type!=BindingType::DataOffer||offer.lease!=lease_||offer.generation!=generation_)return Fail({Code::AccessDenied});
        ChannelIdentity current;const auto error=ObserveProcessIdentity(current);
        if(error||!SameChannelIdentity(current,identity_))return Fail({Code::AccessDenied,0,error});
        // Same authenticated stream; no new path/connection or peer change.
        offer.type=BindingType::DataBindRequest;offer.identity=identity_;
        result=Bind(offer,BindingType::DataBindReply,deadline);if(result.code!=Code::Ok)return result;
        state_=State::DataBound;return {Code::Ok};
    }
    NativeChannelResult Exchange(const void* request,std::size_t requestBytes,void* response,std::size_t capacity,Deadline deadline)noexcept override{
        const auto fail=[&](NativeChannelResult result){
            if(response&&capacity<=MaxMessageBytes)std::memset(response,0,capacity);
            return Fail(result);
        };
        if(Cancelled())return fail({Code::Cancelled});
        if((state_!=State::BootstrapBound&&state_!=State::DataBound)||!request||requestBytes<HeaderBytes||
            requestBytes>MaxMessageBytes||!response||capacity<HeaderBytes||capacity>MaxMessageBytes)
            return fail({Code::Failed,0,ERROR_INVALID_PARAMETER});
        const auto* bytes=static_cast<const unsigned char*>(request);
        const auto opcode=static_cast<Opcode>(frames_detail::Read(bytes,6,2));
        if((state_==State::BootstrapBound)!=(opcode==Opcode::Session))return fail({Code::AccessDenied});
        Context context;context.opcode=opcode;context.lastRequestId=lastRequest_;context.leaseId=lease_;context.generation=generation_;
        const auto decoded=ValidateMessage(request,requestBytes,context);
        if(decoded.error!=Error::None)return fail({Code::Failed,0,ERROR_INVALID_DATA});
        context.response=true;context.requestId=decoded.message.requestId;
        if(opcode==Opcode::ReadRender)context.maxFrames=static_cast<std::uint32_t>(frames_detail::Read(bytes,56,4));
        auto& stream=*stream_;
        auto result=stream.WriteAll(request,requestBytes,deadline);
        if(result.code!=Code::Ok||result.bytes!=requestBytes)return fail(result.code==Code::Ok?NativeChannelResult{Code::Failed}:result);
        result=ReadAudioFrame(stream,response,capacity,deadline);
        if(result.code!=Code::Ok)return fail(result);
        const auto reply=ValidateMessage(response,result.bytes,context);
        if(reply.error!=Error::None)return fail({Code::Failed,0,ERROR_INVALID_DATA});
        if(Cancelled())return fail({Code::Cancelled});
        lastRequest_=context.requestId;
        if(opcode==Opcode::Session){
            if(reply.message.status==Status::Ok){
                lease_=reply.message.leaseId;generation_=frames_detail::Read(reply.message.payload,0,8);state_=State::LeasePending;
            }else Close();
        }else if(opcode==Opcode::Close)Close();
        return result;
    }
    void RequestCancel()noexcept override{
        cancelled_.store(true,std::memory_order_release);stream_->RequestCancel();
    }
    void Close()noexcept override{
        stream_->Close();state_=State::Closed;identity_={};lease_=generation_=lastRequest_=0;
    }
private:
    enum class State {Disconnected,BootstrapBound,LeasePending,DataBound,Closed};
    bool Cancelled()const noexcept{return cancelled_.load(std::memory_order_acquire);}
    NativeChannelResult Fail(NativeChannelResult result)noexcept{Close();result.bytes=0;return result;}
    NativeChannelResult Bind(ChannelBinding expected,BindingType type,Deadline deadline)noexcept{
        auto result=WriteBindingFrame(*stream_,expected,deadline);if(result.code!=Code::Ok)return Fail(result);
        ChannelBinding received;result=ReadBindingFrame(*stream_,received,deadline);if(result.code!=Code::Ok)return Fail(result);
        expected.type=type;if(!MatchesBinding(received,expected))return Fail({Code::AccessDenied});
        if(Cancelled())return Fail({Code::Cancelled});
        return {Code::Ok};
    }
    std::atomic<bool> cancelled_{false};
    bool valid_=false;State state_=State::Disconnected;
    std::array<char,108> name_{};
    UnixSocketStream* socket_=nullptr;
    std::unique_ptr<SchannelStream> stream_;
    ChannelIdentity identity_{};
    std::uint64_t lease_=0,generation_=0,lastRequest_=0;
};
}
std::unique_ptr<INativeAudioAuthenticatedChannel> CreateWindowsNativeAudioSocketChannel(
    const NativeAudioChannelCredentials& credentials,const char* trustedPath){
    return std::make_unique<SocketChannel>(credentials,trustedPath);
}
#if defined(TIMOX_NATIVE_CHANNEL_TEST_ENDPOINT)
std::size_t WindowsNativeAudioSocketChannelRetainedBytesForTest() noexcept{
    return sizeof(SocketChannel)+timox_broker::SchannelStream::RetainedApplicationBytes()+
        timox_broker::UnixSocketStream::FixedStorageBytes();
}
#endif
