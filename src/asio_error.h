#pragma once
#include <string>

// ASIO's error-message contract provides 124 bytes. Keep an extra zero byte
// so a driver filling the entire documented buffer cannot cause an over-read.
// Only call on the host control thread, immediately after the failed operation.
template<class Driver>
std::string DescribeAsioFailure(Driver& driver, const char* operation, long code) {
    char detail[125]{};
    driver.getErrorMessage(detail);
    detail[124] = '\0';
    std::string message = std::string("ASIO ") + operation + " failed (code " + std::to_string(code) + ")";
    if (detail[0]) message += std::string(": ") + detail;
    return message;
}
