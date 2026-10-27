#include "native_audio_broker_transport.h"
#include "native_audio_broker_protocol.h"
#include <array>
#include <atomic>
#include <cstring>
#include <limits>

namespace {
using namespace timox_broker;
constexpr auto OperationTimeout=std::chrono::seconds(5);
void Put(void* memory,std::size_t at,std::uint64_t value,unsigned width) noexcept {
    auto* p=static_cast<unsigned char*>(memory);
    for(unsigned i=0;i<width;++i)p[at+i]=static_cast<unsigned char>(value>>(i*8));
}
std::uint64_t Read(const void* memory,std::size_t at,unsigned width) noexcept {
    return timox_native::detail::ReadLittleEndian(static_cast<const unsigned char*>(memory),at,width);
}
NativeTransferResult FromChannel(NativeChannelResult result) noexcept {
    NativeTransferCode code=NativeTransferCode::Failed;
    switch(result.code){
    case NativeChannelCode::Ok:code=NativeTransferCode::Ok;break;
    case NativeChannelCode::Unavailable:code=NativeTransferCode::Unavailable;break;
    case NativeChannelCode::AccessDenied:code=NativeTransferCode::AccessDenied;break;
    case NativeChannelCode::Removed:code=NativeTransferCode::Removed;break;
    case NativeChannelCode::Cancelled:code=NativeTransferCode::Cancelled;break;
    case NativeChannelCode::TimedOut:case NativeChannelCode::Failed:break;
    }
    return {code,0,result.systemError};
}
NativeTransferCode FromBroker(Status status) noexcept {
    switch(status){
    case Status::Ok:return NativeTransferCode::Ok;
    case Status::Empty:return NativeTransferCode::Empty;
    case Status::Full:return NativeTransferCode::Full;
    case Status::Unavailable:return NativeTransferCode::Unavailable;
    case Status::AccessDenied:case Status::Protected:return NativeTransferCode::AccessDenied;
    case Status::OwnedElsewhere:return NativeTransferCode::OwnedElsewhere;
    case Status::Removed:return NativeTransferCode::Removed;
    case Status::Cancelled:return NativeTransferCode::Cancelled;
    case Status::InvalidPacket:case Status::ProtocolMismatch:return NativeTransferCode::InvalidPacket;
    case Status::Failed:return NativeTransferCode::Failed;
    }
    return NativeTransferCode::InvalidPacket;
}
class BrokerTransport final:public INativeAudioTransport {
public:
    explicit BrokerTransport(std::unique_ptr<INativeAudioAuthenticatedChannel> channel):channel_(std::move(channel)){}
    ~BrokerTransport()override{RequestCancel();EndChannel();}
    NativeTransferResult Open(NativeSessionInfo& info)noexcept override{
        info={};
        if(Cancelled())return {NativeTransferCode::Cancelled};
        if(!channel_)return {NativeTransferCode::Unavailable};
        if(channelStarted_)return {NativeTransferCode::Failed};
        const auto deadline=Deadline();channelStarted_=true;
        const auto connected=channel_->Connect(deadline);
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(connected.code!=NativeChannelCode::Ok)return Abort(FromChannel(connected));
        if(connected.bytes)return Abort({NativeTransferCode::InvalidPacket});
        auto result=Exchange(Opcode::Session,0,4096,deadline);
        if(result.code!=NativeTransferCode::Ok)return result;
        // SESSION response is validated before reading nonzero lease/generation.
        lease_=Read(reply_.data(),24,8);generation_=Read(reply_.data(),HeaderBytes,8);
        const auto bound=channel_->ConfirmLease(lease_,generation_,deadline);
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(bound.code!=NativeChannelCode::Ok)return Abort(FromChannel(bound));
        if(bound.bytes)return Abort({NativeTransferCode::InvalidPacket});
        info.wireGeneration=generation_;return {NativeTransferCode::Ok};
    }
    NativeTransferResult ReadRender(std::uint32_t frames,void* packet,std::uint32_t capacity)noexcept override{
        // Silence only a bounded caller span. Never expose partially validated
        // replies, diagnostics or PCM from a previous lease.
        if(packet && capacity<=MaxPayloadBytes)std::memset(packet,0,capacity);
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(!packet || !frames || frames>4096 || capacity>MaxPayloadBytes || capacity<64+frames*8)
            return {NativeTransferCode::InvalidPacket};
        if(!generation_)return {NativeTransferCode::Removed};
        Put(request_.data(),HeaderBytes,generation_,8);Put(request_.data(),HeaderBytes+8,frames,4);Put(request_.data(),HeaderBytes+12,0,4);
        auto result=Exchange(Opcode::ReadRender,16,frames,Deadline());
        if(result.code!=NativeTransferCode::Ok)return result;
        const auto* payload=reply_.data()+HeaderBytes;
        const auto sequence=Read(payload,24,8);
        if(renderSeen_ && sequence<=renderSequence_)return Abort({NativeTransferCode::InvalidPacket});
        renderSeen_=true;renderSequence_=sequence;
        // Codec checked packet length against frames; hence <= caller capacity.
        std::memcpy(packet,payload,result.bytes);return result;
    }
    NativeTransferResult WriteCapture(const void* packet,std::uint32_t bytes)noexcept override{
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(!generation_)return {NativeTransferCode::Removed};
        if(!packet || bytes>MaxPayloadBytes)return {NativeTransferCode::InvalidPacket};
        const auto valid=timox_native::ValidatePacket(packet,bytes,{generation_,2,4096});
        if(valid.error!=timox_native::PacketError::None || (captureSeen_ && valid.metadata.sequence<=captureSequence_))
            return {NativeTransferCode::InvalidPacket};
        std::memcpy(request_.data()+HeaderBytes,packet,bytes);
        auto result=Exchange(Opcode::WriteCapture,bytes,4096,Deadline());
        if(result.code==NativeTransferCode::Ok){captureSeen_=true;captureSequence_=valid.metadata.sequence;}
        // Full does not advance sequence. Broker retains exact pending N0 bytes;
        // a retry gets a new request ID and is independently checked there.
        return result;
    }
    NativeTransferResult ReadStatus(std::uint64_t (&counters)[3])noexcept override{
        for(auto& value:counters)value=0;
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(!generation_)return {NativeTransferCode::Removed};
        Put(request_.data(),HeaderBytes,generation_,8);
        auto result=Exchange(Opcode::Status,8,4096,Deadline());
        if(result.code==NativeTransferCode::Ok)
            for(unsigned i=0;i<3;++i)counters[i]=Read(reply_.data(),HeaderBytes+i*8,8);
        return result;
    }
    void RequestCancel()noexcept override{
        cancelled_.store(true,std::memory_order_release);
        // Fixed pointer/lifetime; no worker-owned state is read here.
        if(channel_)channel_->RequestCancel();
    }
    void Close()noexcept override{
        if(generation_ && !Cancelled()){
            Put(request_.data(),HeaderBytes,generation_,8);
            Exchange(Opcode::Close,8,4096,Deadline());
        }
        EndChannel();
    }
private:
    using TimePoint=INativeAudioAuthenticatedChannel::Deadline;
    static TimePoint Deadline()noexcept{return std::chrono::steady_clock::now()+OperationTimeout;}
    bool Cancelled()const noexcept{return cancelled_.load(std::memory_order_acquire);}
    void EndChannel()noexcept{
        if(channelStarted_){channel_->Close();channelStarted_=false;}
        lease_=generation_=renderSequence_=captureSequence_=0;renderSeen_=captureSeen_=false;
        nextRequest_=1;requestsExhausted_=false;request_.fill(0);reply_.fill(0);
    }
    NativeTransferResult Abort(NativeTransferResult result)noexcept{EndChannel();return result;}
    NativeTransferResult Exchange(Opcode opcode,std::uint32_t payloadBytes,std::uint32_t frames,TimePoint deadline)noexcept{
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(!channelStarted_)return {NativeTransferCode::Removed};
        if(requestsExhausted_)return Abort({NativeTransferCode::InvalidPacket});
        const auto id=nextRequest_;
        if(id==(std::numeric_limits<std::uint64_t>::max)())requestsExhausted_=true;
        else ++nextRequest_;
        std::memset(request_.data(),0,HeaderBytes);
        Put(request_.data(),0,Magic,4);Put(request_.data(),4,1,2);Put(request_.data(),6,static_cast<unsigned>(opcode),2);
        Put(request_.data(),8,HeaderBytes,4);Put(request_.data(),12,payloadBytes,4);Put(request_.data(),16,id,8);
        Put(request_.data(),24,opcode==Opcode::Session?0:lease_,8);
        const auto response=channel_->Exchange(request_.data(),HeaderBytes+payloadBytes,reply_.data(),reply_.size(),deadline);
        if(Cancelled())return Abort({NativeTransferCode::Cancelled});
        if(response.code!=NativeChannelCode::Ok)return Abort(FromChannel(response));
        if(response.bytes>MaxMessageBytes)return Abort({NativeTransferCode::InvalidPacket});
        const auto decoded=ValidateMessage(reply_.data(),response.bytes,{true,opcode,id,0,lease_,generation_,frames});
        if(decoded.error!=Error::None)return Abort({NativeTransferCode::InvalidPacket});
        const auto& message=decoded.message;
        if(message.status==Status::Ok)return {NativeTransferCode::Ok,message.payloadBytes};
        const auto error=static_cast<std::uint32_t>(Read(message.payload,0,4));
        if(message.status==Status::Empty && opcode==Opcode::ReadRender)return {NativeTransferCode::Empty,0,error};
        if(message.status==Status::Full && opcode==Opcode::WriteCapture)return {NativeTransferCode::Full,0,error};
        if(message.status==Status::Empty || message.status==Status::Full)return Abort({NativeTransferCode::InvalidPacket});
        // Protected is mapped only from the explicit broker-domain status, never
        // inferred from an ambiguous Win32 AccessDenied diagnostic.
        return Abort({FromBroker(message.status),0,error});
    }
    const std::unique_ptr<INativeAudioAuthenticatedChannel> channel_;
    std::atomic<bool> cancelled_{false};bool channelStarted_=false;
    std::uint64_t lease_=0,generation_=0,nextRequest_=1,renderSequence_=0,captureSequence_=0;
    bool requestsExhausted_=false,renderSeen_=false,captureSeen_=false;
    std::array<unsigned char,MaxMessageBytes> request_{},reply_{};
};
static_assert(sizeof(BrokerTransport)<=2*MaxMessageBytes+1024,"adapter metadata exceeds channel memory accounting");
}
std::unique_ptr<INativeAudioTransport> CreateBrokerNativeAudioTransport(std::unique_ptr<INativeAudioAuthenticatedChannel> channel){return std::make_unique<BrokerTransport>(std::move(channel));}
