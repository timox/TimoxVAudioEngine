#include "pwar_client_supervisor.h"

#include <bcrypt.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using Json = nlohmann::json;
constexpr char kExpectedClientSha256[] =
    TIMOX_PWAR_CLIENT_SHA256;
constexpr wchar_t kDriverClsid[] = L"{A4D39126-78CB-4D89-9E0A-54494D4F5856}";
constexpr std::size_t kMaximumLineBytes = 16384;
constexpr DWORD kStopTimeoutMs = 5000;

std::string win32Error(const char* operation, DWORD code = GetLastError()) {
    return std::string(operation) + " (Windows error " + std::to_string(code) + ")";
}

bool validPeerAddress(const std::string& address) {
    if (address.empty() || address.size() > 15) return false;
    std::size_t begin = 0;
    unsigned components = 0;
    while (begin < address.size()) {
        const auto end = address.find('.', begin);
        const auto length = (end == std::string::npos ? address.size() : end) - begin;
        if (length == 0 || length > 3 || (length > 1 && address[begin] == '0')) return false;
        unsigned value = 0;
        for (std::size_t index = begin; index < begin + length; ++index) {
            if (address[index] < '0' || address[index] > '9') return false;
            value = value * 10 + static_cast<unsigned>(address[index] - '0');
        }
        if (value > 255) return false;
        ++components;
        if (end == std::string::npos) { begin = address.size(); break; }
        begin = end + 1;
    }
    return components == 4 && address.back() != '.';
}

std::wstring quoteArgument(const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') { ++backslashes; continue; }
        if (character == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(character);
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

std::wstring numberArgument(std::uint32_t value) {
    return std::to_wstring(value);
}

bool hashClient(const std::filesystem::path& path, std::string& result, std::string& error) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, returned = 0, digestBytes = 0;
    std::vector<UCHAR> object;
    std::array<UCHAR, 32> digest{};
    HANDLE file = INVALID_HANDLE_VALUE;
    auto fail = [&](const char* operation, NTSTATUS status = 0) {
        error = status ? std::string(operation) + " (BCrypt status " + std::to_string(status) + ")"
                       : win32Error(operation);
    };
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        fail("Unable to initialize the PWAR client hash"); return false;
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes), &returned, 0) < 0 ||
        digestBytes != digest.size()) {
        error = "Unable to query the PWAR client SHA-256 parameters";
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    object.resize(objectBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0) {
        error = "Unable to create the PWAR client SHA-256 hash";
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fail("Unable to open the PWAR client for verification");
        BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
    std::array<UCHAR, 64 * 1024> buffer{};
    bool success = true;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            fail("Unable to read the PWAR client for verification"); success = false; break;
        }
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) < 0) {
            error = "Unable to hash the PWAR client"; success = false; break;
        }
    }
    if (success && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        error = "Unable to finish the PWAR client SHA-256 hash"; success = false;
    }
    CloseHandle(file);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) return false;
    static constexpr char hex[] = "0123456789ABCDEF";
    result.clear(); result.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 0x0F]);
    }
    return true;
}

bool resolveClientPath(std::filesystem::path& path, std::string& error) {
    std::array<wchar_t, 32768> module{};
    const DWORD length = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!length || length >= module.size()) { error = "Unable to locate the Timox engine executable"; return false; }
    const auto engineDirectory = std::filesystem::path(module.data()).parent_path();
    const auto bundled = engineDirectory / L"PwarAsioClient.exe";
    std::error_code code;
    if (std::filesystem::is_regular_file(bundled, code)) { path = bundled; return true; }

    error = "The verified PWAR client is missing beside the engine";
    return false;
}

bool makePipe(HANDLE& readHandle, HANDLE& writeHandle, SECURITY_ATTRIBUTES& attributes) {
    return CreatePipe(&readHandle, &writeHandle, &attributes, 0) != FALSE;
}
}

PwarClientSupervisor::~PwarClientSupervisor() {
    std::string ignored;
    Stop(ignored);
}

bool PwarClientSupervisor::Start(const std::string& peerAddress, std::uint32_t maxQueueMs,
                                 std::string& error) {
    std::lock_guard<std::mutex> operation(operationMutex_);
    return startLocked(peerAddress, maxQueueMs, error);
}

