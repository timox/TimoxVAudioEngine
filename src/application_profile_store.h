// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "application_profile.h"

#include <string>
#include <string_view>
#include <vector>

class ApplicationProfileStore final {
public:
    explicit ApplicationProfileStore(std::wstring path);

    static std::wstring DefaultPath();
    static std::vector<ApplicationProfile> DefaultProfiles();
    static bool NormalizeAndValidate(std::vector<ApplicationProfile>& profiles,
                                     std::string& error);
    static ApplicationProfile EffectiveProfileForExecutable(
        std::wstring_view executablePath, const std::vector<ApplicationProfile>& profiles);

    bool Load(std::vector<ApplicationProfile>& profiles, std::string& error) const;
    bool Save(const std::vector<ApplicationProfile>& profiles, std::string& error) const;

private:
    std::wstring path_;
};
