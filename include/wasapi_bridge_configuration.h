#pragma once

#include "wasapi_backend.h"
#include <string>
#include <vector>

// Desired configuration only: no backend is opened by parsing or validation.
// IDs are stable ASCII [A-Za-z0-9_-], length 1..128. At most 32 bridges.
struct WasapiBridgeConfiguration {
    std::string id;
    bool enabled = true;
    WasapiOpenConfig open;
};

// Control-thread helpers. Reject malformed UTF, embedded NUL and endpoint IDs
// longer than 32768 UTF-16 code units. Outputs remain unchanged on failure.
bool WasapiEndpointFromUtf8(const std::string& input, std::wstring& output);
bool WasapiEndpointToUtf8(const std::wstring& input, std::string& output);
bool ValidateWasapiBridgeConfigurations(const std::vector<WasapiBridgeConfiguration>& bridges,
    std::string& error);
