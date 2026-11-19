#include "sdr/source.h"

#include <chrono>
#include <cstdlib>
#include <thread>

namespace zrf {

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void Pacer::reset() { due_ = 0; }

void Pacer::wait(size_t samples, double rate) {
    double now = now_s();
    // Start the clock, or restart it after a long stall instead of bursting.
    if (due_ == 0 || now - due_ > 0.5) due_ = now;
    due_ += samples / rate;
    if (due_ > now) std::this_thread::sleep_for(std::chrono::duration<double>(due_ - now));
}

std::unique_ptr<Source> make_source(const std::string& spec, std::string& err) {
    if (spec == "sim") return make_sim_source();
    if (spec.rfind("file:", 0) == 0) return make_file_source(spec.substr(5));
    if (spec == "rtlsdr" || spec.rfind("rtlsdr:", 0) == 0) {
#ifdef ZRF_HAVE_RTLSDR
        int index = spec.size() > 7 ? std::atoi(spec.c_str() + 7) : 0;
        return make_rtlsdr_source(index);
#else
        err = "built without RTL-SDR support";
        return nullptr;
#endif
    }
    err = "unknown source: " + spec;
    return nullptr;
}

}  // namespace zrf
