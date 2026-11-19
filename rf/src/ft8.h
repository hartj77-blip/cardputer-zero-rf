// FT8: the weak-signal digital mode. Decoding is done by ft8_lib (Karlis
// Goba, MIT licence), fetched at build time; this is the thin layer we use.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zrf {

struct Ft8Message {
    double freq_hz = 0;   // audio frequency of the lowest tone
    double dt = 0;        // seconds from the start of the slot (0.5 is on time)
    int snr_db = 0;       // estimated signal-to-noise ratio in 2500 Hz, as FT8 programs report it
    std::string text;
};

// Decode one 15-second slot of audio (mono, -1..1, starting at the slot's
// start). sample_rate * 0.16 must be a whole number (32000 and 31250 are).
std::vector<Ft8Message> ft8_decode(const float* audio, size_t n, int sample_rate);

const int kFt8Symbols = 79;
const double kFt8SymbolSeconds = 0.16, kFt8ToneSpacing = 6.25, kFt8SlotSeconds = 15.0;

// The 79 tones (0..7) that carry a message, for generating test signals.
// False if the text can't be packed into an FT8 message.
bool ft8_tones(const std::string& text, uint8_t tones[kFt8Symbols]);

}  // namespace zrf
