// Demodulators: IQ samples centred on the wanted signal in, mono audio out.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zrf {

using cf32 = std::complex<float>;

enum class Mode { Off, Wfm, Nfm, Am, Usb, Lsb, Cw };

// "off", "wfm" (broadcast FM), "nfm" (narrow FM), "am", "usb", "lsb"
// (upper and lower sideband), "cw" (Morse: a carrier on the tuned frequency
// is heard as a 700 Hz tone). False if unknown.
bool parse_mode(const std::string& s, Mode& out);
const char* mode_name(Mode m);

// Low-pass filter that keeps every `decim`-th output. T is cf32 or float.
template <typename T>
class FirDecim {
public:
    // cutoff is a fraction of the input sample rate (0..0.5).
    FirDecim(int decim, double cutoff, int ntaps);
    void run(const T* in, size_t n, std::vector<T>& out);
private:
    int decim_, phase_ = 0;
    std::vector<float> taps_;
    std::vector<T> hist_;   // the last ntaps-1 inputs, then the new block
};

class FmBroadcast;
class RdsDecoder;

class Demod {
public:
    ~Demod();
    // rate: input sample rate in Hz (at least 64 kHz). mode must not be Off.
    Demod(Mode mode, double rate);
    double audio_rate() const { return audio_rate_; }
    // 2 for broadcast FM (left and right in turn), 1 for everything else.
    int channels() const { return mode_ == Mode::Wfm ? 2 : 1; }
    // Appends 16-bit audio samples to `out`.
    void run(const cf32* in, size_t n, std::vector<int16_t>& out);
    // Broadcast FM extras: whether the station is in stereo, and its RDS
    // data (null in other modes). station_changed() forgets both on a retune.
    bool stereo() const;
    const RdsDecoder* rds() const;
    void station_changed();

private:
    Mode mode_;
    double audio_rate_;
    // To the channel bandwidth, in two steps: a wide filter that brings the
    // rate down cheaply, then a sharp one that keeps neighbours out.
    FirDecim<cf32> channel_, channel2_;
    std::vector<cf32> stage_buf_;
    void run_channel(const cf32* in, size_t n);
    std::unique_ptr<FmBroadcast> fm_;   // broadcast FM only: stereo and RDS from the discriminator
    std::vector<cf32> chan_buf_;
    std::vector<float> disc_buf_, audio_buf_;
    cf32 prev_{1, 0};
    float fm_gain_, deemph_a_, deemph_y_ = 0, am_level_ = 0;
    // Sideband, at the audio rate: shift the wanted 3 kHz to the middle,
    // filter sharply (the other sideband is 400 Hz away), shift it back.
    FirDecim<cf32> sideband_;
    cf32 pre_{1, 0}, pre_step_{1, 0}, post_{1, 0}, post_step_{1, 0};
    std::vector<cf32> mix_buf_, side_buf_;
    float ssb_level_ = 0;
    int renorm_ = 0;
};

}  // namespace zrf
