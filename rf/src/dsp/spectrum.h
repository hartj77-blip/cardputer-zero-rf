// FFT and power spectrum.
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace zrf {

using cf32 = std::complex<float>;

// In-place radix-2 FFT. Size must be a power of two.
class Fft {
public:
    explicit Fft(size_t n);
    size_t size() const { return n_; }
    void run(cf32* data) const;
private:
    size_t n_;
    std::vector<cf32> twiddle_;
    std::vector<uint32_t> rev_;
};

// Averages windowed FFTs into one power spectrum per frame.
class Spectrum {
public:
    explicit Spectrum(size_t nfft);
    size_t size() const { return fft_.size(); }
    // Add one block of size() samples to the current frame.
    void add(const cf32* samples);
    // Finish the frame: power in dB relative to full scale, lowest frequency
    // first (so the tuned frequency is in the middle). Empty if nothing was added.
    std::vector<float> take();
private:
    Fft fft_;
    std::vector<float> window_;
    std::vector<cf32> work_;
    std::vector<double> acc_;
    float scale_;
    int count_ = 0;
};

// Reduce a spectrum to `cols` columns, keeping the strongest bin in each so
// narrow signals don't vanish. cols must be 1..in.size().
std::vector<float> reduce_peak(const std::vector<float>& in, size_t cols);

}  // namespace zrf
