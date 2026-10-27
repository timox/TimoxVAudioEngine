#pragma once
#include <windows.h>

// Process-lifetime exclusion, acquired before client discovery or audio setup.
// Global because the control API port and physical audio device are machine-wide.
// This is not API authentication. An inaccessible/conflicting object fails closed.
class EngineInstanceGuard final {
public:
    explicit EngineInstanceGuard(const wchar_t* name=L"Global\\TimoxVasio.Engine.v1") noexcept {
        SetLastError(ERROR_SUCCESS);
        handle_=CreateMutexW(nullptr,FALSE,name);
        error_=GetLastError();
        if(!handle_){if(!error_)error_=ERROR_GEN_FAILURE;return;}
        if(error_==ERROR_ALREADY_EXISTS){CloseHandle(handle_);handle_=nullptr;return;}
        error_=ERROR_SUCCESS;
    }
    ~EngineInstanceGuard(){if(handle_)CloseHandle(handle_);}
    EngineInstanceGuard(const EngineInstanceGuard&)=delete;
    EngineInstanceGuard& operator=(const EngineInstanceGuard&)=delete;
    bool acquired()const noexcept{return handle_!=nullptr;}
    DWORD error()const noexcept{return error_;}
private:
    HANDLE handle_=nullptr;
    DWORD error_=ERROR_SUCCESS;
};
