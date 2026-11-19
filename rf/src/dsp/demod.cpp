#include "dsp/demod.h"

#include <algorithm>
#include <cmath>

namespace zrf {

bool parse_mode(const std::string& s, Mode& out) {
    if (s == "off") out = Mode::Off;
    else if (s == "wfm") out = Mode::Wfm;
    else if (s == "nfm") out = Mode::Nfm;
    else if (s == "am") out = Mode::Am;
    else if (s == "usb") out = Mode::Usb;
    else if (s == "lsb") out = Mode::Lsb;
    else if (s == "cw") out = Mode::Cw;
    else return false;
    return true;
}

const char* mode_name(Mode m) {
    switch (m) {
    case Mode::Wfm: return "wfm";
    case Mode::Nfm: return "nfm";
    case Mode::Am: return "am";
    case Mode::Usb: return "usb";
    case Mode::Lsb: return "lsb";
    case Mode::Cw: return "cw";
    default: return "off";
    }
}

template <typename T>
FirDecim<T>::FirDecim(int decim, double cutoff, int ntaps) : decim_(decim), taps_(size_t(ntaps)), hist_(size_t(ntaps - 1)) {
    // Windowed sinc (Hamming), unity gain at DC.
    double sum = 0, mid = (ntaps - 1) / 2.0;
    for (int i = 0; i < ntaps; ++i) {
        double x = i - mid;
        double sinc = x == 0 ? 2 * cutoff : std::sin(2 * M_PI * cutoff * x) / (M_PI * x);
        double w = ntaps > 1 ? 0.54 - 0.46 * std::cos(2 * M_PI * i / (ntaps - 1)) : 1.0;
        taps_[size_t(i)] = float(sinc * w);
        sum += sinc * w;
    }
    for (float& t : taps_) t = float(t / sum);
}

template <typename T>
void FirDecim<T>::run(const T* in, size_t n, std::vector<T>& out) {
    size_t nt = taps_.size(), keep = nt - 1;
    hist_.insert(hist_.end(), in, in + n);
    size_t i = size_t(phase_);
    for (; i + nt <= hist_.size(); i += size_t(decim_)) {
        T acc{};
        const T* p = &hist_[i];
        for (size_t k = 0; k < nt; ++k) acc += p[k] * taps_[k];
        out.push_back(acc);
    }
    // Keep the tail the next block's first outputs need.
    size_t consumed = hist_.size() - keep;
    phase_ = int(i - consumed);
    hist_.erase(hist_.begin(), hist_.begin() + long(consumed));
}

template class FirDecim<cf32>;
template class FirDecim<float>;

namespace {

const double kAudioTarget = 32000;
// A sideband is taken as 200-3000 Hz from the carrier: centre 1600, half-width 1400.
const double kSsbCentre = 1600, kSsbHalfWidth = 1400;
// Morse: keep 250 Hz either side of the tuned frequency and play it at this pitch.
const double kCwPitch = 700, kCwHalfWidth = 250;

bool is_sideband(Mode m) { return m == Mode::Usb || m == Mode::Lsb || m == Mode::Cw; }

struct Plan {
    int chan_decim, chan2_decim, audio_decim;
    double chan_cutoff_hz, chan_rate;   // chan_rate: after both channel steps
};

Plan make_plan(Mode mode, double rate) {
    int total = std::max(1, int(std::lround(rate / kAudioTarget)));
    Plan p{};
    if (mode == Mode::Wfm) {
        // Discriminate at about 8x the audio rate so the +-75 kHz swing fits.
        p.audio_decim = std::min(total, 8);
        p.chan_decim = std::max(1, total / p.audio_decim);
        p.chan2_decim = 1;
        p.chan_cutoff_hz = 100e3;
    } else {
        p.audio_decim = 1;
        // Leave the last factor of 4 to the sharp second step where possible.
        p.chan2_decim = total % 4 == 0 && total >= 8 ? 4 : 1;
        p.chan_decim = total / p.chan2_decim;
        p.chan_cutoff_hz = mode == Mode::Am ? 5e3 : 6e3;
    }
    p.chan_rate = rate / (p.chan_decim * p.chan2_decim);
    return p;
}

// First step. With a second step to follow it only has to protect the part
// of the spectrum that step keeps: pass a quarter of its output rate.
FirDecim<cf32> channel_filter(const Plan& p, double rate) {
    if (p.chan_decim == 1) return FirDecim<cf32>(1, 0.25, 1);
    double cutoff = p.chan2_decim > 1 ? 0.25 / p.chan_decim : std::min(p.chan_cutoff_hz / rate, 0.45 / p.chan_decim);
    return FirDecim<cf32>(p.chan_decim, cutoff, std::min(8 * p.chan_decim + 1, 257));
}

// Second step: the real channel filter, about 4 kHz from pass to stop.
FirDecim<cf32> channel2_filter(const Plan& p, double rate) {
    if (p.chan2_decim == 1) return FirDecim<cf32>(1, 0.25, 1);
    return FirDecim<cf32>(p.chan2_decim, p.chan_cutoff_hz / (rate / p.chan_decim), 97);
}

}  // namespace

Demod::Demod(Mode mode, double rate)
    : mode_(mode),
      audio_rate_(make_plan(mode, rate).chan_rate / make_plan(mode, rate).audio_decim),
      channel_(channel_filter(make_plan(mode, rate), rate)),
      channel2_(channel2_filter(make_plan(mode, rate), rate)),
      audio_(make_plan(mode, rate).audio_decim, 15e3 / make_plan(mode, rate).chan_rate,
             make_plan(mode, rate).audio_decim == 1 ? 1 : 6 * make_plan(mode, rate).audio_decim + 1),
      sideband_(1, (mode == Mode::Cw ? kCwHalfWidth : kSsbHalfWidth) / audio_rate_, is_sideband(mode) ? 257 : 1) {
    Plan p = make_plan(mode, rate);
    // Full deviation (75 kHz broadcast, 5 kHz narrow) maps to full scale.
    double deviation = mode == Mode::Wfm ? 75e3 : 5e3;
    fm_gain_ = float(p.chan_rate / (2 * M_PI * deviation));
    // 75 microsecond de-emphasis (Americas, Korea). Europe uses 50; to be a setting.
    deemph_a_ = float(1 - std::exp(-1.0 / (audio_rate_ * 75e-6)));
    if (is_sideband(mode)) {
        // Shift the wanted slice to the middle by `a` a sample, and back out
        // to its audio pitch by `b`.
        double sign = mode == Mode::Lsb ? -1 : 1;
        double a = mode == Mode::Cw ? 0 : -sign * 2 * M_PI * kSsbCentre / audio_rate_;
        double b = mode == Mode::Cw ? 2 * M_PI * kCwPitch / audio_rate_ : -a;
        pre_step_ = cf32(float(std::cos(a)), float(std::sin(a)));
        post_step_ = cf32(float(std::cos(b)), float(std::sin(b)));
    }
}

void Demod::run_channel(const cf32* in, size_t n) {
    stage_buf_.clear();
    channel_.run(in, n, stage_buf_);
    channel2_.run(stage_buf_.data(), stage_buf_.size(), chan_buf_);
}

void Demod::run(const cf32* in, size_t n, std::vector<int16_t>& out) {
    chan_buf_.clear();
    audio_buf_.clear();
    if (is_sideband(mode_)) {
        run_channel(in, n);
        mix_buf_.clear();
        for (const cf32& s : chan_buf_) {
            mix_buf_.push_back(s * pre_);
            pre_ *= pre_step_;
        }
        side_buf_.clear();
        sideband_.run(mix_buf_.data(), mix_buf_.size(), side_buf_);
        for (const cf32& s : side_buf_) {
            float v = (s * post_).real();
            post_ *= post_step_;
            // Level the volume: quick to turn down, slow to turn up.
            float mag = std::fabs(v);
            ssb_level_ += (mag - ssb_level_) * (mag > ssb_level_ ? 0.05f : 0.0005f);
            audio_buf_.push_back(ssb_level_ > 1e-7f ? 0.35f * v / ssb_level_ : 0);
        }
        // The rotating phases drift off unit length; pull them back now and then.
        if (++renorm_ % 16 == 0) {
            pre_ /= std::abs(pre_);
            post_ /= std::abs(post_);
        }
        for (float v : audio_buf_) out.push_back(int16_t(std::clamp(v, -1.0f, 1.0f) * 32000));
        return;
    }
    run_channel(in, n);

    if (mode_ == Mode::Am) {
        for (const cf32& s : chan_buf_) {
            float mag = std::abs(s);
            // The slow average is the carrier: remove it, and use it to level the volume.
            am_level_ += (mag - am_level_) * 0.001f;
            audio_buf_.push_back(am_level_ > 1e-6f ? (mag - am_level_) / am_level_ : 0);
        }
    } else {
        disc_buf_.clear();
        for (const cf32& s : chan_buf_) {
            disc_buf_.push_back(std::arg(s * std::conj(prev_)) * fm_gain_);
            prev_ = s;
        }
        if (mode_ == Mode::Wfm) {
            audio_.run(disc_buf_.data(), disc_buf_.size(), audio_buf_);
            for (float& v : audio_buf_) {
                deemph_y_ += (v - deemph_y_) * deemph_a_;
                v = deemph_y_;
            }
        } else {
            audio_buf_ = disc_buf_;
        }
    }
    for (float v : audio_buf_) out.push_back(int16_t(std::clamp(v, -1.0f, 1.0f) * 32000));
}

}  // namespace zrf
