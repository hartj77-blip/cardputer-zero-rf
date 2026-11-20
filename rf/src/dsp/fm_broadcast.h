// What a broadcast FM station carries besides mono sound: stereo (a 19 kHz
// pilot tone and the left-minus-right signal around 38 kHz) and RDS data
// around 57 kHz. Works on the FM discriminator's output.
#pragma once

#include <cstddef>
#include <vector>

#include "dsp/demod.h"
#include "rds.h"

namespace zrf {

class FmBroadcast {
public:
    // rate: discriminator sample rate (around 250 kHz). decim: down to audio.
    FmBroadcast(double rate, int decim);
    // disc: discriminator output, 1.0 = full (75 kHz) deviation. Appends
    // left and right samples in turn to `out` (the same in both when the
    // station is mono or too weak for stereo).
    void run(const float* disc, size_t n, std::vector<float>& out);

    bool stereo() const { return stereo_; }
    const RdsDecoder& rds() const { return rds_; }
    // Forget the station: called when the receiver is retuned.
    void reset();

private:
    void rds_sample(float i, float q);

    double rate_;
    // Phase-locked loop on the pilot tone. Everything else is timed from it:
    // the stereo subcarrier is its 2nd harmonic, RDS its 3rd, and one RDS
    // bit lasts exactly 16 pilot cycles.
    double phase_ = 0, step_, nominal_step_, bit_phase_ = 0;
    float pilot_level_ = 0;
    bool stereo_ = false;

    FirDecim<float> mono_, diff_;
    std::vector<float> diff_in_, mono_out_, diff_out_;
    float deemph_a_, deemph_l_ = 0, deemph_r_ = 0;

    // RDS: both phases of the 57 kHz subcarrier (stations may use either),
    // brought down to a few samples per bit.
    FirDecim<float> rds_i_, rds_q_;
    std::vector<float> rds_i_in_, rds_q_in_, rds_i_out_, rds_q_out_, bit_phase_at_;
    // Eight candidate bit timings on each phase; the strongest is used.
    static const int kTimings = 8;
    float acc_[2][kTimings] = {}, energy_[2][kTimings] = {}, last_frac_[kTimings] = {};
    int best_axis_ = 0, best_timing_ = 0, symbols_ = 0;
    bool prev_symbol_ = false;
    size_t decim_count_ = 0;
    RdsDecoder rds_;
};

}  // namespace zrf
