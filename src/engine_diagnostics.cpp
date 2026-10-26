#include "engine_diagnostics.h"

#include <windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
using Json = nlohmann::json;
constexpr std::uintmax_t kMaxLogBytes = 5u * 1024u * 1024u;
constexpr int kArchiveCount = 4;

std::string timestampNow() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &seconds);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char value[40]{};
    std::snprintf(value, sizeof(value), "%04d-%02d-%02dT%02d:%02d:%02d.%03lldZ",
        utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec,
        static_cast<long long>(millis));
    return value;
}
}

std::wstring EngineDiagnostics::DefaultDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    return (std::filesystem::path(localAppData) / L"TimoxVasio" / L"logs").wstring();
}

bool EngineDiagnostics::ParseLevel(std::string_view value, Level& level) {
    if (value == "debug") { level = Level::Debug; return true; }
    if (value == "info") { level = Level::Info; return true; }
    if (value == "warning") { level = Level::Warning; return true; }
    if (value == "error") { level = Level::Error; return true; }
    return false;
}

std::string_view EngineDiagnostics::LevelName(Level level) {
    switch (level) {
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warning: return "warning";
    case Level::Error: return "error";
    }
    return "info";
}

bool EngineDiagnostics::Initialize(std::wstring directory, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    try {
        directory_ = std::move(directory);
        std::filesystem::create_directories(directory_);
        logPath_ = (std::filesystem::path(directory_) / L"engine.log").wstring();
        auto settingsDirectory = std::filesystem::path(directory_);
        if (settingsDirectory.filename() == L"logs") settingsDirectory = settingsDirectory.parent_path();
        std::filesystem::create_directories(settingsDirectory);
        settingsPath_ = (settingsDirectory / L"diagnostics.json").wstring();
        std::ifstream settings{std::filesystem::path(settingsPath_)};
        if (settings) {
            Json value; settings >> value;
            Level persisted{};
            if (value.is_object() && value.contains("level") && value["level"].is_string() &&
                ParseLevel(value["level"].get<std::string>(), persisted)) level_ = persisted;
        }
        initialized_ = true;
        error.clear();
        return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

bool EngineDiagnostics::saveLevel(Level level, std::string& error) const {
    try {
        const auto temp = settingsPath_ + L".tmp";
        std::ofstream output(std::filesystem::path(temp), std::ios::trunc);
        output << Json{{"level", LevelName(level)}}.dump(2) << '\n';
        output.close();
        if (!output) { error = "could not write diagnostics settings"; return false; }
        if (!MoveFileExW(temp.c_str(), settingsPath_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            error = "could not replace diagnostics settings"; return false;
        }
        return true;
    } catch (const std::exception& ex) { error = ex.what(); return false; }
}

bool EngineDiagnostics::SetLevel(Level level, std::string& error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) { error = "diagnostics are not initialized"; return false; }
    if (!saveLevel(level, error)) return false;
    level_ = level;
    error.clear();
    return true;
}

EngineDiagnostics::Level EngineDiagnostics::GetLevel() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return level_;
}

bool EngineDiagnostics::rotateIfNeeded(std::size_t incomingBytes) {
    try {
        const std::filesystem::path current(logPath_);
        const auto size = std::filesystem::exists(current) ? std::filesystem::file_size(current) : 0;
        if (size + incomingBytes <= kMaxLogBytes) return true;
        std::error_code ec;
        std::filesystem::remove(current.wstring() + L"." + std::to_wstring(kArchiveCount), ec);
        for (int i = kArchiveCount - 1; i >= 1; --i) {
            const auto from = current.wstring() + L"." + std::to_wstring(i);
            if (std::filesystem::exists(from))
                MoveFileExW(from.c_str(), (current.wstring() + L"." + std::to_wstring(i + 1)).c_str(), MOVEFILE_REPLACE_EXISTING);
        }
        if (std::filesystem::exists(current)) MoveFileExW(current.c_str(), (current.wstring() + L".1").c_str(), MOVEFILE_REPLACE_EXISTING);
        return true;
    } catch (...) { return false; }
}

bool EngineDiagnostics::Write(Level level, std::string_view component, std::string_view message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || (level == Level::Debug && level_ != Level::Debug)) return false;
    try {
        Json entry{{"timestamp", timestampNow()}, {"level", LevelName(level)}, {"component", component}, {"message", message}};
        const auto line = entry.dump() + "\n";
        if (!rotateIfNeeded(line.size())) return false;
        std::ofstream output(std::filesystem::path(logPath_), std::ios::binary | std::ios::app);
        output.write(line.data(), static_cast<std::streamsize>(line.size()));
        return static_cast<bool>(output);
    } catch (...) { return false; }
}

std::vector<EngineDiagnostics::Entry> EngineDiagnostics::ReadRecent(std::size_t limit) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Entry> entries;
    if (!initialized_ || limit == 0) return entries;
    limit = std::min<std::size_t>(limit, 500);
    std::vector<std::filesystem::path> files;
    for (int i = kArchiveCount; i >= 1; --i) {
        auto path = std::filesystem::path(logPath_ + L"." + std::to_wstring(i));
        if (std::filesystem::exists(path)) files.push_back(path);
    }
    if (std::filesystem::exists(logPath_)) files.emplace_back(logPath_);
    for (const auto& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string line;
        while (std::getline(input, line)) {
            try {
                const auto item = Json::parse(line);
                entries.push_back({item.value("timestamp", ""), item.value("level", "info"),
                    item.value("component", ""), item.value("message", "")});
            } catch (...) { }
        }
    }
    if (entries.size() > limit) entries.erase(entries.begin(), entries.end() - static_cast<std::ptrdiff_t>(limit));
    return entries;
}
