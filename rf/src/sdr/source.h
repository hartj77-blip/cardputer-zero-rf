// SDR source layer: the only way the radio daemon reaches a receiver.
//
// Backends: "sim" (generated signals, for the simulator and CI),
// "file:<path>" (IQ recording playback) and "rtlsdr[:index]" (a real dongle).
#pragma once

#include <complex>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace zrf {

using cf32 = std::complex<float>;

class Source {
public:
    virtual ~Source() = default;
    // False with a message in err if the receiver can't be opened.
    virtual bool open(std::string& err) = 0;
    virtual bool set_freq(double hz) = 0;
    virtual bool set_rate(double hz) = 0;
    // nullopt = automatic gain.
    virtual bool set_gain(std::optional<double> db) = 0;
    // Blocks until n samples are ready, at the source's real pace.
    // Returns the number read; 0 means the source failed or was unplugged.
    virtual size_t read(cf32* out, size_t n) = 0;
};

// spec: "sim", "file:<path>", "rtlsdr" or "rtlsdr:<index>".
// Null with a message in err for an unknown spec or a backend not built in.
std::unique_ptr<Source> make_source(const std::string& spec, std::string& err);

std::unique_ptr<Source> make_sim_source();
std::unique_ptr<Source> make_file_source(const std::string& path);
#ifdef ZRF_HAVE_RTLSDR
std::unique_ptr<Source> make_rtlsdr_source(int index);
#endif

// Sleeps so that samples leave a non-hardware source at the sample rate.
class Pacer {
public:
    void reset();
    void wait(size_t samples, double rate);
private:
    double due_ = 0;   // seconds, steady clock
};

}  // namespace zrf
