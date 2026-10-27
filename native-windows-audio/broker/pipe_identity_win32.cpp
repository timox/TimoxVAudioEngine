#include "pipe_identity_win32.h"
#include <cstdlib>
#include <cstring>
namespace timox_broker {
namespace {
struct Token {
    HANDLE value=nullptr;
    ~Token(){if(value)CloseHandle(value);}
};
DWORD CopySidBytes(PSID source,std::array<std::uint8_t,68>& target,std::uint16_t& length) noexcept {
    if(!source||!IsValidSid(source))return ERROR_INVALID_SID;
    const DWORD bytes=GetLengthSid(source);
    if(bytes<8||bytes>target.size())return ERROR_INVALID_SID;
    std::memcpy(target.data(),source,bytes);length=static_cast<std::uint16_t>(bytes);return ERROR_SUCCESS;
}
DWORD ReadIdentity(HANDLE token,ChannelIdentity& output) noexcept {
    ChannelIdentity identity;DWORD bytes=0;
    // Each query below contains one SID, not an unbounded list of token groups.
    alignas(TOKEN_USER) std::array<std::uint8_t,256> user{};
    if(!GetTokenInformation(token,TokenUser,user.data(),static_cast<DWORD>(user.size()),&bytes))return GetLastError();
    auto result=CopySidBytes(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,identity.userSid,identity.userSidBytes);
    if(result!=ERROR_SUCCESS)return result;
    alignas(TOKEN_GROUPS) std::array<std::uint8_t,256> logon{};
    if(!GetTokenInformation(token,TokenLogonSid,logon.data(),static_cast<DWORD>(logon.size()),&bytes))return GetLastError();
    const auto* groups=reinterpret_cast<const TOKEN_GROUPS*>(logon.data());
    if(groups->GroupCount!=1||(groups->Groups[0].Attributes&SE_GROUP_LOGON_ID)!=SE_GROUP_LOGON_ID)return ERROR_INVALID_SID;
    result=CopySidBytes(groups->Groups[0].Sid,identity.logonSid,identity.logonSidBytes);
    if(result!=ERROR_SUCCESS)return result;
    TOKEN_STATISTICS stats{};
    if(!GetTokenInformation(token,TokenStatistics,&stats,sizeof(stats),&bytes))return GetLastError();
    identity.authenticationId=(std::uint64_t{static_cast<std::uint32_t>(stats.AuthenticationId.HighPart)}<<32)|stats.AuthenticationId.LowPart;
    DWORD session=0;
    if(!GetTokenInformation(token,TokenSessionId,&session,sizeof(session),&bytes))return GetLastError();
    identity.sessionId=session;
    if(!SameChannelIdentity(identity,identity))return ERROR_INVALID_DATA;
    output=identity;return ERROR_SUCCESS;
}
}
DWORD ObserveProcessIdentity(ChannelIdentity& output) noexcept {
    Token token;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.value))return GetLastError();
    return ReadIdentity(token.value,output);
}
DWORD ObserveProcessHandleIdentity(HANDLE process,ChannelIdentity& output) noexcept {
    if(!process||process==INVALID_HANDLE_VALUE||!GetProcessId(process))return ERROR_INVALID_HANDLE;
    Token token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.value))return GetLastError();
    return ReadIdentity(token.value,output);
}
DWORD ObservePipeIdentity(HANDLE pipe,ChannelIdentity& output) noexcept {
    Token previous;
    if(OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&previous.value))return ERROR_INVALID_STATE;
    const DWORD priorError=GetLastError();if(priorError!=ERROR_NO_TOKEN)return priorError;
    if(!ImpersonateNamedPipeClient(pipe))return GetLastError();
    Token client;
    const BOOL opened=OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&client.value);
    const DWORD captureError=opened?ERROR_SUCCESS:GetLastError();
    if(!RevertToSelf()){
        // Microsoft requires shutdown on failure: no privileged work may proceed
        // with an unknown effective identity. Never return a recoverable error.
        TerminateProcess(GetCurrentProcess(),ERROR_CANNOT_IMPERSONATE);
        std::abort();
    }
    if(captureError!=ERROR_SUCCESS)return captureError;
    return ReadIdentity(client.value,output);
}
}
