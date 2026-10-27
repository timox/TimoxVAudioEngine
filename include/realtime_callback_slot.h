#pragma once

#include "physical_asio_host.h"

#include <atomic>
#include <cstdint>
#include <windows.h>

// The controller owns every published context. Publish a replacement, then
// wait for callbacks that may still hold the old context before destroying it.
class RealtimeCallbackSlot final {
public:
    explicit RealtimeCallbackSlot(PhysicalAsioProcessCallback callback) noexcept
        : callback_(callback) {
        static_assert(std::atomic<void*>::is_always_lock_free);
        static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    }

    void Publish(void* context) noexcept {
        current_.store(context, std::memory_order_seq_cst);
    }

    void WaitForQuiescence() const noexcept {
        while (activeCallbacks_.load(std::memory_order_seq_cst) != 0) Sleep(1);
    }

    static void Dispatch(const float* const* inputs, float* const* outputs,
        std::uint32_t frames, void* context) noexcept {
        auto& slot = *static_cast<RealtimeCallbackSlot*>(context);
        slot.activeCallbacks_.fetch_add(1, std::memory_order_seq_cst);
        void* current = slot.current_.load(std::memory_order_seq_cst);
        if (current) slot.callback_(inputs, outputs, frames, current);
        slot.activeCallbacks_.fetch_sub(1, std::memory_order_seq_cst);
    }

private:
    PhysicalAsioProcessCallback callback_;
    std::atomic<void*> current_{nullptr};
    mutable std::atomic<std::uint32_t> activeCallbacks_{0};
};
