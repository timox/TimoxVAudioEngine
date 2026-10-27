#include "realtime_callback_slot.h"

#include <atomic>
#include <cstdio>
#include <thread>

namespace {
struct Context {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{true};
    std::atomic<unsigned> calls{0};
};

void callback(const float* const*, float* const*, std::uint32_t, void* pointer) noexcept {
    auto& context = *static_cast<Context*>(pointer);
    context.entered.store(true, std::memory_order_release);
    while (!context.release.load(std::memory_order_acquire)) std::this_thread::yield();
    context.calls.fetch_add(1, std::memory_order_relaxed);
}
}

int main() {
    RealtimeCallbackSlot slot(callback);
    Context old;
    Context next;
    old.release.store(false);
    slot.Publish(&old);
    std::thread audio([&] { RealtimeCallbackSlot::Dispatch(nullptr, nullptr, 64, &slot); });
    while (!old.entered.load(std::memory_order_acquire)) std::this_thread::yield();
    slot.Publish(&next);
    std::atomic<bool> waiting{false};
    std::atomic<bool> quiescent{false};
    std::thread controller([&] {
        waiting.store(true, std::memory_order_release);
        slot.WaitForQuiescence();
        quiescent.store(true, std::memory_order_release);
    });
    while (!waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    Sleep(10);
    const bool waitedForOld = !quiescent.load(std::memory_order_acquire);
    old.release.store(true, std::memory_order_release);
    audio.join();
    controller.join();
    RealtimeCallbackSlot::Dispatch(nullptr, nullptr, 64, &slot);
    const bool passed = waitedForOld && old.calls.load() == 1 &&
        next.calls.load() == 1 && quiescent.load();
    std::puts(passed ? "PASS: callback context replacement waits for the old callback."
                     : "FAIL: callback context lifetime was not protected.");
    return passed ? 0 : 1;
}
