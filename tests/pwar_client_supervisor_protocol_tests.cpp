#include <nlohmann/json.hpp>
#include <Windows.h>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <cstdio>
// Exercise the parser without launching a client or exposing a production API.
#define private public
#include "pwar_client_supervisor.h"
#undef private
#include "../src/pwar_client_supervisor.cpp"

int main() {
    std::filesystem::path resolved;
    std::string error;
    if (!resolveClientPath(resolved, error) && error != "The verified PWAR client is missing beside the engine") return 3;
    if (!resolved.empty() && resolved.filename() != L"PwarAsioClient.exe") return 4;
    using Json = nlohmann::json;
    for (const auto& version : {Json(), Json(0), Json(2), Json("1"), Json(true), Json(1.0)}) {
        PwarClientSupervisor supervisor;
        Json message{{"event", "asio_start_accepted"}, {"id", "test"},
                     {"status", {{"state", "running"}}}};
        if (!version.is_null()) message["version"] = version;
        supervisor.processLine(message.dump(), false);
        if (supervisor.startAccepted_ || !supervisor.responses_.empty() ||
            !supervisor.clientStatus_.empty() || supervisor.state_ != "error") {
            std::fprintf(stderr, "Unsupported/missing version mutated accepted state: %s\n", message.dump().c_str());
            return 1;
        }
    }
    PwarClientSupervisor supervisor;
    supervisor.processLine(R"({"version":1,"event":"asio_start_accepted","id":"ok","status":{"state":"running"}})", false);
    if (!supervisor.startAccepted_ || supervisor.responses_.count("ok") != 1 || supervisor.state_ != "running") return 2;
    std::puts("PWAR version gate preserves accepted state and accepts protocol v1");
}


