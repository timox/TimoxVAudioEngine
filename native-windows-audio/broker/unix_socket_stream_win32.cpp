#include <winsock2.h>
#include <afunix.h>
#include "unix_socket_stream_win32.h"
#include <algorithm>
#include <atomic>
#include <cstring>

namespace timox_broker {
namespace {
using Code=NativeChannelCode;
using Clock=std::chrono::steady_clock;
NativeChannelResult Error(int error) noexcept {
    Code code=Code::Failed;
    if(error==WSAEACCES)code=Code::AccessDenied;
    if(error==WSAECONNREFUSED||error==WSAEADDRNOTAVAIL)code=Code::Unavailable;
    if(error==WSAECONNRESET||error==WSAECONNABORTED||error==WSAESHUTDOWN)code=Code::Removed;
    return {code,0,static_cast<std::uint32_t>(error)};
}
bool Address(const char* path,sockaddr_un& address) noexcept {
    if(!path||!path[0])return false;
    std::size_t length=0;while(length<sizeof(address.sun_path)&&path[length])++length;
    if(length==sizeof(address.sun_path))return false;
    address.sun_family=AF_UNIX;std::memcpy(address.sun_path,path,length+1);return true;
}
bool Nonblocking(SOCKET socket) noexcept {u_long yes=1;return ioctlsocket(socket,FIONBIO,&yes)==0;}
}
struct UnixSocketStream::Impl {
    SOCKET socket=INVALID_SOCKET;
    bool listening=false,connected=false;
    std::atomic<bool> cancelled{false};
    int startup=0;
    Impl(){WSADATA data{};startup=WSAStartup(MAKEWORD(2,2),&data);}
    ~Impl(){Close();if(!startup)WSACleanup();}
    void Close() noexcept {
        if(socket!=INVALID_SOCKET){closesocket(socket);socket=INVALID_SOCKET;}
        listening=false;connected=false;
    }
    NativeChannelResult Ready(Deadline deadline) const noexcept {
        if(cancelled.load(std::memory_order_acquire))return {Code::Cancelled};
        if(startup)return Error(startup);
        if(Clock::now()>=deadline)return {Code::TimedOut};
        return {Code::Ok};
    }
    NativeChannelResult Create() noexcept {
        if(socket!=INVALID_SOCKET)return {Code::Failed};
        socket=::socket(AF_UNIX,SOCK_STREAM,0);
        if(socket==INVALID_SOCKET)return Error(WSAGetLastError());
        if(!Nonblocking(socket)){const auto result=Error(WSAGetLastError());Close();return result;}
        return {Code::Ok};
    }
    NativeChannelResult Wait(bool writing,Deadline deadline) noexcept {
        for(;;){
            auto result=Ready(deadline);if(result.code!=Code::Ok)return result;
            const auto remaining=std::chrono::duration_cast<std::chrono::microseconds>(deadline-Clock::now()).count();
            if(remaining<=0)return {Code::TimedOut};
            timeval timeout{0,static_cast<long>(std::min<std::int64_t>(remaining,10000))};
            fd_set ready,errors;FD_ZERO(&ready);FD_ZERO(&errors);FD_SET(socket,&ready);FD_SET(socket,&errors);
            const int count=select(0,writing?nullptr:&ready,writing?&ready:nullptr,&errors,&timeout);
            if(count==SOCKET_ERROR)return Error(WSAGetLastError());
            result=Ready(deadline);if(result.code!=Code::Ok)return result;
            if(count){
                if(FD_ISSET(socket,&errors)){
                    int error=0,size=sizeof(error);
                    if(getsockopt(socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)==SOCKET_ERROR)return Error(WSAGetLastError());
                    return error?Error(error):NativeChannelResult{Code::Failed};
                }
                return {Code::Ok};
            }
        }
    }
};
UnixSocketStream::UnixSocketStream():impl_(std::make_unique<Impl>()){}
UnixSocketStream::~UnixSocketStream()=default;
std::size_t UnixSocketStream::FixedStorageBytes() noexcept{return sizeof(UnixSocketStream)+sizeof(Impl);}
void UnixSocketStream::RequestCancel() noexcept {impl_->cancelled.store(true,std::memory_order_release);}
void UnixSocketStream::Close() noexcept {impl_->Close();}
NativeChannelResult UnixSocketStream::GetPeerProcessId(std::uint32_t& output) noexcept {
    auto result=impl_->Ready(Deadline::max());if(result.code!=Code::Ok)return result;
    if(!impl_->connected)return {Code::Failed};
    ULONG pid=0;DWORD bytes=0;
    if(WSAIoctl(impl_->socket,SIO_AF_UNIX_GETPEERPID,nullptr,0,&pid,sizeof(pid),&bytes,nullptr,nullptr)==SOCKET_ERROR)
        return Error(WSAGetLastError());
    // Windows 10.0.26200 populates ULONG but reports zero bytes on success.
    // Accept that observed convention or the explicit ULONG size, never an
    // unpopulated PID or a different output shape.
    if((bytes!=0&&bytes!=sizeof(pid))||!pid)return {Code::Failed};
    output=pid;return {Code::Ok};
}
NativeChannelResult UnixSocketStream::Listen(const char* path) noexcept {
    auto result=impl_->Ready(Deadline::max());if(result.code!=Code::Ok)return result;
    sockaddr_un address{};if(!Address(path,address))return {Code::Failed};
    result=impl_->Create();if(result.code!=Code::Ok)return result;
    if(bind(impl_->socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR||listen(impl_->socket,4)==SOCKET_ERROR){
        result=Error(WSAGetLastError());impl_->Close();return result;
    }
    impl_->listening=true;return {Code::Ok};
}
NativeChannelResult UnixSocketStream::Accept(UnixSocketStream& destination,Deadline deadline) noexcept {
    auto result=impl_->Ready(deadline);if(result.code!=Code::Ok)return result;
    result=destination.impl_->Ready(deadline);if(result.code!=Code::Ok)return result;
    if(!impl_->listening||destination.impl_->socket!=INVALID_SOCKET)return {Code::Failed};
    for(;;){
        result=impl_->Wait(false,deadline);if(result.code!=Code::Ok)return result;
        auto socket=accept(impl_->socket,nullptr,nullptr);
        if(socket==INVALID_SOCKET){const int error=WSAGetLastError();if(error==WSAEWOULDBLOCK)continue;return Error(error);}
        if(!Nonblocking(socket)){result=Error(WSAGetLastError());closesocket(socket);return result;}
        result=destination.impl_->Ready(deadline);
        if(result.code!=Code::Ok){closesocket(socket);return result;}
        destination.impl_->socket=socket;destination.impl_->connected=true;return {Code::Ok};
    }
}
NativeChannelResult UnixSocketStream::Connect(const char* path,Deadline deadline) noexcept {
    auto result=impl_->Ready(deadline);if(result.code!=Code::Ok)return result;
    sockaddr_un address{};if(!Address(path,address))return {Code::Failed};
    result=impl_->Create();if(result.code!=Code::Ok)return result;
    if(connect(impl_->socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==SOCKET_ERROR){
        const int error=WSAGetLastError();
        result=error==WSAEWOULDBLOCK?impl_->Wait(true,deadline):Error(error);
        if(result.code==Code::Ok){
            int completion=0,size=sizeof(completion);
            if(getsockopt(impl_->socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&completion),&size)==SOCKET_ERROR)result=Error(WSAGetLastError());
            else if(completion)result=Error(completion);
        }
        if(result.code!=Code::Ok){impl_->Close();return result;}
    }
    impl_->connected=true;return {Code::Ok};
}
NativeChannelResult UnixSocketStream::ReadSome(void* output,std::size_t capacity,Deadline deadline) noexcept {
    auto result=impl_->Ready(deadline);if(result.code!=Code::Ok)return result;
    if(!impl_->connected||!output||!capacity||capacity>MaxTransferBytes)return {Code::Failed};
    for(;;){
        result=impl_->Wait(false,deadline);if(result.code!=Code::Ok)return result;
        const int bytes=recv(impl_->socket,static_cast<char*>(output),static_cast<int>(capacity),0);
        if(bytes>0)return {Code::Ok,static_cast<std::uint32_t>(bytes)};
        if(!bytes)return {Code::Removed};
        const int error=WSAGetLastError();if(error!=WSAEWOULDBLOCK)return Error(error);
    }
}
NativeChannelResult UnixSocketStream::WriteAll(const void* input,std::size_t bytes,Deadline deadline) noexcept {
    auto result=impl_->Ready(deadline);if(result.code!=Code::Ok)return result;
    if(!impl_->connected||!input||!bytes||bytes>MaxTransferBytes)return {Code::Failed};
    std::uint32_t total=0;
    while(total<bytes){
        result=impl_->Wait(true,deadline);if(result.code!=Code::Ok){result.bytes=total;return result;}
        const int sent=send(impl_->socket,static_cast<const char*>(input)+total,static_cast<int>(bytes-total),0);
        if(sent>0){total+=static_cast<std::uint32_t>(sent);continue;}
        const int error=sent?WSAGetLastError():WSAECONNRESET;
        if(error!=WSAEWOULDBLOCK){result=Error(error);result.bytes=total;return result;}
    }
    return {Code::Ok,total};
}
}
