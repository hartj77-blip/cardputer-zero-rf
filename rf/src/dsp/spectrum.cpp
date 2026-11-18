#include "dsp/spectrum.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace zrf {

Fft::Fft(size_t n) : n_(n), twiddle_(n / 2), rev_(n) {
    if (n < 2 || (n & (n - 1)) != 0) throw std::invalid_argument("FFT size must be a power of two");
    for (size_t i = 0; i < n / 2; ++i) {
        double a = -2.0 * M_PI * double(i) / double(n);
        twiddle_[i] = cf32(float(std::cos(a)), float(std::sin(a)));
    }
    size_t bits = 0;
    while ((size_t(1) << bits) < n) ++bits;
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = 0;
        for (size_t b = 0; b < bits; ++b)
            if (i & (size_t(1) << b)) r |= uint32_t(1) << (bits - 1 - b);
        rev_[i] = r;
    }
}

void Fft::run(cf32* d) const {
    for (size_t i = 0; i < n_; ++i)
        if (rev_[i] > i) std::swap(d[i], d[rev_[i]]);
    for (size_t len = 2; len <= n_; len <<= 1) {
        size_t half = len / 2, step = n_ / len;
        for (size_t i = 0; i < n_; i += len) {
            for (size_t k = 0; k < half; ++k) {
                cf32 t = d[i + k + half] * twiddle_[k * step];
                d[i + k + half] = d[i + k] - t;
                d[i + k] += t;
            }
        }
    }
}

Spectrum::Spectrum(size_t nfft) : fft_(nfft), window_(nfft), work_(nfft), acc_(nfft, 0.0) {
    double sum = 0;
    for (size_t i = 0; i < nfft; ++i) {
        window_[i] = float(0.5 - 0.5 * std::cos(2.0 * M_PI * double(i) / double(nfft)));   // Hann
        sum += window_[i];
    }
    // A full-scale tone on a bin centre reads 0 dB.
    scale_ = float(1.0 / (sum * sum));
}

void Spectrum::add(const cf32* samples) {
    size_t n = fft_.size();
    for (size_t i = 0; i < n; ++i) work_[i] = samples[i] * window_[i];
    fft_.run(work_.data());
    for (size_t i = 0; i < n; ++i) acc_[i] += std::norm(work_[i]);
    ++count_;
}

std::vector<float> Spectrum::take() {
    std::vector<float> out;
    if (count_ == 0) return out;
    size_t n = fft_.size(), half = n / 2;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        // FFT order is 0..+rate/2 then -rate/2..0; swap halves.
        double p = acc_[(i + half) % n] / count_ * scale_;
        out[i] = float(10.0 * std::log10(p + 1e-20));
        acc_[(i + half) % n] = 0;
    }
    count_ = 0;
    return out;
}

std::vector<float> reduce_peak(const std::vector<float>& in, size_t cols) {
    std::vector<float> out(cols);
    size_t n = in.size();
    for (size_t c = 0; c < cols; ++c) {
        size_t a = c * n / cols, b = std::max(a + 1, (c + 1) * n / cols);
        out[c] = *std::max_element(in.begin() + long(a), in.begin() + long(b));
    }
    return out;
}

}  // namespace zrf