bool PwarClientSupervisor::startLocked(const std::string& peerAddress, std::uint32_t maxQueueMs,
                                       std::string& error) {
    error.clear();
    if (!validPeerAddress(peerAddress) || maxQueueMs < 1 || maxQueueMs > 2000) {
        error = "PWAR peer must be an IPv4 address and queue age must be 1 to 2000 ms";
        return false;
    }
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        if (process_ && !processExited_) { error = "The PWAR ASIO client is already running"; return false; }
    }
    closeChild();

    std::filesystem::path clientPath;
    if (!resolveClientPath(clientPath, error)) return false;
    std::string actualHash;
    if (!hashClient(clientPath, actualHash, error)) return false;
    if (actualHash != kExpectedClientSha256) {
        error = "The PWAR client SHA-256 does not match the verified handoff binary";
        return false;
    }

    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE childInput = nullptr, parentInput = nullptr;
    HANDLE parentOutput = nullptr, childOutput = nullptr;
    HANDLE parentError = nullptr, childError = nullptr;
    auto closeLocal = [&] {
        for (HANDLE* handle : {&childInput, &parentInput, &parentOutput, &childOutput, &parentError, &childError}) {
            if (*handle) { CloseHandle(*handle); *handle = nullptr; }
        }
    };
    if (!makePipe(childInput, parentInput, attributes) ||
        !makePipe(parentOutput, childOutput, attributes) ||
        !makePipe(parentError, childError, attributes)) {
        error = win32Error("Unable to create PWAR client pipes"); closeLocal(); return false;
    }
    SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentOutput, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentError, HANDLE_FLAG_INHERIT, 0);

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::uint8_t> attributeStorage(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes)) {
        error = win32Error("Unable to prepare PWAR child handle inheritance"); closeLocal(); return false;
    }
    const std::array<HANDLE, 3> childHandles{childInput, childOutput, childError};
    const BOOL attributesSet = UpdateProcThreadAttribute(attributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, const_cast<HANDLE*>(childHandles.data()),
        sizeof(childHandles), nullptr, nullptr);
    if (!attributesSet) {
        error = win32Error("Unable to restrict PWAR child handle inheritance");
        DeleteProcThreadAttributeList(attributeList); closeLocal(); return false;
    }

    std::wstring command = quoteArgument(clientPath.wstring()) + L" --driver-clsid " +
        kDriverClsid + L" --peer " + std::wstring(peerAddress.begin(), peerAddress.end()) +
        L" --max-queue-ms " + numberArgument(maxQueueMs) + L" --stdio";
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = childInput;
    startup.StartupInfo.hStdOutput = childOutput;
    startup.StartupInfo.hStdError = childError;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION processInfo{};
    const BOOL created = CreateProcessW(clientPath.c_str(), mutableCommand.data(), nullptr, nullptr,
        TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr,
        clientPath.parent_path().c_str(), &startup.StartupInfo, &processInfo);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributeList);
    CloseHandle(childInput); childInput = nullptr;
    CloseHandle(childOutput); childOutput = nullptr;
    CloseHandle(childError); childError = nullptr;
    if (!created) { error = win32Error("Unable to launch the verified PWAR client", createError); closeLocal(); return false; }
    CloseHandle(processInfo.hThread);

    {
        std::lock_guard<std::mutex> state(stateMutex_);
        process_ = processInfo.hProcess;
        stdinWrite_ = parentInput; parentInput = nullptr;
        stdoutRead_ = parentOutput; parentOutput = nullptr;
        stderrRead_ = parentError; parentError = nullptr;
        processId_ = processInfo.dwProcessId;
        exitCode_ = STILL_ACTIVE;
        processExited_ = false;
        startAccepted_ = false;
        stopRequested_ = false;
        peerAddress_ = peerAddress;
        maxQueueMs_ = maxQueueMs;
        state_ = "starting";
        error_.clear(); stderrTail_.clear();
        clientStatus_ = Json::object(); responses_.clear();
        ++revision_;
    }
    try {
        stdoutThread_ = std::thread(&PwarClientSupervisor::readOutput, this, stdoutRead_, false);
        stderrThread_ = std::thread(&PwarClientSupervisor::readOutput, this, stderrRead_, true);
        waitThread_ = std::thread(&PwarClientSupervisor::waitForExit, this);
    } catch (const std::exception& exception) {
        error = std::string("Unable to supervise the PWAR client: ") + exception.what();
        { std::lock_guard<std::mutex> state(stateMutex_); error_ = error; state_ = "error"; stopRequested_ = true; }
        TerminateProcess(process_, 1);
        WaitForSingleObject(process_, kStopTimeoutMs);
        closeChild();
        return false;
    }

    std::unique_lock<std::mutex> state(stateMutex_);
    const bool signaled = stateChanged_.wait_for(state, std::chrono::seconds(10), [this] {
        return startAccepted_ || processExited_ || state_ == "error";
    });
    if (signaled && startAccepted_) return true;
    error = !error_.empty() ? error_ : signaled ? "The PWAR client exited before accepting ASIO start"
        : "Timed out waiting for the PWAR client ASIO start event";
    state.unlock();
    std::string ignored;
    stopLocked(ignored);
    return false;
}

