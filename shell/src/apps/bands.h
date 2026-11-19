// Bands the RF apps know about: where to jump to, how to listen, and the
// channel grid to scan. Frequencies in Hz.
#pragma once

namespace zs {

struct Band {
    const char* name;
    double start, end;      // scan range
    double channel;         // channel spacing
    double home;            // where "go to this band" lands
    const char* mode;       // daemon mode name: wfm, nfm, am, usb, lsb or off
    int span;               // index into the Spectrum app's spans
};

inline constexpr Band kBands[] = {
    {"FM broadcast", 87.5e6, 108e6, 100e3, 98.1e6, "wfm", 1},
    {"Airband", 118e6, 137e6, 25e3, 121.5e6, "am", 1},
    {"Weather radio", 162.4e6, 162.55e6, 25e3, 162.55e6, "nfm", 0},
    {"Marine VHF", 156e6, 162.025e6, 25e3, 156.8e6, "nfm", 1},
    {"2 m amateur", 144e6, 148e6, 5e3, 145.5e6, "nfm", 1},
    {"70 cm amateur", 430e6, 450e6, 12.5e3, 446e6, "nfm", 1},
    {"FRS / GMRS", 462.55e6, 467.725e6, 12.5e3, 462.5625e6, "nfm", 1},
    {"PMR446", 446.00625e6, 446.19375e6, 12.5e3, 446.00625e6, "nfm", 0},
    {"433 MHz devices", 433.05e6, 434.79e6, 25e3, 433.92e6, "off", 1},
    {"CB", 26.965e6, 27.405e6, 10e3, 27.185e6, "am", 1},
    {"20 m amateur", 14e6, 14.35e6, 1e3, 14.2e6, "usb", 0},
    {"40 m amateur", 7e6, 7.3e6, 1e3, 7.15e6, "lsb", 0},
    {"Shortwave 49 m", 5.9e6, 6.2e6, 5e3, 6e6, "am", 0},
    {"Aircraft ADS-B", 1090e6, 1090e6, 1e6, 1090e6, "off", 2},
};

}  // namespace zs
