// Simulated receiver: a fixed set of signals at real-world frequencies over a
// noise floor, so tuning around shows a believable band with no radio attached.
#include <cmath>
#include <cstdint>
#include <vector>

#include "sdr/source.h"

namespace zrf {
namespace {

enum class Kind { FmBroadcast, NarrowFm, AmTone, Bursts };

struct Signal {
    double hz;
    float amp;      // linear, 1.0 = full scale
    Kind kind;
};

const Signal kSignals[] = {
    {88.1e6, 0.20f, Kind::FmBroadcast},  {94.5e6, 0.35f, Kind::FmBroadcast},
    {98.3e6, 0.50f, Kind::FmBroadcast},  {98.7e6, 0.08f, Kind::FmBroadcast},
    {101.1e6, 0.25f, Kind::FmBroadcast}, {104.7e6, 0.15f, Kind::FmBroadcast},
    {121.5e6, 0.05f, Kind::AmTone},      {145.5e6, 0.10f, Kind::NarrowFm},
    {162.55e6, 0.12f, Kind::NarrowFm},   {433.92e6, 0.30f, Kind::Bursts},
};

const double kTwoPi = 6.283185307179586;

class SimSource : public Source {
public:
    bool open(std::string&) override {
        pacer_.reset();
        return true;
    }
    bool set_freq(double hz) override { freq_ = hz; return true; }
    bool set_rate(double hz) override {
        if (hz < 1e4 || hz > 3.2e6) return false;
        rate_ = hz;
        return true;
    }
    bool set_gain(std::optional<double>) override { return true; }

    size_t read(cf32* out, size_t n) override {
        const float noise = 0.003f;
        for (size_t i = 0; i < n; ++i) out[i] = cf32(gauss() * noise, gauss() * noise);

        for (size_t s = 0; s < std::size(kSignals); ++s) {
            const Signal& sig = kSignals[s];
            double off = sig.hz - freq_;
            if (std::fabs(off) > rate_ * 0.42) continue;   // outside the receiver's filter
            double& ph = phase_[s];
            double& mod = mod_phase_[s];
            double t = time_;
            for (size_t i = 0; i < n; ++i, t += 1.0 / rate_) {
                double dev = 0;
                float amp = sig.amp;
                switch (sig.kind) {
                case Kind::FmBroadcast:
                    mod += kTwoPi * 1000.0 / rate_;
                    dev = 55e3 * std::sin(mod) + 15e3 * std::sin(mod * 7.3);
                    break;
                case Kind::NarrowFm:
                    mod += kTwoPi * 700.0 / rate_;
                    dev = 3e3 * std::sin(mod);
                    break;
                case Kind::AmTone:
                    mod += kTwoPi * 1000.0 / rate_;
                    amp *= 1.0f + 0.6f * float(std::sin(mod));
                    break;
                case Kind::Bursts:
                    // 120 ms of on-off keying every 1.5 s, like a weather sensor.
                    if (std::fmod(t, 1.5) > 0.12 || std::fmod(t, 0.001) > 0.0005) amp = 0;
                    break;
                }
                ph += kTwoPi * (off + dev) / rate_;
                if (amp != 0) out[i] += cf32(amp * float(std::cos(ph)), amp * float(std::sin(ph)));
            }
            ph = std::fmod(ph, kTwoPi);
            mod = std::fmod(mod, kTwoPi * 1000);
        }
        time_ += n / rate_;
        pacer_.wait(n, rate_);
        return n;
    }

private:
    float uniform() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return float(rng_ >> 8) * (1.0f / 16777216.0f) - 0.5f;
    }
    // Sum of four uniforms: close enough to a normal curve for a noise floor.
    float gauss() { return (uniform() + uniform() + uniform() + uniform()) * 1.7320508f; }

    double freq_ = 100e6, rate_ = 1.024e6, time_ = 0;
    double phase_[std::size(kSignals)] = {}, mod_phase_[std::size(kSignals)] = {};
    uint32_t rng_ = 0x2545F491u;
    Pacer pacer_;
};

}  // namespace

std::unique_ptr<Source> make_sim_source() { return std::make_unique<SimSource>(); }

}  // namespace zrf
