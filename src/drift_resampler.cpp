#include "drift_resampler.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
constexpr double pi = 3.14159265358979323846;
}

DriftResampler::DriftResampler(std::uint32_t channels, double nominalRatio,
                             std::uint32_t taps)
    : channels_(channels), taps_(taps), nominalRatio_(nominalRatio), ratio_(nominalRatio) {
    if (!channels || taps < 32 || !std::isfinite(nominalRatio) || nominalRatio <= 0 ||
        !std::isfinite(1/(nominalRatio*0.998)) || !std::isfinite(nominalRatio*1.002))
        throw std::invalid_argument("Invalid resampler dimensions or ratio");
    if (channels > history_.max_size()/taps || taps > coefficients_.max_size()/(phases_+1))
        throw std::length_error("Resampler dimensions overflow");
    history_.assign(static_cast<std::size_t>(channels)*taps, 0);
    coefficients_.resize(static_cast<std::size_t>(phases_+1)*taps);

    // Four-term Blackman-Harris window. A conservative transition band keeps
    // stopband rejection across all allowed ratio corrections. No coefficient
    // generation, transcendental functions or heap work in the streaming path.
    const double cutoff = 0.5*std::min(1.0, nominalRatio*0.998)*0.94;
    const double half = DelayInputFrames();
    for (std::uint32_t phase=0; phase<=phases_; ++phase) {
        double sum = 0;
        auto* row = coefficients_.data()+static_cast<std::size_t>(phase)*taps;
        for (std::uint32_t k=0; k<taps; ++k) {
            const double x = k-half+static_cast<double>(phase)/phases_;
            const double w = std::abs(x)>half ? 0 :
                0.35875 + 0.48829*std::cos(pi*x/half) +
                0.14128*std::cos(2*pi*x/half) + 0.01168*std::cos(3*pi*x/half);
            row[k] = w * (std::abs(x)<1e-12 ? 2*cutoff : std::sin(2*pi*cutoff*x)/(pi*x));
            sum += row[k];
        }
        for (std::uint32_t k=0; k<taps; ++k) row[k] /= sum;
    }
}

void DriftResampler::SetRatio(double ratio) noexcept {
    if (!flushing_ && std::isfinite(ratio))
        ratio_ = std::clamp(ratio, nominalRatio_*0.998, nominalRatio_*1.002);
}

void DriftResampler::Push(const float* frame) noexcept {
    auto* destination = history_.data()+static_cast<std::size_t>(write_)*channels_;
    if (frame) std::copy_n(frame, channels_, destination);
    else std::fill_n(destination, channels_, 0.0f);
    if (++write_ == taps_) write_ = 0;
    phase_ -= 1;
}

void DriftResampler::Emit(float* frame) const noexcept {
    // phase_ is next input position minus the number of frames consumed.
    // Keep it bounded instead of accumulating a growing absolute sample clock.
    const double tablePosition = std::clamp(phase_+1, 0.0, 1.0)*phases_;
    const auto rowIndex = std::min(static_cast<std::uint32_t>(tablePosition), phases_-1);
    const double fraction = tablePosition-rowIndex;
    const auto* a = coefficients_.data()+static_cast<std::size_t>(rowIndex)*taps_;
    const auto* b = a+taps_;
    for (std::uint32_t c=0; c<channels_; ++c) {
        double value = 0;
        std::uint32_t slot = write_;
        for (std::uint32_t k=0; k<taps_; ++k) {
            slot = slot ? slot-1 : taps_-1;
            value += history_[static_cast<std::size_t>(slot)*channels_+c] *
                     (a[k]+fraction*(b[k]-a[k]));
        }
        frame[c] = static_cast<float>(value);
    }
}

DriftResampler::Progress DriftResampler::Process(const float* input, std::uint32_t inputFrames,
                                               float* output, std::uint32_t outputCapacity) noexcept {
    Progress progress;
    if (flushing_) return progress;
    while (progress.produced < outputCapacity) {
        if (phase_ >= 0) {
            if (progress.consumed == inputFrames) break;
            Push(input+static_cast<std::size_t>(progress.consumed)*channels_);
            ++progress.consumed;
            hasInput_ = true;
        } else {
            Emit(output+static_cast<std::size_t>(progress.produced)*channels_);
            ++progress.produced;
            phase_ += 1/ratio_;
        }
    }
    return progress;
}

