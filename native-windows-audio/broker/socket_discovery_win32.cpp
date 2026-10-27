#include "socket_discovery_win32.h"
#include <aclapi.h>
#include <winternl.h>

namespace timox_broker {
namespace {
// sc.exe showsid TimoxNativeAudioBroker; tied to the production service name,
// not supplied by a descriptor, environment variable or a connected client.
struct BrokerSid {
    BYTE revision=SID_REVISION;
    BYTE count=6;
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    DWORD sub[6]={80,954356743,986487362,260697779,2273189862,4263205101};
};
static_assert(offsetof(BrokerSid,sub)==8,"Windows SID layout");
unsigned Principal(PSID sid) noexcept {
    if(!sid || !IsValidSid(sid)) return 0;
    if(IsWellKnownSid(sid,WinLocalSystemSid)) return 1;
    if(IsWellKnownSid(sid,WinBuiltinAdministratorsSid)) return 2;
    BrokerSid broker;
    if(EqualSid(sid,&broker)) return 4;
    if(IsWellKnownSid(sid,WinAuthenticatedUserSid)) return 8;
    return 0;
}
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct Security {
    PSECURITY_DESCRIPTOR value=nullptr;
    ~Security(){if(value)LocalFree(value);}
};
DWORD ValidateHandleSecurity(HANDLE file,bool directory=false) noexcept {
    Security security;
    const auto error=GetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        nullptr,nullptr,nullptr,nullptr,&security.value);
    if(error!=ERROR_SUCCESS) return error;
    return directory ? ValidateSocketDiscoveryDirectorySecurity(security.value) : ValidateSocketDiscoverySecurity(security.value);
}
DWORD ValidateSecurity(PSECURITY_DESCRIPTOR descriptor,DWORD readerMask) noexcept {
    if(!descriptor || !IsValidSecurityDescriptor(descriptor)) return ERROR_ACCESS_DENIED;
    PSID owner=nullptr; BOOL defaulted=FALSE,present=FALSE;
    SECURITY_DESCRIPTOR_CONTROL control=0; DWORD revision=0; PACL acl=nullptr;
    if(!GetSecurityDescriptorOwner(descriptor,&owner,&defaulted) ||
       !GetSecurityDescriptorControl(descriptor,&control,&revision) ||
       !GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)) return ERROR_ACCESS_DENIED;
    const unsigned ownerKind=Principal(owner);
    if((ownerKind!=1 && ownerKind!=2 && ownerKind!=4) ||
       !(control&SE_DACL_PROTECTED) || !present || !acl || !IsValidAcl(acl) || acl->AceCount!=4)
        return ERROR_ACCESS_DENIED;
    unsigned seen=0;
    for(DWORD i=0;i<acl->AceCount;++i) {
        void* raw=nullptr;
        if(!GetAce(acl,i,&raw)) return ERROR_ACCESS_DENIED;
        const auto* header=static_cast<ACE_HEADER*>(raw);
        constexpr auto offset=offsetof(ACCESS_ALLOWED_ACE,SidStart);
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE || header->AceFlags!=0 || header->AceSize<offset+8)
            return ERROR_ACCESS_DENIED;
        const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
        auto* sid=reinterpret_cast<BYTE*>(const_cast<DWORD*>(&ace->SidStart));
        if(sid[1]>SID_MAX_SUB_AUTHORITIES || offset+8+4*static_cast<std::size_t>(sid[1])!=header->AceSize)
            return ERROR_ACCESS_DENIED;
        const auto kind=Principal(sid);
        const DWORD expected=kind==8 ? readerMask : FILE_ALL_ACCESS;
        if(!kind || (seen&kind) || ace->Mask!=expected)
            return ERROR_ACCESS_DENIED;
        seen|=kind;
    }
    return seen==15 ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
}
DWORD ValidateSocketDiscoverySecurity(PSECURITY_DESCRIPTOR descriptor) noexcept {
    return ValidateSecurity(descriptor,FILE_GENERIC_READ);
}
DWORD ValidateSocketDiscoveryDirectorySecurity(PSECURITY_DESCRIPTOR descriptor) noexcept {
    return ValidateSecurity(descriptor,FILE_GENERIC_READ|FILE_GENERIC_EXECUTE);
}
DWORD ReadSocketDiscoveryHandle(HANDLE original, SocketDiscovery& output) noexcept {
    if(!original || original==INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    Handle file{ReOpenFile(original,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ,FILE_FLAG_OPEN_REPARSE_POINT)};
    if(file.value==INVALID_HANDLE_VALUE) return GetLastError();
    if(GetFileType(file.value)!=FILE_TYPE_DISK) return ERROR_ACCESS_DENIED;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(file.value,&info)) return GetLastError();
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || info.nNumberOfLinks!=1)
        return ERROR_ACCESS_DENIED;
    if(info.nFileSizeHigh || info.nFileSizeLow!=SocketDiscoveryBytes) return ERROR_INVALID_DATA;
    const auto policy=ValidateHandleSecurity(file.value);
    if(policy!=ERROR_SUCCESS) return policy;
    SocketDiscoveryBuffer bytes{}; DWORD read=0;
    if(!ReadFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)) return GetLastError();
    if(read!=bytes.size()) return ERROR_INVALID_DATA;
    return DecodeSocketDiscovery(bytes.data(),bytes.size(),output) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}