bool PwarClientSupervisor::Stop(std::string& error) noexcept {
    std::lock_guard<std::mutex> operation(operationMutex_);
    return stopLocked(error);
}

bool PwarClientSupervisor::stopLocked(std::string& error) noexcept {
    error.clear();
    HANDLE process = nullptr;
    bool exited = true;
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        process = process_;
        exited = processExited_;
        if (process && !exited) { stopRequested_ = true; state_ = "stopping"; ++revision_; }
    }
    if (!process) return true;
    if (!exited) {
        std::string commandError;
        if (!sendCommandLocked("stop", commandError)) {
            std::lock_guard<std::mutex> state(stateMutex_);
            if (error_.empty()) error_ = commandError;
        }
        const DWORD wait = WaitForSingleObject(process, kStopTimeoutMs);
        if (wait == WAIT_TIMEOUT) {
            if (!TerminateProcess(process, ERROR_CANCELLED)) error = win32Error("Unable to stop the PWAR client");
            WaitForSingleObject(process, 1000);
        } else if (wait == WAIT_FAILED) error = win32Error("Unable to wait for the PWAR client to stop");
    }
    closeChild();
    return error.empty();
}

bool PwarClientSupervisor::RequestStatus(std::string& error) {
    std::lock_guard<std::mutex> operation(operationMutex_);
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        if (!process_ || processExited_) { error = "The PWAR client is not running"; return false; }
    }
    return sendCommandLocked("status", error);
}

bool PwarClientSupervisor::sendCommandLocked(const std::string& command, std::string& error) {
    error.clear();
    std::string id;
    HANDLE input = nullptr;
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        if (!stdinWrite_ || processExited_) { error = "The PWAR client pipe is closed"; return false; }
        id = "timox-" + std::to_string(++requestSequence_);
        input = stdinWrite_;
    }
    const std::string line = Json{{"version", 1}, {"id", id}, {"command", command}}.dump() + "\n";
    DWORD written = 0;
    if (!WriteFile(input, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size()) {
        error = win32Error("Unable to write to the PWAR client control pipe"); return false;
    }
    std::unique_lock<std::mutex> state(stateMutex_);
    const bool received = stateChanged_.wait_for(state,
        command == "stop" ? std::chrono::seconds(2) : std::chrono::seconds(3), [this, &id] {
            return responses_.find(id) != responses_.end() || processExited_;
        });
    const auto response = responses_.find(id);
    if (!received || response == responses_.end()) {
        error = processExited_ ? "The PWAR client exited before replying" : "Timed out waiting for the PWAR client response";
        return false;
    }
    const auto reply = response->second;
    responses_.erase(response);
    if (!reply.value("ok", false)) {
        error = reply.value("error", Json::object()).value("code", std::string("PWAR_COMMAND_FAILED"));
        return false;
    }
    if (reply.contains("status") && reply["status"].is_object()) {
        clientStatus_ = reply["status"];
        state_ = clientStatus_.value("state", state_);
        ++revision_;
    }
    return true;
}

nlohmann::json PwarClientSupervisor::Snapshot() const {
    std::lock_guard<std::mutex> state(stateMutex_);
    Json status = clientStatus_;
    Json snapshot{{"available", process_ != nullptr && !processExited_},
        {"pid", processId_ ? Json(processId_) : Json(nullptr)},
        {"state", state_}, {"peerAddress", peerAddress_}, {"peerPort", 8321},
        {"localPort", 8321}, {"channels", 2}, {"sampleRate", 48000},
        {"maxQueueMs", maxQueueMs_}, {"latencyMs", nullptr}, {"driftPpm", nullptr},
        {"status", std::move(status)}, {"error", error_.empty() ? Json(nullptr) : Json(error_)},
        {"stderrTail", stderrTail_}};
    if (processId_) snapshot["exitCode"] = processExited_ ? Json(exitCode_) : Json(nullptr);
    if (clientStatus_.contains("block_frames")) snapshot["blockFrames"] = clientStatus_["block_frames"];
    return snapshot;
}

std::uint64_t PwarClientSupervisor::Revision() const noexcept {
    std::lock_guard<std::mutex> state(stateMutex_);
    return revision_;
}

