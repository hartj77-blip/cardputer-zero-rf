#include "dsp/fm_broadcast.h"

#include <cmath>

namespace zrf {
namespace {
const double kPilotHz = 19000;
const int kRdsDecim = 16;
}

FmBroadcast::FmBroadcast(double rate, int decim)
    : rate_(rate),
      step_(2 * M_PI * kPilotHz / rate),
      nominal_step_(2 * M_PI * kPilotHz / rate),
      mono_(decim, 15e3 / rate, decim == 1 ? 1 : 6 * decim + 1),
      diff_(decim, 15e3 / rate, decim == 1 ? 1 : 6 * decim + 1),
      // 75 microsecond de-emphasis (Americas, Korea). Europe uses 50; to be a setting.
      deemph_a_(float(1 - std::exp(-1.0 / (rate / decim * 75e-6)))),
      rds_i_(kRdsDecim, 2400 / rate, 8 * kRdsDecim + 1),
      rds_q_(kRdsDecim, 2400 / rate, 8 * kRdsDecim + 1) {}

void FmBroadcast::reset() {
    rds_ = RdsDecoder();
    stereo_ = false;
    pilot_level_ = 0;
    for (auto& axis : energy_)
        for (float& e : axis) e = 0;
}

void FmBroadcast::run(const float* disc, size_t n, std::vector<float>& out) {
    // Loop gains for a pilot at about 9% of full deviation and a loop some
    // 30 Hz wide: narrow enough to ignore the music around it.
    const double kp = 0.023, ki = 1.2e-5;
    diff_in_.resize(n);
    rds_i_in_.resize(n);
    rds_q_in_.resize(n);
    bit_phase_at_.clear();
    for (size_t k = 0; k < n; ++k) {
        float x = disc[k];
        float s = float(std::sin(phase_)), c = float(std::cos(phase_));
        // The pilot is sin(phase): multiplied by our cosine it averages to
        // zero when we are in step, and by our sine to half its amplitude.
        double err = x * c;
        pilot_level_ += (x * s - pilot_level_) * 2e-4f;
        step_ += ki * err * nominal_step_;
        step_ = std::min(std::max(step_, nominal_step_ * 0.995), nominal_step_ * 1.005);
        double advance = step_ + kp * err * nominal_step_;
        phase_ += advance;
        if (phase_ > 2 * M_PI) phase_ -= 2 * M_PI;
        bit_phase_ += advance / 16;
        if (bit_phase_ > 2 * M_PI) bit_phase_ -= 2 * M_PI;

        diff_in_[k] = x * 2 * (2 * s * c);                     // sin of twice the pilot phase
        rds_i_in_[k] = x * (3 * s - 4 * s * s * s);            // sin of three times
        rds_q_in_[k] = x * (4 * c * c * c - 3 * c);            // cos of three times
        if (++decim_count_ % kRdsDecim == 0) bit_phase_at_.push_back(float(bit_phase_));
    }
    // A real pilot gives about 0.045 here; noise alone stays near zero.
    stereo_ = stereo_ ? pilot_level_ > 0.012f : pilot_level_ > 0.02f;

    mono_out_.clear();
    diff_out_.clear();
    mono_.run(disc, n, mono_out_);
    diff_.run(diff_in_.data(), n, diff_out_);
    for (size_t k = 0; k < mono_out_.size(); ++k) {
        float m = mono_out_[k], d = stereo_ && k < diff_out_.size() ? diff_out_[k] : 0;
        deemph_l_ += (m + d - deemph_l_) * deemph_a_;
        deemph_r_ += (m - d - deemph_r_) * deemph_a_;
        out.push_back(deemph_l_);
        out.push_back(deemph_r_);
    }

    rds_i_out_.clear();
    rds_q_out_.clear();
    rds_i_.run(rds_i_in_.data(), n, rds_i_out_);
    rds_q_.run(rds_q_in_.data(), n, rds_q_out_);
    // The filters and our phase list count the same input samples, so they
    // line up to within one output sample; the timing search absorbs that.
    size_t count = std::min(rds_i_out_.size(), bit_phase_at_.size());
    if (stereo_)
        for (size_t k = 0; k < count; ++k) {
            float theta = bit_phase_at_[k];
            for (int t = 0; t < kTimings; ++t) {
                float ph = theta + float(t) * float(2 * M_PI / kTimings);
                float frac = ph - float(2 * M_PI) * std::floor(ph / float(2 * M_PI));
                if (frac < last_frac_[t]) {
                    // One bit has gone by on this timing: bank it.
                    for (int a = 0; a < 2; ++a) {
                        energy_[a][t] += (acc_[a][t] * acc_[a][t] - energy_[a][t]) * 0.02f;
                        if (a == best_axis_ && t == best_timing_) {
                            bool symbol = acc_[a][t] > 0;
                            // Data is in the changes, so which way up we see it doesn't matter.
                            rds_.push_bit(symbol != prev_symbol_);
                            prev_symbol_ = symbol;
                            // Now and then, move to a clearly better timing.
                            if (++symbols_ % 64 == 0) {
                                int ba = best_axis_, bt = best_timing_;
                                for (int a2 = 0; a2 < 2; ++a2)
                                    for (int t2 = 0; t2 < kTimings; ++t2)
                                        if (energy_[a2][t2] > energy_[ba][bt] * 1.3f) { ba = a2; bt = t2; }
                                best_axis_ = ba;
                                best_timing_ = bt;
                            }
                        }
                        acc_[a][t] = 0;
                    }
                }
                last_frac_[t] = frac;
                // One cycle of a sine per bit is the shape RDS bits are sent in.
                float w = std::sin(frac);
                acc_[0][t] += rds_i_out_[k] * w;
                acc_[1][t] += rds_q_out_[k] * w;
            }
        }
}

}  // namespace zrf
