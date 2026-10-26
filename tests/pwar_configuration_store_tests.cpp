#include "pwar_configuration_store.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#define CHECK(x) do { if (!(x)) { std::cerr << "line " << __LINE__ << ": " #x "\n"; return 1; } } while(false)
int main() {
    const auto path = std::filesystem::temp_directory_path() / (L"timox-pwar-test-" + std::to_wstring(GetCurrentProcessId()) + L".cfg");
    CHECK(!std::filesystem::exists(path));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove(path, error); } } cleanup{path};
    PwarConfigurationStore store(path.wstring());
    PwarSettings settings; std::string error;
    CHECK(store.Load(settings, error)); CHECK(!settings.exists && settings.port == 8321);
    { std::ofstream file(path); file << "# keep this\nother=value\nudp_send_ip=10.1.0.2\nudp_send_port=8321\nudp_send_ip=10.1.0.3\n"; }
    CHECK(store.Load(settings, error)); CHECK(settings.address == "10.1.0.3");
    CHECK(store.Save({"127.0.0.1", 18321, true}, error));
    CHECK(store.Load(settings, error)); CHECK(settings.address == "127.0.0.1" && settings.port == 18321);
    const auto read = [&] { std::ifstream file(path); return std::string(std::istreambuf_iterator<char>(file), {}); };
    const auto saved = read(); CHECK(saved.find("# keep this") != std::string::npos && saved.find("other=value") != std::string::npos);
    CHECK(saved.find("10.1.0.") == std::string::npos);
    for (const auto* bad : {"", "localhost", "1.2.3.256", "01.2.3.4", "1.2.3.4\nother=x"}) {
        CHECK(!store.Save({bad, 8321, true}, error)); CHECK(read() == saved);
    }
    CHECK(!store.Save({"127.0.0.1", 0, true}, error)); CHECK(read() == saved);
    CHECK(!store.Save({"127.0.0.1", 65536, true}, error)); CHECK(read() == saved);
    std::cout << "PWAR configuration persistence passed\n";
}
