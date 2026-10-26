#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

// Fixed-format interleaved storage. Construct/prefill before publishing to the
// audio threads; destroy only after both threads have stopped using the ring.
// Exactly one producer calls Write/WritePlanar, exactly one consumer calls
// Read/ReadPlanar. The two APIs may be mixed on their respective owning thread.
// Transfers never allocate, lock, wait, overwrite unread data, or pad silence.
class SpscAudioRing final {
public:
    SpscAudioRing(std::size_t channels, std::size_t capacityFrames)
        : channels_(channels), capacity_(capacityFrames),
          samples_(ValidateDimensions(channels, capacityFrames), 0.0f) {}

    SpscAudioRing(const SpscAudioRing&) = delete;
    SpscAudioRing& operator=(const SpscAudioRing&) = delete;

    // Buffers must contain frames * channels samples (or frames per plane).
    // With frames > 0 all pointers must be valid; null is allowed for zero.
    // Return actual frames transferred; caller retains/retries any unwritten tail.
    std::size_t Write(const float* interleaved, std::size_t frames) noexcept {
        return Produce(frames, [&](std::size_t slot, std::size_t frame) noexcept {
            std::copy_n(interleaved + frame * channels_, channels_, samples_.data() + slot * channels_);
        });
    }

    std::size_t WritePlanar(const float* const* planes, std::size_t frames) noexcept {
        return Produce(frames, [&](std::size_t slot, std::size_t frame) noexcept {
            for (std::size_t channel = 0; channel < channels_; ++channel)
                samples_[slot * channels_ + channel] = planes[channel][frame];
        });
    }

    // Return actual frames copied. Unread destination samples are untouched:
    // callers requiring a whole block MUST silence-pad [returned, frames).
    std::size_t Read(float* interleaved, std::size_t frames) noexcept {
        return Consume(frames, [&](std::size_t slot, std::size_t frame) noexcept {
            std::copy_n(samples_.data() + slot * channels_, channels_, interleaved + frame * channels_);
        });
    }

    // Same short-read/silence-padding contract as Read, separately for each plane.
    std::size_t ReadPlanar(float* const* planes, std::size_t frames) noexcept {
        return Consume(frames, [&](std::size_t slot, std::size_t frame) noexcept {
            for (std::size_t channel = 0; channel < channels_; ++channel)
                planes[channel][frame] = samples_[slot * channels_ + channel];
        });
    }

    std::size_t Capacity() const noexcept { return capacity_; }

    // Snapshot only, not a reservation. Exact when quiescent; a third observer
    // can sample different instants, so its result is bounded to [0, Capacity()].
    std::size_t Available() const noexcept {
        const auto read = read_.value.load(std::memory_order_acquire);
        const auto write = write_.value.load(std::memory_order_acquire);
        return (std::min)(Distance(write, read), capacity_);
    }

    // One incident per nonzero short operation, including a transfer of zero.
    std::uint64_t Underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t Overruns() const noexcept { return overruns_.load(std::memory_order_relaxed); }

private:
    static_assert(std::atomic<std::size_t>::is_always_lock_free, "Audio cursors must be lock-free");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free, "Audio counters must be lock-free");

    static std::size_t ValidateDimensions(std::size_t channels, std::size_t capacity) {
        if (channels == 0 || capacity == 0)
            throw std::invalid_argument("Audio ring dimensions must be nonzero");
        const auto maximum = (std::numeric_limits<std::size_t>::max)();
        if (capacity > maximum / 2 || channels > maximum / sizeof(float) / capacity)
            throw std::length_error("Audio ring dimensions overflow storage or cursor range");
        return channels * capacity;
    }

    // Cursors wrap at twice the capacity, preserving both full/empty distinction
    // and the storage position for arbitrary capacities over indefinite uptime.
    // Wrapping a free-running integer then taking % capacity would not do that.
    std::size_t Distance(std::size_t write, std::size_t read) const noexcept {
        return write >= read ? write - read : capacity_ * 2 - (read - write);
    }

    std::size_t Advance(std::size_t cursor, std::size_t frames) const noexcept {
        const auto limit = capacity_ * 2;
        return cursor >= limit - frames ? cursor - (limit - frames) : cursor + frames;
    }

    template <typename CopyFrame>
    std::size_t Produce(std::size_t requested, CopyFrame copy) noexcept {
        const auto write = write_.value.load(std::memory_order_relaxed);
        const auto read = read_.value.load(std::memory_order_acquire);
        const auto count = (std::min)(requested, capacity_ - Distance(write, read));
        if (count < requested)
            overruns_.store(overruns_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        auto slot = write % capacity_;
        for (std::size_t frame = 0; frame < count; ++frame) {
            copy(slot, frame);
            if (++slot == capacity_) slot = 0;
        }
        // Publish copied samples; producer may only reuse slots the consumer released.
        write_.value.store(Advance(write, count), std::memory_order_release);
        return count;
    }

    template <typename CopyFrame>
    std::size_t Consume(std::size_t requested, CopyFrame copy) noexcept {
        const auto read = read_.value.load(std::memory_order_relaxed);
        const auto write = write_.value.load(std::memory_order_acquire);
        const auto count = (std::min)(requested, Distance(write, read));
        if (count < requested)
            underruns_.store(underruns_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        auto slot = read % capacity_;
        for (std::size_t frame = 0; frame < count; ++frame) {
            copy(slot, frame);
            if (++slot == capacity_) slot = 0;
        }
        // Release consumed slots only after all destination samples have been copied.
        read_.value.store(Advance(read, count), std::memory_order_release);
        return count;
    }

    struct alignas(64) Cursor { std::atomic<std::size_t> value{0}; };
    const std::size_t channels_;
    const std::size_t capacity_;
    std::vector<float> samples_;
    Cursor write_;
    Cursor read_;
    alignas(64) std::atomic<std::uint64_t> overruns_{0};
    alignas(64) std::atomic<std::uint64_t> underruns_{0};
};
