#include "dsp/demod.h"

#include <algorithm>
#include <cmath>

namespace zrf {

bool parse_mode(const std::string& s, Mode& out) {
    if (s == "off") out = Mode::Off;
    else if (s == "wfm") out = Mode::Wfm;
    else if (s == "nfm") out = Mode::Nfm;
    else if (s == "am") out = Mode::Am;
    else return false;
    return true;
}

const char* mode_name(Mode m) {
    switch (m) {
    case Mode::Wfm: return "wfm";
    case Mode::Nfm: return "nfm";
    case Mode::Am: return "am";
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

struct Plan {
    int chan_decim, audio_decim;
    double chan_cutoff_hz, chan_rate;
};

Plan make_plan(Mode mode, double rate) {
    int total = std::max(1, int(std::lround(rate / kAudioTarget)));
    Plan p{};
    if (mode == Mode::Wfm) {
        // Discriminate at about 8x the audio rate so the +-75 kHz swing fits.
        p.audio_decim = std::min(total, 8);
        p.chan_decim = std::max(1, total / p.audio_decim);
        p.chan_cutoff_hz = 100e3;
    } else {
        p.audio_decim = 1;
        p.chan_decim = total;
        p.chan_cutoff_hz = mode == Mode::Nfm ? 6e3 : 5e3;
    }
    p.chan_rate = rate / p.chan_decim;
    return p;
}

FirDecim<cf32> channel_filter(const Plan& p, double rate) {
    double cutoff = std::min(p.chan_cutoff_hz / rate, 0.45 / p.chan_decim);
    return FirDecim<cf32>(p.chan_decim, cutoff, p.chan_decim == 1 ? 1 : std::min(4 * p.chan_decim + 1, 257));
}

}  // namespace

Demod::Demod(Mode mode, double rate)
    : mode_(mode),
      audio_rate_(make_plan(mode, rate).chan_rate / make_plan(mode, rate).audio_decim),
      channel_(channel_filter(make_plan(mode, rate), rate)),
      audio_(make_plan(mode, rate).audio_decim, 15e3 / make_plan(mode, rate).chan_rate,
             make_plan(mode, rate).audio_decim == 1 ? 1 : 6 * make_plan(mode, rate).audio_decim + 1) {
    Plan p = make_plan(mode, rate);
    // Full deviation (75 kHz broadcast, 5 kHz narrow) maps to full scale.
    double deviation = mode == Mode::Wfm ? 75e3 : 5e3;
    fm_gain_ = float(p.chan_rate / (2 * M_PI * deviation));
    // 75 microsecond de-emphasis (Americas, Korea). Europe uses 50; to be a setting.
    deemph_a_ = float(1 - std::exp(-1.0 / (audio_rate_ * 75e-6)));
}

void Demod::run(const cf32* in, size_t n, std::vector<int16_t>& out) {
    chan_buf_.clear();
    channel_.run(in, n, chan_buf_);
    audio_buf_.clear();

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