DWORD OpenSocketDiscoveryChild(HANDLE parent,const wchar_t* component,HANDLE& output) noexcept {
    if(output!=INVALID_HANDLE_VALUE) return ERROR_INVALID_PARAMETER;
    if(!parent || parent==INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    if(!component) return ERROR_INVALID_NAME;
    std::size_t length=0;
    for(;length<128 && component[length];++length) {
        const auto ch=component[length];
        if(ch<L' ' || ch==L'\\' || ch==L'/' || ch==L':' || ch==L'*' || ch==L'?' || ch==L'"' || ch==L'<' || ch==L'>' || ch==L'|')
            return ERROR_INVALID_NAME;
    }
    if(length==0 || length==128 || component[length-1]==L'.' || component[length-1]==L' ')
        return ERROR_INVALID_NAME;
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(parent,&info)) return GetLastError();
    if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
        return ERROR_ACCESS_DENIED;
    UNICODE_STRING name{};
    name.Buffer=const_cast<wchar_t*>(component);
    name.Length=static_cast<USHORT>(length*sizeof(wchar_t)); name.MaximumLength=name.Length;
    OBJECT_ATTRIBUTES attributes{};
    InitializeObjectAttributes(&attributes,&name,OBJ_CASE_INSENSITIVE,parent,nullptr);
    IO_STATUS_BLOCK io{}; Handle child;
    const auto status=NtCreateFile(&child.value,FILE_GENERIC_READ,&attributes,&io,nullptr,0,FILE_SHARE_READ,
        FILE_OPEN,FILE_NON_DIRECTORY_FILE|FILE_SYNCHRONOUS_IO_NONALERT|FILE_OPEN_REPARSE_POINT,nullptr,0);
    if(status<0) return RtlNtStatusToDosError(status);
    if(child.value==INVALID_HANDLE_VALUE || child.value==nullptr) return ERROR_INVALID_HANDLE;
    if(GetFileType(child.value)!=FILE_TYPE_DISK) return ERROR_ACCESS_DENIED;
    if(!GetFileInformationByHandle(child.value,&info)) return GetLastError();
    if(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) return ERROR_ACCESS_DENIED;
    output=child.value; child.value=INVALID_HANDLE_VALUE;
    return ERROR_SUCCESS;
}
DWORD ReadSocketDiscoveryDirectory(HANDLE original,SocketDiscovery& output) noexcept {
    if(!original || original==INVALID_HANDLE_VALUE) return ERROR_INVALID_HANDLE;
    Handle directory{ReOpenFile(original,FILE_READ_ATTRIBUTES|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT)};
    if(directory.value==INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(directory.value,&info)) return GetLastError();
    if(GetFileType(directory.value)!=FILE_TYPE_DISK || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
       (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) return ERROR_ACCESS_DENIED;
    const auto policy=ValidateHandleSecurity(directory.value,true);
    if(policy!=ERROR_SUCCESS) return policy;
    Handle publication;
    const auto opened=OpenSocketDiscoveryChild(directory.value,L"discovery.bin",publication.value);
    if(opened!=ERROR_SUCCESS) return opened;
    return ReadSocketDiscoveryHandle(publication.value,output);
}
}
