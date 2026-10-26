#include "engine_instance_guard.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
void Require(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
DWORD Run(const wchar_t* executable,const std::wstring& arguments) {
    std::wstring command=L"\""+std::wstring(executable)+L"\" "+arguments;
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,
        nullptr,nullptr,&startup,&process)!=FALSE,"spawn competing process");
    CloseHandle(process.hThread);
    const auto wait=WaitForSingleObject(process.hProcess,10000);
    DWORD code=999;
    if(wait!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,10000);}
    else GetExitCodeProcess(process.hProcess,&code);
    CloseHandle(process.hProcess);Require(wait==WAIT_OBJECT_0,"child timeout");return code;
}
DWORD Probe(const std::wstring& name,const wchar_t* mode=L"--probe") {
    wchar_t executable[32768]{};
    Require(GetModuleFileNameW(nullptr,executable,32768)>0,"executable path");
    return Run(executable,std::wstring(mode)+L" \""+name+L"\"");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc==3&&(std::wstring(argv[1])==L"--probe"||std::wstring(argv[1])==L"--crash")) {
        EngineInstanceGuard guard(argv[2]);
        if(guard.acquired()&&std::wstring(argv[1])==L"--crash")ExitProcess(0); // No stack unwinding.
        return guard.acquired()?0:guard.error()==ERROR_ALREADY_EXISTS?3:4;
    }
    try {
        if(argc==3&&std::wstring(argv[1])==L"--engine") {
            EngineInstanceGuard owner;
            Require(owner.acquired()||owner.error()==ERROR_ALREADY_EXISTS,"production exclusion available");
            Require(Run(argv[2],L"")==3,"real engine must reject duplicate before initialization");
            std::puts("Real engine rejects competing startup");return 0;
        }
        const auto name=L"Global\\TimoxEngineGuardTest-"+std::to_wstring(GetCurrentProcessId());
        {
            EngineInstanceGuard owner(name.c_str());Require(owner.acquired(),"first instance owns guard");
            Require(Probe(name)==3,"second process must be rejected");
            {EngineInstanceGuard second(name.c_str());Require(!second.acquired()&&second.error()==ERROR_ALREADY_EXISTS,"same-process contender rejected");}
            Require(Probe(name)==3,"loser must not release the owner's guard");
        }
        Require(Probe(name)==0,"new process can start after owner exits");
        Require(Probe(name,L"--crash")==0,"abrupt exit while holding guard");
        Require(Probe(name)==0,"OS releases guard without destructor");
        const auto eventName=name+L"-event";
        HANDLE event=CreateEventW(nullptr,TRUE,FALSE,eventName.c_str());Require(event!=nullptr,"create conflicting object");
        {EngineInstanceGuard conflict(eventName.c_str());Require(!conflict.acquired()&&conflict.error()!=0,"unexpected object fails closed");}
        CloseHandle(event);
        std::puts("Engine instance exclusion and release passed");return 0;
    } catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
