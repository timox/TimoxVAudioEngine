#include "asio_error.h"
#include <cstring>
#include <iostream>

struct Driver {
    const char* message = "";
    bool terminate = true;
    unsigned calls = 0;
    void getErrorMessage(char* output) {
        ++calls;
        if (terminate) std::strcpy(output, message);
        else std::memset(output, 'x', 124);
    }
};
int main() {
    Driver driver;
    const auto empty = DescribeAsioFailure(driver, "start", -999);
    if (empty != "ASIO start failed (code -999)" || driver.calls != 1) return 1;
    driver.message = "Cannot initialize or bind UDP listener on port 8321";
    const auto detailed = DescribeAsioFailure(driver, "start", -999);
    if (detailed != empty + ": " + driver.message) return 2;
    driver.terminate = false;
    const auto bounded = DescribeAsioFailure(driver, "createBuffers", -998);
    if (bounded != "ASIO createBuffers failed (code -998): " + std::string(124, 'x')) return 3;
    std::cout << "ASIO failure context, driver detail and bounded unterminated message passed\n";
}
