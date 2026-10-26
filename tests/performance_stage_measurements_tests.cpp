#include "physical_asio_host.h"

#include <atomic>
#include <thread>

int main() {
    StageTimingAccumulator accumulator;
    auto empty = accumulator.Consume();
    if (empty.samples || empty.sumTicks || empty.worstTicks) return 1;
    accumulator.Add(12);
    accumulator.Add(7);
    const auto window = accumulator.Consume();
    if (window.samples != 2 || window.sumTicks != 19 || window.worstTicks != 12) return 2;
    empty = accumulator.Consume();
    if (empty.samples || empty.sumTicks || empty.worstTicks) return 3;
    std::atomic<bool> running{true};
    std::thread producer([&] {
        for (int index = 0; index < 100000; ++index) accumulator.Add(7);
        running.store(false, std::memory_order_release);
    });
    std::uint64_t observed = 0;
    bool consistent = true;
    do {
        const auto sample = accumulator.Consume();
        if (sample.sumTicks != sample.samples * 7 ||
            (sample.samples && sample.worstTicks != 7) ||
            (!sample.samples && sample.worstTicks)) consistent = false;
        observed += sample.samples;
    } while (running.load(std::memory_order_acquire));
    producer.join();
    if (!consistent) return 4;
    const auto final = accumulator.Consume();
    if (final.sumTicks != final.samples * 7 ||
        (final.samples && final.worstTicks != 7) ||
        (!final.samples && final.worstTicks)) return 5;
    observed += final.samples;
    if (!observed || observed > 100000) return 6;
}
