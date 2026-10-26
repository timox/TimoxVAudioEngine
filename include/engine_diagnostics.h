#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class EngineDiagnostics final {
public:
    enum class Level { Debug, Info, Warning, Error };

    struct Entry {
        std::string timestamp;
        std::string level;
        std::string component;
        std::string message;
    };

    static std::wstring DefaultDirectory();
    static bool ParseLevel(std::string_view value, Level& level);
    static std::string_view LevelName(Level level);

    bool Initialize(std::wstring directory, std::string& error);
    bool SetLevel(Level level, std::string& error);
    Level GetLevel() const;
    bool Write(Level level, std::string_view component, std::string_view message);
    std::vector<Entry> ReadRecent(std::size_t limit) const;

private:
    bool saveLevel(Level level, std::string& error) const;
    bool rotateIfNeeded(std::size_t incomingBytes);

    mutable std::mutex mutex_;
    std::wstring directory_;
    std::wstring logPath_;
    std::wstring settingsPath_;
    Level level_ = Level::Info;
    bool initialized_ = false;
};
