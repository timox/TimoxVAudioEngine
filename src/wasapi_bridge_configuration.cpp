#include "wasapi_bridge_configuration.h"
#include <algorithm>
#include <unordered_set>
#include <utility>

bool WasapiEndpointFromUtf8(const std::string& input, std::wstring& output) {
    if (input.empty() || input.size() > 4 * 32768 || input.find('\0') != std::string::npos) return false;
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0);
    if (length <= 0 || length > 32768) return false;
    std::wstring converted(length, L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), converted.data(), length) != length) return false;
    output = std::move(converted);
    return true;
}

bool WasapiEndpointToUtf8(const std::wstring& input, std::string& output) {
    if (input.empty() || input.size() > 32768 || input.find(L'\0') != std::wstring::npos) return false;
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return false;
    std::string converted(length, '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), converted.data(), length, nullptr, nullptr) != length) return false;
    output = std::move(converted);
    return true;
}

bool ValidateWasapiBridgeConfigurations(const std::vector<WasapiBridgeConfiguration>& bridges,
    std::string& error) {
    error.clear();
    const auto fail = [&error](const char* message) { error = message; return false; };
    if (bridges.size() > 32) return fail("At most 32 WASAPI bridges are supported");
    std::unordered_set<std::string> ids;
    for (const auto& bridge : bridges) {
        if (bridge.id.empty() || bridge.id.size() > 128 ||
            !std::all_of(bridge.id.begin(), bridge.id.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
            }) || !ids.insert(bridge.id).second) return fail("Invalid or duplicate WASAPI bridge ID");
        const auto& config = bridge.open;
        if (config.kind == WasapiStreamKind::ProcessLoopback) {
            std::string image;
            if (!config.processId || !config.processCreationTime || !config.endpointId.empty() || config.defaultRole ||
                !WasapiEndpointToUtf8(config.processImagePath, image) || image.size() > 32768)
                return fail("Process loopback requires a valid process identity and no endpoint selector");
        } else {
            if (config.kind != WasapiStreamKind::Capture && config.kind != WasapiStreamKind::Render &&
                config.kind != WasapiStreamKind::EndpointLoopback) return fail("Unsupported WASAPI bridge kind");
            if (config.endpointId.empty() == !config.defaultRole.has_value())
                return fail("WASAPI bridge requires exactly one endpoint ID or default role");
        }
        if (config.defaultRole && *config.defaultRole != WasapiDefaultRole::Console &&
            *config.defaultRole != WasapiDefaultRole::Multimedia &&
            *config.defaultRole != WasapiDefaultRole::Communications) return fail("Invalid WASAPI default role");
        if (!config.endpointId.empty()) {
            std::string encoded;
            if (!WasapiEndpointToUtf8(config.endpointId, encoded)) return fail("Invalid WASAPI endpoint ID Unicode");
        }
        if (config.bufferDuration100ns < 0 || config.bufferDuration100ns > 10000000)
            return fail("WASAPI buffer duration must be between zero and one second");
    }
    return true;
}
