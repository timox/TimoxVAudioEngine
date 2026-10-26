// SPDX-License-Identifier: GPL-3.0-only
#include "application_profile_store.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>

namespace {
using Json = nlohmann::json;
constexpr std::uint32_t kStoreVersion = 1;
constexpr DWORD kMaximumProfileBytes = 1024 * 1024;

bool validExecutableName(const std::wstring& name) {
    if (name.size() < 5 || name.size() > 260 || name == L"." || name == L"..") return false;
    for (const wchar_t character : name) {
        if (character < 0x20 || character == L'\\' || character == L'/' ||
            character == L':' || character == L'*' || character == L'?' ||
            character == L'"' || character == L'<' || character == L'>' || character == L'|')
            return false;
    }
    return CompareStringOrdinal(name.data() + name.size() - 4, 4, L".exe", 4, TRUE) == CSTR_EQUAL;
}

bool wide(const std::string& value, std::wstring& result) {
    if (value.empty()) { result.clear(); return true; }
    const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (characters <= 0) return false;
    result.resize(static_cast<std::size_t>(characters));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), characters) == characters;
}

bool decodeProfiles(const Json& value, std::vector<ApplicationProfile>& profiles) {
    if (!value.is_object() || value.size() != 2 || !value.contains("version") ||
        !value["version"].is_number_unsigned() ||
        value["version"].get<std::uint32_t>() != kStoreVersion ||
        !value.contains("profiles") || !value["profiles"].is_array() ||
        value["profiles"].size() > 1024) return false;

    std::vector<ApplicationProfile> decoded;
    decoded.reserve(value["profiles"].size());
    for (const auto& item : value["profiles"]) {
        if (!item.is_object() || item.size() != 3 || !item.contains("processName") ||
            !item["processName"].is_string() || !item.contains("inputChannels") ||
            !item["inputChannels"].is_number_unsigned() || !item.contains("outputChannels") ||
            !item["outputChannels"].is_number_unsigned()) return false;
        const auto inputChannels = item["inputChannels"].get<std::uint64_t>();
        const auto outputChannels = item["outputChannels"].get<std::uint64_t>();
        if (inputChannels == 0 || inputChannels > 256 || outputChannels == 0 || outputChannels > 256)
            return false;
        std::wstring processName;
        if (!wide(item["processName"].get<std::string>(), processName)) return false;
        decoded.push_back({std::move(processName),
            static_cast<std::uint32_t>(inputChannels), static_cast<std::uint32_t>(outputChannels)});
    }
    std::string error;
    if (!ApplicationProfileStore::NormalizeAndValidate(decoded, error)) return false;
    profiles = std::move(decoded);
    return true;
}
}

ApplicationProfileStore::ApplicationProfileStore(std::wstring path) : path_(std::move(path)) {}

std::wstring ApplicationProfileStore::DefaultPath() {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (!required) return {};
    std::wstring localAppData(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData.data(), required);
    if (!written || written >= required) return {};
    localAppData.resize(written);
    return localAppData + L"\\TimoxVasio\\application-profiles.json";
}

std::vector<ApplicationProfile> ApplicationProfileStore::DefaultProfiles() {
    return {{L"mixxx.exe", 255, 255}};
}

bool ApplicationProfileStore::NormalizeAndValidate(
    std::vector<ApplicationProfile>& profiles, std::string& error) {
    error.clear();
    if (profiles.size() > 1024) {
        error = "At most 1024 application profiles are supported";
        return false;
    }
    for (auto& profile : profiles) {
        if (!validExecutableName(profile.processName) || profile.inputChannels == 0 ||
            profile.inputChannels > 256 || profile.outputChannels == 0 ||
            profile.outputChannels > 256) {
            error = "Each profile requires an executable basename and channel counts from 1 to 256";
            return false;
        }
        if (!CharLowerBuffW(profile.processName.data(), static_cast<DWORD>(profile.processName.size()))) {
            error = "Unable to normalize an application profile process name";
            return false;
        }
    }
    std::sort(profiles.begin(), profiles.end(), [](const auto& left, const auto& right) {
        return CompareStringOrdinal(left.processName.data(), static_cast<int>(left.processName.size()),
            right.processName.data(), static_cast<int>(right.processName.size()), TRUE) == CSTR_LESS_THAN;
    });
    for (std::size_t index = 1; index < profiles.size(); ++index) {
        const auto& previous = profiles[index - 1].processName;
        const auto& current = profiles[index].processName;
        if (CompareStringOrdinal(previous.data(), static_cast<int>(previous.size()),
                current.data(), static_cast<int>(current.size()), TRUE) == CSTR_EQUAL) {
            error = "Application profile process names must be unique, ignoring case";
            return false;
        }
    }
    return true;
}

ApplicationProfile ApplicationProfileStore::EffectiveProfileForExecutable(
    std::wstring_view executablePath, const std::vector<ApplicationProfile>& profiles) {
    const auto separator = executablePath.find_last_of(L"\\/");
    const auto name = separator == std::wstring_view::npos
        ? executablePath : executablePath.substr(separator + 1);
    for (const auto& profile : profiles) {
        if (CompareStringOrdinal(profile.processName.data(), static_cast<int>(profile.processName.size()),
                name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL)
            return profile;
    }
    return {std::wstring(name), 256, 256};
}

bool ApplicationProfileStore::Load(
    std::vector<ApplicationProfile>& profiles, std::string& error) const {
    error.clear();
    if (path_.empty()) {
        error = "LOCALAPPDATA is unavailable; cannot locate application profiles";
        return false;
    }
    HANDLE file = CreateFileW(path_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
            profiles = DefaultProfiles();
            return true;
        }
        error = "Unable to open the TimoxVasio application profile file";
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > kMaximumProfileBytes) {
        CloseHandle(file);
        error = "The TimoxVasio application profile file has an invalid size";
        return false;
    }
    std::string contents(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD bytesRead = 0;
    const BOOL read = contents.empty() || ReadFile(file, contents.data(),
        static_cast<DWORD>(contents.size()), &bytesRead, nullptr);
    CloseHandle(file);
    if (!read || bytesRead != contents.size()) {
        error = "Unable to read the complete TimoxVasio application profile file";
        return false;
    }
    try {
        if (!decodeProfiles(Json::parse(contents), profiles)) {
            error = "The TimoxVasio application profile file does not match the supported schema";
            return false;
        }
    } catch (const Json::exception&) {
        error = "The TimoxVasio application profile file is malformed";
        return false;
    }
    return true;
}
