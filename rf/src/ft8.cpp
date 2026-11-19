#include "ft8.h"

#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include <common/monitor.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <ft8/message.h>
}

namespace zrf {

std::vector<Ft8Message> ft8_decode(const float* audio, size_t n, int sample_rate) {
    std::vector<Ft8Message> out;
    monitor_config_t cfg{};
    cfg.f_min = 200;
    cfg.f_max = 3000;
    cfg.sample_rate = sample_rate;
    cfg.time_osr = 2;
    cfg.freq_osr = 2;
    cfg.protocol = FTX_PROTOCOL_FT8;
    monitor_t mon;
    monitor_init(&mon, &cfg);
    for (size_t pos = 0; pos + size_t(mon.block_size) <= n; pos += size_t(mon.block_size)) monitor_process(&mon, audio + pos);

    // Strongest sync candidates first; a signal usually shows up as several.
    const int kMaxCandidates = 140, kMinScore = 10, kLdpcIterations = 25;
    ftx_candidate_t cands[kMaxCandidates];
    int found = ftx_find_candidates(&mon.wf, kMaxCandidates, cands, kMinScore);
    // Noise floor: the median level over the whole slot (0.5 dB steps).
    std::vector<uint8_t> levels(mon.wf.mag, mon.wf.mag + size_t(mon.wf.num_blocks) * size_t(mon.wf.block_stride));
    double noise_db = 0;
    if (!levels.empty()) {
        std::nth_element(levels.begin(), levels.begin() + long(levels.size() / 2), levels.end());
        noise_db = levels[levels.size() / 2] * 0.5;
    }
    std::vector<ftx_message_t> seen;
    for (int i = 0; i < found; ++i) {
        const ftx_candidate_t& c = cands[i];
        ftx_message_t msg;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(&mon.wf, &c, kLdpcIterations, &msg, &status)) continue;
        bool duplicate = false;
        for (const ftx_message_t& s : seen)
            if (std::memcmp(s.payload, msg.payload, sizeof msg.payload) == 0) duplicate = true;
        if (duplicate) continue;
        seen.push_back(msg);

        char text[FTX_MAX_MESSAGE_LENGTH + 1] = {};
        ftx_message_offsets_t offsets;
        if (ftx_message_decode(&msg, nullptr, text, &offsets) != FTX_MESSAGE_RC_OK) continue;
        Ft8Message m;
        m.freq_hz = (mon.min_bin + c.freq_offset + double(c.freq_sub) / mon.wf.freq_osr) / mon.symbol_period;
        // The analysis window reaches one symbol back, so times read one symbol late.
        m.dt = (c.time_offset + double(c.time_sub) / mon.wf.time_osr) * mon.symbol_period - mon.symbol_period;
        // Strength: the loudest of the eight tones in each symbol, averaged,
        // against the noise floor, then referred to the customary 2500 Hz.
        const ftx_waterfall_t& wf = mon.wf;
        double sum = 0;
        int count = 0;
        for (int sym = 0; sym < FT8_NN; ++sym) {
            int block = c.time_offset + sym;
            if (block < 0 || block >= wf.num_blocks) continue;
            const uint8_t* row = wf.mag + ((block * wf.time_osr + c.time_sub) * wf.freq_osr + c.freq_sub) * wf.num_bins;
            int best = 0;
            for (int tone = 0; tone < 8; ++tone)
                if (c.freq_offset + tone < wf.num_bins) best = std::max(best, int(row[c.freq_offset + tone]));
            sum += best * 0.5;
            ++count;
        }
        m.snr_db = count ? int(std::lround(sum / count - noise_db - 10 * std::log10(2500.0 / 6.25))) : 0;
        m.text = text;
        out.push_back(std::move(m));
    }
    monitor_free(&mon);
    return out;
}

bool ft8_tones(const std::string& text, uint8_t tones[kFt8Symbols]) {
    ftx_message_t msg;
    if (ftx_message_encode(&msg, nullptr, text.c_str()) != FTX_MESSAGE_RC_OK) return false;
    ft8_encode(msg.payload, tones);
    return true;
}

}  // namespace zrf