std::uint32_t DriftResampler::Flush(float* output, std::uint32_t capacity) noexcept {
    if (!capacity) return 0;
    flushing_ = true;
    if (!hasInput_) return 0;
    std::uint32_t produced = 0;
    while (produced < capacity && phase_+flushZeros_ < DelayInputFrames()) {
        if (phase_ >= 0) { Push(nullptr); ++flushZeros_; }
        else {
            Emit(output+static_cast<std::size_t>(produced)*channels_);
            ++produced;
            phase_ += 1/ratio_;
        }
    }
    return produced;
}

void DriftResampler::Reset() noexcept {
    std::fill(history_.begin(), history_.end(), 0.0f);
    ratio_ = nominalRatio_;
    phase_ = 0;
    write_ = flushZeros_ = 0;
    hasInput_ = flushing_ = false;
}

FillLevelController::FillLevelController(Direction direction, double targetFrames, double kp,
                                         double ki, double maxDeviation,double measurementTimeConstantSeconds)
    : target_(targetFrames), kp_(kp), ki_(ki), limit_(maxDeviation),
      measurementTimeConstant_(measurementTimeConstantSeconds),measuredFill_(targetFrames) {
    if ((direction != Direction::Capture && direction != Direction::Render) ||
        !std::isfinite(targetFrames) || targetFrames <= 0 || !std::isfinite(kp) || kp < 0 ||
        !std::isfinite(ki) || ki < 0 || !std::isfinite(maxDeviation) ||
        maxDeviation <= 0 || maxDeviation > 0.002 ||
        !std::isfinite(measurementTimeConstantSeconds) || measurementTimeConstantSeconds<0)
        throw std::invalid_argument("Invalid fill controller configuration");
}

FillLevelController FillLevelController::ForPacketCadence(Direction direction,double targetFrames,
    double referenceKp,double referenceKi,double graphRate,std::uint32_t maxQuantum) {
    if(!std::isfinite(graphRate) || graphRate<8000 || graphRate>384000 || maxQuantum<16 || maxQuantum>8192)
        throw std::invalid_argument("Invalid fill controller packet cadence");
    // The averaged plant has gain approximately graphRate. Its filtered PI
    // characteristic is tau*p^3+p^2+a*kp*p+a*ki. Scaling kp by speed, ki by
    // speed^2 and tau by 1/speed preserves normalized poles and damping;
    // rateScale removes a's rate dependence. With default gains kp/(tau*ki)=3,
    // retaining the continuous-time Routh stability margin at every quantum.
    const double speed=0.5*(std::min)(1.0,(2048.0/48000)/(maxQuantum/graphRate));
    const double rateScale=48000/graphRate;
    return FillLevelController(direction,targetFrames,referenceKp*rateScale*speed,
        referenceKi*rateScale*speed*speed,0.002,1/speed);
}

double FillLevelController::Update(double fillFrames, double elapsedSeconds) noexcept {
    if (!std::isfinite(fillFrames) || fillFrames < 0 || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0)
        return correction_;
    if(measurementTimeConstant_>0) {
        // Exact one-pole coefficient for irregular native event intervals.
        // expm1 preserves precision for short successive bounded worker passes.
        const double weight=-std::expm1(-elapsedSeconds/measurementTimeConstant_);
        measuredFill_+=weight*(fillFrames-measuredFill_);
        fillFrames=measuredFill_;
    }
    const double error = target_-fillFrames;
    const double proportional = kp_*error;
    const double increment = ki_*error*elapsedSeconds;
    const double candidate = std::clamp(integral_+increment, -limit_, limit_);
    const double total = proportional+candidate;
    // Conditional integration prevents saturation windup; permit movement back
    // toward the controllable interval even while the proportional term clips.
    if ((total >= -limit_ && total <= limit_) ||
        (total > limit_ && error < 0) || (total < -limit_ && error > 0))
        integral_ = candidate;
    correction_ = 1+std::clamp(proportional+integral_, -limit_, limit_);
    return correction_;
}
