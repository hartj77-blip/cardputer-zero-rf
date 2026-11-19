// Bands the RF apps know about: where to jump to, how to listen, and the
// channel grid to scan. Frequencies in Hz. Amateur allocations follow ITU
// Region 2 (the Americas); edges differ a little elsewhere.
#pragma once

#include <cstddef>

namespace zs {

struct Band {
    const char* name;
    double start, end;      // band edges (also the scan range)
    double channel;         // channel spacing
    double home;            // where "go to this band" lands
    const char* mode;       // daemon mode name: wfm, nfm, am, usb, lsb, cw or off
    int span;               // index into the Spectrum app's spans
    bool amateur;           // an amateur radio band
    bool scan;              // offered in the Scanner (channelised voice bands)
};

inline constexpr Band kBands[] = {
    {"FM broadcast", 87.5e6, 108e6, 100e3, 98.1e6, "wfm", 1, false, true},
    {"Airband", 118e6, 137e6, 25e3, 121.5e6, "am", 1, false, true},
    {"Weather radio", 162.4e6, 162.55e6, 25e3, 162.55e6, "nfm", 0, false, true},
    {"Marine VHF", 156e6, 162.025e6, 25e3, 156.8e6, "nfm", 1, false, true},
    {"FRS / GMRS", 462.55e6, 467.725e6, 12.5e3, 462.5625e6, "nfm", 1, false, true},
    {"PMR446", 446.00625e6, 446.19375e6, 12.5e3, 446.00625e6, "nfm", 0, false, true},
    {"CB", 26.965e6, 27.405e6, 10e3, 27.185e6, "am", 1, false, true},
    {"433 MHz devices", 433.05e6, 434.79e6, 25e3, 433.92e6, "off", 1, false, false},
    {"Shortwave 49 m", 5.9e6, 6.2e6, 5e3, 6e6, "am", 0, false, false},
    {"Aircraft ADS-B", 1090e6, 1090e6, 1e6, 1090e6, "off", 2, false, false},
    // Amateur bands. Below 10 MHz voice is lower sideband, above it upper
    // (60 m is the exception); 30 m has no voice.
    {"160 m", 1.8e6, 2e6, 1e3, 1.9e6, "lsb", 0, true, false},
    {"80 m", 3.5e6, 4e6, 1e3, 3.85e6, "lsb", 0, true, false},
    {"60 m", 5.3305e6, 5.4065e6, 1e3, 5.357e6, "usb", 0, true, false},
    {"40 m", 7e6, 7.3e6, 1e3, 7.2e6, "lsb", 0, true, false},
    {"30 m", 10.1e6, 10.15e6, 1e3, 10.116e6, "cw", 0, true, false},
    {"20 m", 14e6, 14.35e6, 1e3, 14.2e6, "usb", 0, true, false},
    {"17 m", 18.068e6, 18.168e6, 1e3, 18.13e6, "usb", 0, true, false},
    {"15 m", 21e6, 21.45e6, 1e3, 21.3e6, "usb", 0, true, false},
    {"12 m", 24.89e6, 24.99e6, 1e3, 24.95e6, "usb", 0, true, false},
    {"10 m", 28e6, 29.7e6, 1e3, 28.4e6, "usb", 0, true, false},
    {"6 m", 50e6, 54e6, 1e3, 50.125e6, "usb", 0, true, false},
    {"2 m", 144e6, 148e6, 5e3, 146.52e6, "nfm", 1, true, true},
    {"1.25 m", 222e6, 225e6, 5e3, 223.5e6, "nfm", 1, true, true},
    {"70 cm", 420e6, 450e6, 12.5e3, 446e6, "nfm", 1, true, true},
    {"33 cm", 902e6, 928e6, 12.5e3, 927.5e6, "nfm", 1, true, false},
    {"23 cm", 1240e6, 1300e6, 25e3, 1294.5e6, "nfm", 1, true, false},
};

// The band a frequency falls in, or null. Amateur bands win where a
// narrower service sits inside one (PMR446 inside 70 cm).
inline const Band* band_at(double hz) {
    const Band* found = nullptr;
    for (const Band& b : kBands)
        if (hz >= b.start && hz <= b.end && (!found || b.amateur)) found = &b;
    return found;
}

// How a frequency is normally listened to.
inline const char* suggested_mode(double hz) {
    if (const Band* b = band_at(hz))
        if (b->mode[0] != 'o') {
            // The bottom of each HF amateur band is Morse.
            if (b->amateur && hz < 30e6 && hz < b->start + (b->end - b->start) * 0.2 && b->mode[0] != 'c') return "cw";
            return b->mode;
        }
    return hz < 30e6 ? "am" : "nfm";
}

}  // namespace zs
