#pragma once

#include <cstdint>
#include <vector>

// Single worker ownership: construct before publication; Process, SetRatio,
// Flush and Reset must never run concurrently. All streaming methods allocate
// nothing. Buffers are interleaved, have channels * frames samples and must not
// overlap. The caller retains and retries input beyond Progress::consumed.
class DriftResampler final {
public:
    struct Progress { std::uint32_t consumed = 0, produced = 0; };
    explicit DriftResampler(std::uint32_t channels, double nominalRatio,
                            std::uint32_t taps = 256);

    // Ratio is output frames / input frames; finite commands clamp to nominal
    // +/-0.2%. Nonfinite commands and commands after draining begins are ignored.
    // The filter covers that whole band.
    void SetRatio(double ratio) noexcept;
    double Ratio() const noexcept { return ratio_; }
    double DelayInputFrames() const noexcept { return (taps_ - 1) * 0.5; }

    // Causal FIR, initially zero padded. Output frame j at constant ratio r
    // represents input time j/r - DelayInputFrames(). Zero output capacity
    // consumes nothing; zero input can still emit pending interpolated frames.
    Progress Process(const float* interleavedIn, std::uint32_t inputFrames,
                     float* interleavedOut, std::uint32_t outputCapacity) noexcept;

    // End of stream: zero extend through input duration + declared delay.
    // At constant r, nonempty N-frame input yields ceil((N+delay)*r) total
    // output frames (within roundoff of an exact integer). This includes the
    // startup delay, and ends at the original signal duration after compensation.
    // Retry until zero. Process is disabled after first nonzero-capacity Flush
    // until Reset. Empty streams yield zero. The ratio freezes when draining
    // begins; subsequent SetRatio calls are ignored until Reset.
    std::uint32_t Flush(float* interleavedOut, std::uint32_t outputCapacity) noexcept;
    void Reset() noexcept;

private:
    static constexpr std::uint32_t phases_ = 2048;
    void Push(const float* frame) noexcept;
    void Emit(float* frame) const noexcept;
    const std::uint32_t channels_, taps_;
    const double nominalRatio_;
    double ratio_, phase_ = 0;
    std::uint32_t write_ = 0, flushZeros_ = 0;
    bool hasInput_ = false, flushing_ = false;
    std::vector<float> history_;
    std::vector<double> coefficients_;
};

// Occupancy PI for the graph-rate FIFO: capture queues SRC output; render
// queues SRC input. Ratio always means output/input. Both need NEGATIVE fill
// feedback: capture dF/dt = deviceRate*r - graphRate; render dF/dt =
// graphRate - deviceRate/r. For the same +device ppm, equilibrium capture r
// decreases and render r increases. Direction names the FIFO convention,
// not an artificial inversion of the stable feedback sign.
// Update returns a multiplier for the nominal ratio, never the absolute ratio.
// Gains use frames: kp [1/frame], ki [1/(frame*second)]. Single worker ownership.
class FillLevelController final {
public:
    enum class Direction { Capture, Render };
    FillLevelController(Direction direction, double targetFrames, double kp,
                        double ki, double maxDeviation = 0.002,
                        double measurementTimeConstantSeconds = 0);
    // Gains are the reference 48 kHz gains. Normalize the loop's frame-rate
    // sensitivity, and slow large callback quanta without changing damping.
    // At <=2048/48000 seconds per callback, occupancy is smoothed over 2 s;
    // the reference loop is slowed 2x to reject 44.1/48 kHz packet beats.
    // Larger quanta scale all loop times together; the FIFO must retain its
    // proportional capacity/quantum margin. Call only during preparation.
    static FillLevelController ForPacketCadence(Direction direction,double targetFrames,
        double referenceKp,double referenceKi,double graphRate,std::uint32_t maxQuantum);
    double Update(double fillFrames, double elapsedSeconds) noexcept;

private:
    const double target_, kp_, ki_, limit_, measurementTimeConstant_;
    double measuredFill_;
    double integral_ = 0, correction_ = 1;
};
