// RTL-SDR dongles through librtlsdr (RTL-SDR Blog V3/V4 and generic R820T).
// Not yet run against real hardware.
#include <cstdint>
#include <vector>

#include <rtl-sdr.h>

#include "sdr/source.h"

namespace zrf {
namespace {

class RtlSdrSource : public Source {
public:
    explicit RtlSdrSource(int index) : index_(index), buf_(16384) {}
    ~RtlSdrSource() override { if (dev_) rtlsdr_close(dev_); }

    bool open(std::string& err) override {
        if (dev_) { rtlsdr_close(dev_); dev_ = nullptr; }
        if (rtlsdr_get_device_count() == 0) { err = "no dongle"; return false; }
        if (rtlsdr_open(&dev_, uint32_t(index_)) != 0) {
            dev_ = nullptr;
            err = "can't open dongle (in use, or no permission)";
            return false;
        }
        rtlsdr_reset_buffer(dev_);
        pos_ = len_ = 0;
        return true;
    }
    bool set_freq(double hz) override {
        return dev_ && rtlsdr_set_center_freq(dev_, uint32_t(hz + 0.5)) == 0;
    }
    bool set_rate(double hz) override {
        if (!dev_ || rtlsdr_set_sample_rate(dev_, uint32_t(hz + 0.5)) != 0) return false;
        rtlsdr_reset_buffer(dev_);
        pos_ = len_ = 0;
        return true;
    }
    bool set_gain(std::optional<double> db) override {
        if (!dev_) return false;
        if (!db) return rtlsdr_set_tuner_gain_mode(dev_, 0) == 0;
        if (rtlsdr_set_tuner_gain_mode(dev_, 1) != 0) return false;
        return rtlsdr_set_tuner_gain(dev_, nearest_gain(int(*db * 10))) == 0;   // tenths of a dB
    }

    bool set_ppm(int ppm) override {
        if (!dev_) return false;
        // The library reports an error when the value is already set.
        return rtlsdr_get_freq_correction(dev_) == ppm || rtlsdr_set_freq_correction(dev_, ppm) == 0;
    }
    bool set_bias_tee(bool on) override { return dev_ && rtlsdr_set_bias_tee(dev_, on ? 1 : 0) == 0; }

    size_t read(cf32* out, size_t n) override {
        if (!dev_) return 0;
        for (size_t i = 0; i < n; ++i) {
            if (pos_ + 2 > len_) {
                int got = 0;
                // Lengths must be a multiple of 512 bytes.
                if (rtlsdr_read_sync(dev_, buf_.data(), int(buf_.size()), &got) != 0 || got < 2) return 0;
                pos_ = 0;
                len_ = size_t(got) & ~size_t(1);
            }
            out[i] = cf32((buf_[pos_] - 127.5f) / 127.5f, (buf_[pos_ + 1] - 127.5f) / 127.5f);
            pos_ += 2;
        }
        return n;
    }

private:
    // The tuner only accepts the gain steps it lists.
    int nearest_gain(int tenths) {
        int count = rtlsdr_get_tuner_gains(dev_, nullptr);
        if (count <= 0) return tenths;
        std::vector<int> gains(size_t(count), 0);
        rtlsdr_get_tuner_gains(dev_, gains.data());
        int best = gains[0];
        for (int g : gains)
            if (std::abs(g - tenths) < std::abs(best - tenths)) best = g;
        return best;
    }

    int index_;
    rtlsdr_dev_t* dev_ = nullptr;
    std::vector<uint8_t> buf_;
    size_t pos_ = 0, len_ = 0;
};

}  // namespace

std::unique_ptr<Source> make_rtlsdr_source(int index) { return std::make_unique<RtlSdrSource>(index); }

}  // namespace zrf
