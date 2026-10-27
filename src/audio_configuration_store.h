// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <string>

struct AudioControllerConfiguration;

enum class ConfigurationLoadStatus {
    Missing,
    Loaded,
    Error
};

class AudioConfigurationStore final {
public:
    explicit AudioConfigurationStore(std::wstring path);

    static std::wstring DefaultPath();
    ConfigurationLoadStatus Load(AudioControllerConfiguration& configuration,
                                 std::string& error) const;
    bool Save(const AudioControllerConfiguration& configuration, std::string& error) const;

private:
    std::wstring path_;
};
