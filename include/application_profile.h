// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct ApplicationProfile {
    std::wstring processName;
    std::uint32_t inputChannels = 256;
    std::uint32_t outputChannels = 256;
};

struct ApplicationProfileClient {
    std::uint32_t processId = 0;
    std::wstring processName;
};

struct ApplicationProfilesSnapshot {
    std::vector<ApplicationProfile> profiles;
    std::string error;
};

struct ApplicationProfilesUpdateResult {
    bool success = false;
    std::string error;
    std::vector<ApplicationProfile> profiles;
    std::vector<ApplicationProfileClient> restartRequiredClients;
};
