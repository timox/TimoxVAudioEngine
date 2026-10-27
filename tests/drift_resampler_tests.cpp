#include "drift_resampler.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
thread_local bool trackAllocations = false;
thread_local unsigned allocations = 0;
}
void* operator new(std::size_t size) {
    if (trackAllocations) ++allocations;
    if (void* memory=std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

namespace {
constexpr double pi = 3.14159265358979323846;
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

std::vector<float> Tone(unsigned frames, double rate, double frequency) {
    std::vector<float> in(frames * 2);
    for (unsigned n = 0; n < frames; ++n) {
        in[2*n] = static_cast<float>(0.7 * std::sin(2*pi*frequency*n/rate));
        in[2*n+1] = static_cast<float>(0.3 * std::cos(2*pi*frequency*n/rate));
    }
    return in;
}

// This catches lost unconsumed input, lost queued output, channel leakage,
// history resets at packet boundaries and a non-idempotent drain.
std::vector<float> Convert(const std::vector<float>& in, double nominal, bool irregular, double correction = 1) {
    DriftResampler src(2, nominal);
    src.SetRatio(nominal*correction);
    const double ratio = nominal*correction;
    std::vector<float> out;
    float block[514];
    const unsigned packets[] = {1, 127, 3, 256, 17, 61};
    unsigned offset = 0, iteration = 0;
    while (offset < in.size()/2) {
        unsigned count = std::min<unsigned>(static_cast<unsigned>(in.size()/2)-offset,
                                            irregular ? packets[iteration%6] : 257);
        unsigned capacity = irregular ? packets[(iteration+2)%6] : 257;
        auto p = src.Process(in.data()+offset*2, count, block, capacity);
        Require(p.consumed <= count && p.produced <= capacity, "progress exceeds buffers");
        Require(p.consumed || p.produced, "SRC stalls");
        offset += p.consumed;
        out.insert(out.end(), block, block+p.produced*2);
        ++iteration;
    }
    for (;;) {
        auto produced = src.Flush(block, irregular ? 7 : 257);
        if (!produced) break;
        out.insert(out.end(), block, block+produced*2);
    }
    Require(src.Flush(block, 257) == 0, "flush not idempotent");
    const double expected = (in.size()/2 + src.DelayInputFrames()) * ratio;
    Require(std::abs(out.size()/2 - expected) <= 1.00001, "drained duration wrong");
    return out;
}

void Quality() {
    for (double ratio : {1.0, 48000.0/44100.0, 44100.0/48000.0}) {
        double rate = ratio > 1 ? 44100 : 48000;
        DriftResampler metadata(2, ratio);
        for (double hz : {1000.0, 10000.0}) {
            auto input = Tone(24000, rate, hz);
            auto output = Convert(input, ratio, false);
            auto partitioned = Convert(input, ratio, true);
            Require(output == partitioned, "packet partition changes samples");
            double signal = 0, error = 0;
            for (unsigned j = 1024; j + 1024 < output.size()/2; ++j) {
                // Only the declared causal delay is compensated. No fitted phase,
                // correlation alignment, amplitude scaling, or fitted reference.
                const double time = (j/ratio - metadata.DelayInputFrames()) / rate;
                const double expected[] = {0.7*std::sin(2*pi*hz*time), 0.3*std::cos(2*pi*hz*time)};
                for (unsigned c = 0; c != 2; ++c) {
                    double e = output[2*j+c] - expected[c];
                    signal += expected[c]*expected[c]; error += e*e;
                }
            }
            double snr = 10*std::log10(signal/error);
            std::cout << "ratio=" << ratio << " hz=" << hz << " SNR=" << snr << '\n';
            Require(snr > 90, "SNR below 90 dB");
        }
    }
    for (double correction : {0.998, 1.0, 1.002}) {
        for (double frequency : {22200.0, 22500.0, 23000.0, 23700.0}) {
            auto stopband = Convert(Tone(24000, 48000, frequency), 44100.0/48000.0, true, correction);
            double energy = 0; unsigned count = 0;
            for (unsigned j=1024; j+1024<stopband.size()/2; ++j) {
                energy += stopband[2*j]*stopband[2*j]; ++count;
            }
            double rejection = -10*std::log10((energy/count)/(0.7*0.7/2));
            std::cout << "stopband Hz=" << frequency << " correction=" << correction << " rejection=" << rejection << '\n';
            Require(rejection > 80, "alias rejection below 80 dB");
        }
    }
}

void Boundaries() {
    for (unsigned channels : {0U}) {
        bool rejected=false;
        try { DriftResampler invalid(channels, 1); } catch (const std::invalid_argument&) { rejected=true; }
        Require(rejected, "zero channels accepted");
    }
    for (unsigned taps : {0U, 1U}) {
        bool rejected=false;
        try { DriftResampler invalid(2, 1, taps); } catch (const std::invalid_argument&) { rejected=true; }
        Require(rejected, "invalid filter length accepted");
    }
    for (double ratio : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected=false;
        try { DriftResampler invalid(2, ratio); } catch (const std::invalid_argument&) { rejected=true; }
        Require(rejected, "invalid nominal ratio accepted");
    }
    DriftResampler src(2, 48000.0/44100.0);
    const double nominal = src.Ratio();
    src.SetRatio(nominal*2);
    Require(std::abs(src.Ratio()/nominal-1.002)<1e-12, "upper ratio bound");
    src.SetRatio(0);
    Require(std::abs(src.Ratio()/nominal-0.998)<1e-12, "lower ratio bound");
    src.SetRatio(std::numeric_limits<double>::quiet_NaN());
    Require(std::isfinite(src.Ratio()), "NaN ratio accepted");
    float out[2] = {9, 9};
    const float in[2] = {1, 2};
    auto p = src.Process(in, 1, out, 0);
    Require(p.consumed == 0 && p.produced == 0 && out[0] == 9, "zero capacity consumed input");
    src.Process(in, 1, out, 1);
    src.Reset();
    Require(src.Ratio()==nominal, "reset does not restore nominal ratio");
    DriftResampler fresh(2, nominal);
    float reference[2];
    p = src.Process(in, 1, out, 1);
    auto q = fresh.Process(in, 1, reference, 1);
    Require(p.consumed==q.consumed && p.produced==q.produced && out[0]==reference[0] && out[1]==reference[1], "reset retains signal state");
    DriftResampler empty(2, 1);
    Require(empty.Flush(out, 1)==0, "empty drain generates audio");
    trackAllocations=true;
    src.SetRatio(nominal);
    src.Process(in, 1, out, 1);
    while (src.Flush(out, 1)) {}
    src.Reset();
    trackAllocations=false;
    Require(allocations==0, "streaming operation allocated");
}

// Two independent event clocks, not the fluid/continuous FIFO approximation.
// A graph callback moves a whole quantum; each native wake moves one packet.
// Observe steady-state mean error AND modulation: a bounded FIFO alone does
// not prove the SRC ratio is estimating clock drift rather than packet phase.
void PacketizedClockDrift() {
    double worstRms=0,worstMeanError=0,smallestMargin=std::numeric_limits<double>::infinity();
    for(auto direction : {FillLevelController::Direction::Capture,FillLevelController::Direction::Render})
    for(unsigned quantum : {512U,2048U,8192U})
    for(unsigned graphRate : {44100U,48000U})
    for(double nativeRate : {44100.0,48000.0})
    for(unsigned packet : {480U,960U})for(double phase : {0.001,0.013,0.037})
    for(double ppm : {-1000.0,-100.0,0.0,100.0,1000.0}) {
        const double target=4.0*quantum,capacity=8.0*quantum;
        auto controller=FillLevelController::ForPacketCadence(direction,target,3e-6,1e-6,graphRate,quantum);
        const bool capture=direction==FillLevelController::Direction::Capture;
        // Opposite nominal rates exercise both up/down SRC. The FIFO remains
        // graph-rate, so native packet sizes must be converted to graph frames.
        const double graphPeriod=static_cast<double>(quantum)/graphRate;
        const double speed=0.5*(std::min)(1.0,(2048.0/48000)/graphPeriod);
        const double settleSeconds=120/speed;
        double graphIdeal=std::fmod(phase,graphPeriod),graphTime=graphIdeal;
        double nativeIdeal=packet/nativeRate,nativeTime=nativeIdeal,previousNative=0;
        double fill=target,ratio=1;
        unsigned nativeTick=0,graphTick=0;
        double sum=0,sumSquares=0;unsigned count=0,clipped=0;
        // Observe at least ten steady-state minutes, matching the hardware
        // qualification horizon rather than one favorable packet beat phase.
        while((std::min)(graphTime,nativeTime)<7*settleSeconds) {
            if(graphTime<nativeTime) {
                fill+=capture?-static_cast<double>(quantum):quantum;
                graphIdeal+=graphPeriod;++graphTick;
                graphTime=graphIdeal+(graphTick%4==1?0.002:0);
            } else {
                const double graphPacket=packet*graphRate/nativeRate;
                const double movement=capture?graphPacket*ratio:-graphPacket/ratio;
                const double before=fill;
                fill+=movement;
                ratio=controller.Update((before+fill)*0.5,nativeTime-previousNative);
                previousNative=nativeTime;
                if(nativeTime>=2*settleSeconds) {
                    const auto correction=(ratio-1)*1e6;
                    sum+=correction;sumSquares+=correction*correction;++count;
                    if(std::abs(correction)>=1999.9)++clipped;
                }
                // At one settling interval, step the physical mismatch from
                // -ppm to +ppm. Bounded scheduling stalls occasionally bunch
                // successive packets; the data clock does not stop with them.
                const double drift=nativeIdeal<settleSeconds?-ppm:ppm;
                nativeIdeal+=packet/(nativeRate*(1+drift*1e-6));++nativeTick;
                const double delay=nativeTick%307==0?0.012:nativeTick%4==1?0.001:0;
                nativeTime=(std::max)(nativeTime+1e-9,nativeIdeal+delay);
            }
            if(!(fill>0 && fill<capacity))std::cerr<<"FIFO case capture="<<capture<<" q="<<quantum<<" graph="<<graphRate
                <<" packet="<<packet<<" phase="<<phase<<" ppm="<<ppm<<" fill="<<fill<<" time="<<nativeTime<<'\n';
            Require(fill>0 && fill<capacity,"packetized drift starves or overflows FIFO during transient");
            smallestMargin=(std::min)(smallestMargin,(std::min)(fill,capacity-fill));
        }
        const double expected=(capture?1/(1+ppm*1e-6):1+ppm*1e-6);
        const double mean=sum/count;
        const double rms=std::sqrt((std::max)(0.0,sumSquares/count-mean*mean));
        worstRms=(std::max)(worstRms,rms);
        worstMeanError=(std::max)(worstMeanError,std::abs(mean-(expected-1)*1e6));
        if(std::abs(mean-(expected-1)*1e6)>=5 || rms>=100 || clipped)
            std::cerr<<"PI case capture="<<capture<<" q="<<quantum<<" graph="<<graphRate<<" packet="<<packet
                <<" phase="<<phase<<" ppm="<<ppm<<" mean="<<mean<<" rms="<<rms<<" clipped="<<clipped<<'\n';
        Require(std::abs(mean-(expected-1)*1e6)<5,"packetized PI mean correction misses physical drift");
        // Ratio ripple must be below the smallest injected clock mismatch, and
        // normal packet cadence must not spend steady state at the safety clamp.
        Require(rms<100,"packet cadence modulates SRC more than 100 ppm RMS");
        Require(clipped==0,"normal packet cadence saturates SRC correction");
    }
    std::cout<<"Packetized PI worst RMS "<<worstRms<<" ppm; mean error "<<worstMeanError
        <<" ppm; minimum FIFO boundary margin "<<smallestMargin<<" frames\n";
}

void ClockDrift() {
    for(double invalid : {-1.0,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        bool rejected=false;
        try {FillLevelController controller(FillLevelController::Direction::Capture,2048,3e-6,1e-6,0.002,invalid);}
        catch(const std::invalid_argument&){rejected=true;}
        Require(rejected,"invalid occupancy filter time accepted");
    }
    for (double target : {0.0, -1.0, std::numeric_limits<double>::infinity()}) {
        bool rejected=false;
        try { FillLevelController invalid(FillLevelController::Direction::Capture, target, 0.000003, 0.000001); }
        catch (const std::invalid_argument&) { rejected=true; }
        Require(rejected, "invalid PI target accepted");
    }
    for (auto direction : {FillLevelController::Direction::Capture, FillLevelController::Direction::Render}) {
        for (double ppm : {-1000.0, -100.0, 100.0, 1000.0}) {
            FillLevelController piController(direction, 2048, 0.000003, 0.000001);
            double fill = 2048, correction = 1;
            // Capture FIFO is graph-rate output; render FIFO is graph-rate input.
            // Device clock +ppm therefore fills capture faster, drains render faster.
            for (unsigned tick=0; tick<12000; ++tick) {
                double dt = (tick%3 == 0 ? 0.007 : tick%3 == 1 ? 0.013 : 0.01);
                correction = piController.Update(fill, dt);
                Require(correction >= 0.998 && correction <= 1.002, "PI unbounded");
                double device = 48000*(1+ppm*1e-6);
                fill += dt * (direction == FillLevelController::Direction::Capture
                    ? device*correction-48000 : 48000-device/correction);
                Require(fill>0 && fill<4096, "clock drift empties or overflows FIFO");
            }
            Require(std::abs(fill-2048)<1, "PI fails to converge to target");
            const double expected = direction == FillLevelController::Direction::Capture
                ? 1/(1+ppm*1e-6) : 1+ppm*1e-6;
            Require(std::abs(correction-expected)<2e-6, "PI wrong clock correction");
        }
        FillLevelController saturated(direction, 2048, 0.000003, 0.000001);
        for (unsigned n=0; n<10000; ++n) saturated.Update(100000, 0.01);
        double recovered = saturated.Update(2048, 0.01);
        Require(std::abs(recovered-1)<1e-9, "integrator winds up during saturation");
        Require(std::isfinite(saturated.Update(std::numeric_limits<double>::quiet_NaN(), 1)), "PI accepts NaN");
    }
}

// A streaming ratio change must change the time step without resetting phase or
// history. The independent reference integrates the commanded output time steps.
void VariableRatio() {
    for (double nominal : {48000.0/44100.0, 44100.0/48000.0}) {
        DriftResampler src(2, nominal);
        auto input = Tone(30000, 48000, 10000);
        unsigned consumed=0, produced=0;
        double time=0, error=0, signal=0;
        while (consumed < input.size()/2) {
            const double ppm[] = {-1000, 100, 1000, -100};
            double ratio=nominal*(1+ppm[(produced/173)%4]*1e-6);
            src.SetRatio(ratio);
            float output[2];
            auto p=src.Process(input.data()+consumed*2,
                               std::min<unsigned>(37, static_cast<unsigned>(input.size()/2)-consumed), output, 1);
            consumed+=p.consumed;
            if (p.produced) {
                if (produced > 1024) {
                    const double expected=0.7*std::sin(2*pi*10000*(time-src.DelayInputFrames())/48000);
                    error+=(output[0]-expected)*(output[0]-expected);
                    signal+=expected*expected;
                }
                time+=1/ratio; ++produced;
            }
            Require(p.consumed || p.produced, "variable ratio stalls");
        }
        Require(10*std::log10(signal/error)>90, "ratio changes lose phase/history");
    }
}

// Catches losing phase/tail when a changing-ratio stream starts draining, and
// changing duration halfway through a drain. Count is derived from the summed
// input timeline, not a constant-ratio approximation of the preceding stream.
std::vector<float> VariableDrain(double nominal, bool fragmented) {
    DriftResampler src(2, nominal);
    auto input = Tone(12000, 48000, 10000);
    std::vector<float> output;
    unsigned consumed=0, produced=0;
    double inputTime=0;
    while (consumed < input.size()/2) {
        const double corrections[] = {0.998, 1.001, 1.002, 0.999};
        const double ratio=nominal*corrections[(produced/137)%4];
        src.SetRatio(ratio);
        float frame[2];
        const auto p=src.Process(input.data()+consumed*2,
                                std::min<unsigned>(53, static_cast<unsigned>(input.size()/2)-consumed), frame, 1);
        consumed+=p.consumed;
        if (p.produced) {
            output.insert(output.end(), frame, frame+2);
            inputTime+=1/ratio;
            ++produced;
        }
        Require(p.consumed || p.produced, "variable drain input stalls");
    }
    const double finalRatio=nominal*1.0015;
    src.SetRatio(finalRatio);
    const double endTime=input.size()/2+src.DelayInputFrames();
    const unsigned expectedTail=static_cast<unsigned>(std::ceil((endTime-inputTime)*finalRatio));
    unsigned drained=0;
    float block[2048];
    for (;;) {
        const auto n=src.Flush(block, fragmented ? 3 : 1024);
        if (!n) break;
        drained+=n;
        output.insert(output.end(), block, block+n*2);
        if (fragmented) {
            src.SetRatio(nominal*0.998);
            Require(src.Ratio()==finalRatio, "ratio changes during flush");
            const float ignored[2]={1,1};
            const auto p=src.Process(ignored, 1, block, 1);
            Require(p.consumed==0 && p.produced==0, "flush accepts additional input");
        }
    }
    Require(drained==expectedTail, "variable ratio flush duration wrong");
    Require(src.Flush(block, 1024)==0, "variable drain not idempotent");
    return output;
}

void VariableRatioFlush() {
    for (double nominal : {1.0, 48000.0/44100.0, 44100.0/48000.0}) {
        const auto oneShot=VariableDrain(nominal, false);
        const auto fragmented=VariableDrain(nominal, true);
        Require(oneShot==fragmented, "variable-ratio tail changes with flush partition");
    }
}
}

int main() {
    try { PacketizedClockDrift(); Quality(); Boundaries(); ClockDrift(); VariableRatio(); VariableRatioFlush(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "drift_resampler_tests passed\n";
}
