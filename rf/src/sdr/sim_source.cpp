// Simulated receiver: a fixed set of signals at real-world frequencies over a
// noise floor, so tuning around shows a believable band with no radio attached.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "ft8.h"
#include "sdr/source.h"

namespace zrf {
namespace {

enum class Kind { FmBroadcast, NarrowFm, AmTone, Bursts, UpperTones, LowerTones };

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
    // Sideband stand-ins: two tones 700 and 1900 Hz above (or below) the dial frequency.
    {14.2e6, 0.10f, Kind::UpperTones},   {7.15e6, 0.10f, Kind::LowerTones},
};

const double kTwoPi = 6.283185307179586;

// FT8 stations, heard on the 20 m FT8 dial frequency (upper sideband). Each
// transmits in every other 15-second slot, as real stations do. The callsigns
// are the made-up ones the FT8 documentation uses.
struct Ft8Station {
    const char* text;
    double audio_hz;   // lowest tone, above the dial frequency
    int parity;        // 0 = slots starting at :00 and :30, 1 = :15 and :45
    float amp;
};
const double kFt8Dial = 14.074e6;
const Ft8Station kFt8Stations[] = {
    {"CQ K1ABC FN42", 1500, 0, 0.02f},
    {"K1ABC W9XYZ EN37", 900, 1, 0.012f},
    {"CQ DX VE3ABC FN03", 2100, 0, 0.006f},
    {"W9XYZ K1ABC -11", 1500, 1, 0.02f},
};

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
    // The simulated crystal is 20 ppm fast until corrected by the same amount,
    // when ZRF_SIM_PPM_ERROR is set; otherwise it is exact.
    bool set_ppm(int ppm) override { ppm_ = ppm; return true; }
    bool set_bias_tee(bool) override { return true; }

    size_t read(cf32* out, size_t n) override {
        const float noise = 0.003f;
        for (size_t i = 0; i < n; ++i) out[i] = cf32(gauss() * noise, gauss() * noise);

        for (size_t s = 0; s < std::size(kSignals); ++s) {
            const Signal& sig = kSignals[s];
            double off = sig.hz - freq_ * (1 + (error_ppm_ - ppm_) * 1e-6);
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
                case Kind::UpperTones:
                case Kind::LowerTones: {
                    // Carrier suppressed: only the two tones are sent.
                    double sign = sig.kind == Kind::UpperTones ? 1 : -1;
                    mod += kTwoPi * 700.0 / rate_;
                    double a = kTwoPi * off * t + sign * mod, b = kTwoPi * off * t + sign * mod * (1900.0 / 700.0);
                    out[i] += cf32(sig.amp * float(std::cos(a) + 0.6 * std::cos(b)), sig.amp * float(std::sin(a) + 0.6 * std::sin(b)));
                    continue;
                }
                case Kind::Bursts:
                    // 120 ms of on-off keying every 1.5 s, like a weather sensor.
                    if (std::fmod(t, 1.5) > 0.12 || std::fmod(t, 0.001) > 0.0005) amp = 0;
                    break;
                }
                ph += kTwoPi * (off + dev) / rate_;
                if (amp != 0) out[i] += cf32(amp * float(std::cos(ph)), amp * float(std::sin(ph)));
            }
            ph = std::fmod(ph, kTwoPi);
            mod = std::fmod(mod, kTwoPi * 7000);   // a whole number of turns for every tone derived from it
        }
        add_ft8(out, n);
        time_ += n / rate_;
        pacer_.wait(n, rate_);
        return n;
    }

private:
    void add_ft8(cf32* out, size_t n) {
        if (std::fabs(kFt8Dial + 1500 - freq_) > rate_ * 0.42) return;
        if (!ft8_ready_) {
            for (size_t k = 0; k < std::size(kFt8Stations); ++k) ft8_ok_[k] = zrf::ft8_tones(kFt8Stations[k].text, ft8_tones_[k]);
            ft8_ready_ = true;
        }
        // Slots follow the real clock, as on the air.
        double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        for (size_t k = 0; k < std::size(kFt8Stations); ++k) {
            if (!ft8_ok_[k]) continue;
            const Ft8Station& st = kFt8Stations[k];
            double base = kFt8Dial + st.audio_hz - freq_;
            for (size_t i = 0; i < n; ++i) {
                double t = now + double(i) / rate_;
                double slot = std::floor(t / kFt8SlotSeconds);
                double into = t - slot * kFt8SlotSeconds - 0.5;   // transmissions start half a second in
                int sym = int(std::floor(into / kFt8SymbolSeconds));
                if (into < 0 || sym >= kFt8Symbols || (int64_t(slot) & 1) != st.parity) continue;
                ft8_phase_[k] += kTwoPi * (base + ft8_tones_[k][sym] * kFt8ToneSpacing) / rate_;
                out[i] += cf32(st.amp * float(std::cos(ft8_phase_[k])), st.amp * float(std::sin(ft8_phase_[k])));
            }
            ft8_phase_[k] = std::fmod(ft8_phase_[k], kTwoPi);
        }
    }

    float uniform() {
        rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;
        return float(rng_ >> 8) * (1.0f / 16777216.0f) - 0.5f;
    }
    // Sum of four uniforms: close enough to a normal curve for a noise floor.
    float gauss() { return (uniform() + uniform() + uniform() + uniform()) * 1.7320508f; }

    double freq_ = 100e6, rate_ = 1.024e6, time_ = 0;
    int ppm_ = 0;
    bool ft8_ready_ = false;
    bool ft8_ok_[std::size(kFt8Stations)] = {};
    uint8_t ft8_tones_[std::size(kFt8Stations)][kFt8Symbols] = {};
    double ft8_phase_[std::size(kFt8Stations)] = {};
    const int error_ppm_ = std::getenv("ZRF_SIM_PPM_ERROR") ? std::atoi(std::getenv("ZRF_SIM_PPM_ERROR")) : 0;
    double phase_[std::size(kSignals)] = {}, mod_phase_[std::size(kSignals)] = {};
    uint32_t rng_ = 0x2545F491u;
    Pacer pacer_;
};

}  // namespace

std::unique_ptr<Source> make_sim_source() { return std::make_unique<SimSource>(); }

}  // namespace zrf
