// SPDX-License-Identifier: GPL-3.0-only
#include "application_profile_store.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <utility>

namespace {
using Json = nlohmann::json;
constexpr std::uint32_t kStoreVersion = 1;
constexpr DWORD kMaximumProfileBytes = 1024 * 1024;

bool isDirectory(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ensureParentDirectory(const std::wstring& path, std::string& error) {
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return true;
    const auto directory = path.substr(0, separator);
    if ((CreateDirectoryW(directory.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) &&
        isDirectory(directory)) return true;
    error = "Unable to create the TimoxVasio application profile directory";
    return false;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), bytes, nullptr, nullptr) != bytes)
        return {};
    return result;
}

Json encodeProfiles(const std::vector<ApplicationProfile>& profiles) {
    Json items = Json::array();
    for (const auto& profile : profiles) {
        items.push_back({
            {"processName", utf8(profile.processName)},
            {"inputChannels", profile.inputChannels},
            {"outputChannels", profile.outputChannels}
        });
    }
    return Json{{"version", kStoreVersion}, {"profiles", std::move(items)}};
}
}

bool ApplicationProfileStore::Save(
    const std::vector<ApplicationProfile>& profiles, std::string& error) const {
    error.clear();
    if (path_.empty()) {
        error = "LOCALAPPDATA is unavailable; cannot save application profiles";
        return false;
    }
    if (!ensureParentDirectory(path_, error)) return false;
    auto normalized = profiles;
    if (!NormalizeAndValidate(normalized, error)) return false;

    std::string contents;
    try {
        contents = encodeProfiles(normalized).dump();
    } catch (const Json::exception&) {
        error = "The TimoxVasio application profiles cannot be serialized";
        return false;
    }
    if (contents.size() > kMaximumProfileBytes) {
        error = "The TimoxVasio application profiles exceed the supported size";
        return false;
    }

    const std::wstring temporaryPath = path_ + L".tmp";
    HANDLE file = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "Unable to create a temporary application profile file";
        return false;
    }
    DWORD written = 0;
    const BOOL write = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr);
    const BOOL flushed = write && written == contents.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed) {
        DeleteFileW(temporaryPath.c_str());
        error = "Unable to write the complete application profile file";
        return false;
    }
    if (!MoveFileExW(temporaryPath.c_str(), path_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        error = "Unable to atomically replace the application profile file";
        return false;
    }
    return true;
}