void PwarClientSupervisor::readOutput(HANDLE pipe, bool standardError) noexcept {
    std::array<char, 4096> buffer{};
    std::string line;
    bool oversized = false;
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) || !read) break;
        if (standardError) {
            std::lock_guard<std::mutex> state(stateMutex_);
            stderrTail_.append(buffer.data(), read);
            if (stderrTail_.size() > 4096) stderrTail_.erase(0, stderrTail_.size() - 4096);
            ++revision_;
            continue;
        }
        for (DWORD index = 0; index < read; ++index) {
            const char character = buffer[index];
            if (character == '\n') {
                if (oversized) {
                    processLine("{\"version\":1,\"protocol_error\":\"MESSAGE_TOO_LARGE\"}", false);
                } else {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    processLine(std::move(line), false);
                }
                line.clear(); oversized = false;
            } else if (!oversized) {
                if (line.size() == kMaximumLineBytes) { line.clear(); oversized = true; }
                else line.push_back(character);
            }
        }
    }
    if (!line.empty() && !oversized) processLine(std::move(line), false);
}

void PwarClientSupervisor::processLine(std::string line, bool standardError) noexcept {
    if (standardError) return;
    try {
        const auto message = Json::parse(line);
        if (!message.is_object()) throw std::runtime_error("Protocol line is not an object");
        if (!message.contains("version") || !message["version"].is_number_integer() || message["version"] != 1)
            throw std::runtime_error("Unsupported or missing protocol version");
        std::lock_guard<std::mutex> state(stateMutex_);
        if (message.contains("status") && message["status"].is_object()) {
            clientStatus_ = message["status"];
            state_ = clientStatus_.value("state", state_);
        }
        if (message.contains("id") && message["id"].is_string()) {
            responses_[message["id"].get<std::string>()] = message;
            while (responses_.size() > 64) responses_.erase(responses_.begin());
        }
        if (message.contains("event") && message["event"].is_string()) {
            const auto event = message["event"].get<std::string>();
            if (event == "asio_start_accepted") startAccepted_ = true;
            if (event == "stopped") { state_ = "stopped"; stopRequested_ = true; }
            if (event == "error") {
                state_ = "error";
                const auto detail = message.value("error", Json::object());
                error_ = detail.value("code", std::string("PWAR_START_FAILED"));
            }
        }
        if (message.contains("protocol_error")) {
            state_ = "error";
            error_ = "PWAR client emitted an oversized JSON line";
        }
        ++revision_;
    } catch (const std::exception& exception) {
        std::lock_guard<std::mutex> state(stateMutex_);
        state_ = "error";
        error_ = std::string("Invalid PWAR client JSON: ") + exception.what();
        ++revision_;
    }
    stateChanged_.notify_all();
}

void PwarClientSupervisor::waitForExit() noexcept {
    HANDLE process = nullptr;
    { std::lock_guard<std::mutex> state(stateMutex_); process = process_; }
    if (!process || WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) return;
    DWORD code = 0;
    GetExitCodeProcess(process, &code);
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        processExited_ = true;
        exitCode_ = code;
        if (stopRequested_ || state_ == "stopped") state_ = "stopped";
        else {
            state_ = "error";
            if (error_.empty()) error_ = "PWAR client exited unexpectedly with code " + std::to_string(code);
        }
        ++revision_;
    }
    stateChanged_.notify_all();
}

void PwarClientSupervisor::closeChild() noexcept {
    HANDLE process = nullptr, input = nullptr, output = nullptr, error = nullptr;
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        process = process_; input = stdinWrite_; output = stdoutRead_; error = stderrRead_;
    }
    if (input) { CloseHandle(input); }
    if (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        TerminateProcess(process, ERROR_CANCELLED);
        WaitForSingleObject(process, 1000);
    }
    if (stdoutThread_.joinable() && stdoutThread_.get_id() != std::this_thread::get_id()) stdoutThread_.join();
    if (stderrThread_.joinable() && stderrThread_.get_id() != std::this_thread::get_id()) stderrThread_.join();
    if (waitThread_.joinable() && waitThread_.get_id() != std::this_thread::get_id()) waitThread_.join();
    if (output) CloseHandle(output);
    if (error) CloseHandle(error);
    if (process) CloseHandle(process);
    {
        std::lock_guard<std::mutex> state(stateMutex_);
        process_ = nullptr; stdinWrite_ = nullptr; stdoutRead_ = nullptr; stderrRead_ = nullptr;
        processExited_ = true;
        if (state_ == "stopping") state_ = "stopped";
        ++revision_;
    }
    stateChanged_.notify_all();
}
