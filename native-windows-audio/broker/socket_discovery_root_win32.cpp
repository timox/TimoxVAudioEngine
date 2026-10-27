#include "socket_discovery_root_win32.h"
#include "socket_discovery_win32.h"
#include <aclapi.h>
#include <shlobj.h>
#include <winternl.h>
#include <sddl.h>
#include <array>
#include <cwchar>

namespace timox_broker {
namespace {
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    ~Handle(){if(value && value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
};
struct Security {PSECURITY_DESCRIPTOR value=nullptr;~Security(){if(value)LocalFree(value);}};
struct KnownPath {PWSTR value=nullptr;~KnownPath(){CoTaskMemFree(value);}};
bool Trusted(PSID sid) noexcept {
    if(!sid || !IsValidSid(sid)) return false;
    if(IsWellKnownSid(sid,WinLocalSystemSid)||IsWellKnownSid(sid,WinBuiltinAdministratorsSid))return true;
    struct InstallerSid {
        BYTE revision=SID_REVISION,count=6;SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
        DWORD sub[6]={80,956008885,3418522649,1831038044,1853292631,2271478464};
    } installer;
    static_assert(offsetof(InstallerSid,sub)==8,"SID layout");
    return EqualSid(sid,&installer)!=FALSE;
}
DWORD DirectorySecurity(HANDLE directory,bool privateDirectory) noexcept {
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(directory,&info))return GetLastError();
    if(GetFileType(directory)!=FILE_TYPE_DISK || !(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) ||
       (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))return ERROR_ACCESS_DENIED;
    Security sd;
    const auto error=GetSecurityInfo(directory,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        nullptr,nullptr,nullptr,nullptr,&sd.value);
    if(error!=ERROR_SUCCESS)return error;
    return privateDirectory ? ValidateSocketDiscoveryDirectorySecurity(sd.value) : ValidateSocketDiscoveryAncestorSecurity(sd.value);
}
DWORD Directory(HANDLE parent,const wchar_t* text,HANDLE& output,PSECURITY_DESCRIPTOR security=nullptr) noexcept {
    // Only generated absolute root or previously validated individual components.
    const auto length=std::wcslen(text);
    UNICODE_STRING name{};name.Buffer=const_cast<wchar_t*>(text);
    name.Length=static_cast<USHORT>(length*sizeof(wchar_t));name.MaximumLength=name.Length;
    OBJECT_ATTRIBUTES attributes{};InitializeObjectAttributes(&attributes,&name,OBJ_CASE_INSENSITIVE,parent,security);
    IO_STATUS_BLOCK io{};Handle directory;
    const auto status=NtCreateFile(&directory.value,FILE_READ_ATTRIBUTES|READ_CONTROL|FILE_TRAVERSE|SYNCHRONIZE,
        &attributes,&io,nullptr,0,FILE_SHARE_READ|FILE_SHARE_WRITE,security?FILE_CREATE:FILE_OPEN,
        FILE_DIRECTORY_FILE|FILE_OPEN_REPARSE_POINT|FILE_SYNCHRONOUS_IO_NONALERT,nullptr,0);
    if(status<0)return RtlNtStatusToDosError(status);
    if(!directory.value || directory.value==INVALID_HANDLE_VALUE)return ERROR_INVALID_HANDLE;
    output=directory.value;directory.value=INVALID_HANDLE_VALUE;return ERROR_SUCCESS;
}
bool Component(const wchar_t* text,std::size_t size) noexcept {
    if(!size || size>127 || text[size-1]==L'.' || text[size-1]==L' ')return false;
    for(std::size_t i=0;i<size;++i) {
        const auto c=text[i];
        if(c<L' ' || c==L'\\' || c==L'/' || c==L':' || c==L'*' || c==L'?' || c==L'"' || c==L'<' || c==L'>' || c==L'|')return false;
    }
    return true;
}
DWORD LocalNtfsRoot(HANDLE root) noexcept {
    wchar_t path[128]{};
    const auto size=GetFinalPathNameByHandleW(root,path,128,VOLUME_NAME_NT);
    if(!size)return GetLastError();
    if(size>=128)return ERROR_NOT_SUPPORTED;
    constexpr wchar_t prefix[]=L"\\Device\\HarddiskVolume";
    constexpr auto prefixSize=sizeof(prefix)/sizeof(wchar_t)-1;
    if(std::wcsncmp(path,prefix,prefixSize)!=0)return ERROR_NOT_SUPPORTED;
    auto i=prefixSize;
    if(path[i]<L'0'||path[i]>L'9')return ERROR_NOT_SUPPORTED;
    while(path[i]>=L'0'&&path[i]<=L'9')++i;
    if(path[i]!=L'\\'||path[i+1])return ERROR_NOT_SUPPORTED;
    wchar_t filesystem[32]{};DWORD flags=0;
    if(!GetVolumeInformationByHandleW(root,nullptr,0,nullptr,nullptr,&flags,filesystem,32))return GetLastError();
    return std::wcscmp(filesystem,L"NTFS")==0 && (flags&FILE_PERSISTENT_ACLS) ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
}
}
DWORD ValidateSocketDiscoveryAncestorSecurity(PSECURITY_DESCRIPTOR descriptor) noexcept {
    if(!descriptor||!IsValidSecurityDescriptor(descriptor))return ERROR_ACCESS_DENIED;
    PSID owner=nullptr;PACL acl=nullptr;BOOL defaulted=FALSE,present=FALSE;
    if(!GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)||!Trusted(owner)||
       !GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)||!present||!acl||!IsValidAcl(acl)||acl->AceCount>128)
        return ERROR_ACCESS_DENIED;
    constexpr DWORD harmless=FILE_GENERIC_READ|FILE_GENERIC_EXECUTE|FILE_ADD_FILE|FILE_ADD_SUBDIRECTORY|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES;
    for(DWORD i=0;i<acl->AceCount;++i) {
        void* raw=nullptr;if(!GetAce(acl,i,&raw))return ERROR_ACCESS_DENIED;
        const auto* header=static_cast<ACE_HEADER*>(raw);
        constexpr auto offset=offsetof(ACCESS_ALLOWED_ACE,SidStart);
        if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE || header->AceSize<offset+8 ||
           (header->AceFlags&~(OBJECT_INHERIT_ACE|CONTAINER_INHERIT_ACE|NO_PROPAGATE_INHERIT_ACE|INHERIT_ONLY_ACE|INHERITED_ACE)))
            return ERROR_ACCESS_DENIED;
        const auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
        auto* sid=reinterpret_cast<BYTE*>(const_cast<DWORD*>(&ace->SidStart));
        if(sid[1]>SID_MAX_SUB_AUTHORITIES || offset+8+4*static_cast<std::size_t>(sid[1])!=header->AceSize || !IsValidSid(sid))return ERROR_ACCESS_DENIED;
        if(header->AceFlags&INHERIT_ONLY_ACE)continue;
        if(Trusted(sid))continue;
        DWORD mask=ace->Mask;
        GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
        MapGenericMask(&mask,&mapping);
        if(mask&~harmless)return ERROR_ACCESS_DENIED;
    }
    return ERROR_SUCCESS;
}
DWORD OpenSocketDiscoveryProgramData(HANDLE& output) noexcept {
    if(output!=INVALID_HANDLE_VALUE)return ERROR_INVALID_PARAMETER;
    KnownPath known;
    const auto hr=SHGetKnownFolderPath(FOLDERID_ProgramData,KF_FLAG_DONT_VERIFY,nullptr,&known.value);
    if(FAILED(hr))return static_cast<DWORD>(hr);
    if(!known.value)return ERROR_INVALID_DATA;
    std::array<wchar_t,MAX_PATH> path{};
    std::size_t length=0;while(length<path.size()&&known.value[length])++length;
    if(length<4||length>=path.size())return ERROR_BAD_PATHNAME;
    for(std::size_t i=0;i<=length;++i)path[i]=known.value[i];
    if(!((path[0]>=L'A'&&path[0]<=L'Z')||(path[0]>=L'a'&&path[0]<=L'z'))||path[1]!=L':'||path[2]!=L'\\')return ERROR_BAD_PATHNAME;
    // Global DOS mapping avoids a per-logon drive-letter shadow. Resolve only
    // the drive here; subsequent filesystem components are handle-relative.
    wchar_t root[]=L"\\GLOBAL??\\C:\\";root[10]=path[0];
    std::array<Handle,32> held{};
    auto error=Directory(nullptr,root,held[0].value);if(error)return error;
    error=LocalNtfsRoot(held[0].value);if(error)return error;
    error=DirectorySecurity(held[0].value,false);if(error)return error;
    std::size_t count=1,begin=3;
    while(begin<length) {
        auto end=begin;while(end<length&&path[end]!=L'\\')++end;
        if(!Component(path.data()+begin,end-begin)||count==held.size())return ERROR_BAD_PATHNAME;
        path[end]=0;
        error=Directory(held[count-1].value,path.data()+begin,held[count].value);if(error)return error;
        error=DirectorySecurity(held[count].value,false);if(error)return error;
        ++count;begin=end+1;
        if(end+1==length)return ERROR_BAD_PATHNAME;
    }
    output=held[count-1].value;held[count-1].value=INVALID_HANDLE_VALUE;return ERROR_SUCCESS;
}
namespace {
DWORD OpenPrivateRoot(const wchar_t* component,HANDLE& output) noexcept {
    if(output!=INVALID_HANDLE_VALUE)return ERROR_INVALID_PARAMETER;
    Handle programData,product,root;
    auto error=OpenSocketDiscoveryProgramData(programData.value);if(error)return error;
    error=Directory(programData.value,L"TimoxNativeAudioBroker",product.value);if(error)return error;
    error=DirectorySecurity(product.value,true);if(error)return error;
    error=Directory(product.value,component,root.value);if(error)return error;
    error=DirectorySecurity(root.value,true);if(error)return error;
    output=root.value;root.value=INVALID_HANDLE_VALUE;return ERROR_SUCCESS;
}
}
DWORD OpenInstalledSocketDiscoveryRoot(HANDLE& output) noexcept {
    return OpenPrivateRoot(L"Runtime",output);
}
DWORD OpenSocketDiscoveryQualificationRoot(HANDLE& output) noexcept {
    return OpenPrivateRoot(L"Qualification",output);
}
DWORD ReadInstalledSocketDiscovery(SocketDiscovery& output) noexcept {
    Handle root;
    const auto error=OpenInstalledSocketDiscoveryRoot(root.value);
    return error==ERROR_SUCCESS ? ReadSocketDiscoveryDirectory(root.value,output) : error;
}
DWORD PrepareSocketDiscoveryQualificationRoot(bool apply,QualificationRootPreparation& output) noexcept {
    if(apply) {
        BYTE sid[SECURITY_MAX_SID_SIZE]{};DWORD size=sizeof(sid);BOOL member=FALSE;
        if(!CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,sid,&size)||!CheckTokenMembership(nullptr,sid,&member))return GetLastError();
        if(!member)return ERROR_ELEVATION_REQUIRED;
    }
    Security security;
    if(apply&&!ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"O:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;S-1-5-80-954356743-986487362-260697779-2273189862-4263205101)(A;;FRFX;;;AU)",
        SDDL_REVISION_1,&security.value,nullptr))return GetLastError();
    Handle programData,product,qualification;QualificationRootPreparation preparation;
    auto error=OpenSocketDiscoveryProgramData(programData.value);if(error)return error;
    error=Directory(programData.value,L"TimoxNativeAudioBroker",product.value);
    if(error==ERROR_FILE_NOT_FOUND){
        preparation.productMissing=true;preparation.qualificationMissing=true;
        if(!apply){output=preparation;return ERROR_SUCCESS;}
        error=Directory(programData.value,L"TimoxNativeAudioBroker",product.value,security.value);
    }
    if(error)return error;
    error=DirectorySecurity(product.value,true);if(error)return error;
    error=Directory(product.value,L"Qualification",qualification.value);
    if(error==ERROR_FILE_NOT_FOUND){
        preparation.qualificationMissing=true;
        if(!apply){output=preparation;return ERROR_SUCCESS;}
        error=Directory(product.value,L"Qualification",qualification.value,security.value);
    }
    if(error)return error;
    error=DirectorySecurity(qualification.value,true);if(error)return error;
    output=preparation;return ERROR_SUCCESS;
}
}
