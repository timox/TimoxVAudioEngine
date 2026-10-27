#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

struct PwarSettings {
    std::string address = "192.168.66.2";
    int port = 8321;
    bool exists = false;
};

// The engine and the external PWAR driver use the same process USERPROFILE.
// No renderer-supplied filesystem path is accepted by the HTTP API.
class PwarConfigurationStore {
public:
    explicit PwarConfigurationStore(std::wstring path = DefaultPath()) : path_(std::move(path)) {}
    static std::wstring DefaultPath() {
        wchar_t profile[32768]{};
        const auto size = GetEnvironmentVariableW(L"USERPROFILE", profile, 32768);
        return size && size < 32768 ? std::wstring(profile) + L"\\pwarASIO.cfg" : std::wstring{};
    }
    static bool Valid(const PwarSettings& settings) {
        if (settings.port < 1 || settings.port > 65535 || settings.address.size() > 15) return false;
        std::istringstream stream(settings.address); std::string component; unsigned count = 0;
        while (std::getline(stream, component, '.')) {
            if (component.empty() || component.size() > 3 || (component.size() > 1 && component[0] == '0')) return false;
            unsigned value = 0;
            for (const char digit : component) { if (digit < '0' || digit > '9') return false; value = value * 10 + digit - '0'; }
            if (value > 255) return false;
            ++count;
        }
        return count == 4 && !settings.address.empty() && settings.address.back() != '.';
    }
    bool Load(PwarSettings& settings, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string content; bool exists = false;
        if (!Read(content, exists, error)) return false;
        PwarSettings result; result.exists = exists;
        std::istringstream lines(content); std::string line;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("udp_send_ip=", 0) == 0) result.address = line.substr(12);
            if (line.rfind("udp_send_port=", 0) == 0) {
                std::istringstream port(line.substr(14)); int value = 0; char extra = 0;
                if ((port >> value) && !(port >> extra) && value > 0 && value <= 65535) result.port = value;
            }
        }
        settings = std::move(result); return true;
    }
    bool Save(const PwarSettings& settings, std::string& error) {
        std::lock_guard<std::mutex> lock(mutex_);
        error.clear();
        if (!Valid(settings)) { error = "Enter a valid IPv4 address and a port from 1 to 65535"; return false; }
        std::string content; bool exists = false;
        if (!Read(content, exists, error)) return false;
        std::istringstream lines(content); std::string line, updated;
        while (std::getline(lines, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("udp_send_ip=", 0) != 0 && line.rfind("udp_send_port=", 0) != 0) updated += line + "\r\n";
        }
        updated += "udp_send_ip=" + settings.address + "\r\nudp_send_port=" + std::to_string(settings.port) + "\r\n";
        if (updated.size() > MaximumBytes) { error = "PWAR configuration is too large"; return false; }
        static std::atomic<unsigned> sequence{0};
        const auto temporary = path_ + L".timox-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return Fail("Cannot create PWAR configuration", error);
        DWORD written = 0;
        const bool complete = WriteFile(file, updated.data(), static_cast<DWORD>(updated.size()), &written, nullptr) &&
            written == updated.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!complete) { DeleteFileW(temporary.c_str()); error = "Cannot write complete PWAR configuration"; return false; }
        if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            Fail("Cannot replace PWAR configuration", error); DeleteFileW(temporary.c_str()); return false;
        }
        return true;
    }
private:
    static constexpr DWORD MaximumBytes = 32768;
    static bool Fail(const char* operation, std::string& error) {
        error = std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")"; return false;
    }
    bool Read(std::string& content, bool& exists, std::string& error) {
        error.clear();
        if (path_.empty()) { error = "The engine user profile is unavailable"; return false; }
        const HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND) { exists = false; content.clear(); return true; }
            return Fail("Cannot read PWAR configuration", error);
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > MaximumBytes) {
            CloseHandle(file); error = "PWAR configuration is unreadable or too large"; return false;
        }
        content.resize(static_cast<size_t>(size.QuadPart)); DWORD read = 0;
        const bool complete = ReadFile(file, content.data(), static_cast<DWORD>(content.size()), &read, nullptr) && read == content.size();
        CloseHandle(file);
        if (!complete) { error = "Cannot read complete PWAR configuration"; return false; }
        exists = true; return true;
    }
    std::wstring path_;
    std::mutex mutex_;
};
