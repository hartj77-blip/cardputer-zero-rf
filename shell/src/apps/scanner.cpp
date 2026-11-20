// Scanner: sweep a band, stop on the first channel with a signal, listen until
// it goes quiet, then carry on. Keeps a log of what was heard.
//
// Left/Right change band, Space skips the current channel, H holds on it,
// L locks it out (skips it from now on), C clears the log and lockouts.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "apps/bands.h"
#include "apps/background_radio.h"
#include "apps/common.h"
#include "apps/radio_link.h"
#include "memories.h"

namespace zs {
namespace {
using namespace theme;

const int kW = kScreenW;
const int kStripY = 18, kStripH = 40;
const int kRows = 5;
const double kRate = 1.024e6;
const double kUsable = 0.42;        // of the view each side of centre; the edges roll off
const int kThreshold = 24;          // 12 dB above the noise floor, in the 0.5 dB steps frames use
const uint32_t kHangMs = 2500;      // stay this long after a signal drops
const int kSettleFrames = 2;        // frames to look at after each retune

uint16_t rgb565(uint32_t hex) { return uint16_t((hex >> 19 & 31) << 11 | (hex >> 10 & 63) << 5 | (hex >> 3 & 31)); }

struct Hit {
    double hz;
    int count;
    uint32_t last;   // lv_tick
};

class ScannerApp : public App {
public:
    ScannerApp() {
        for (int i = 0; i < int(std::size(kBands)); ++i)
            if (kBands[i].scan) bands_.push_back(i);
        band_ = 1;   // Airband
    }
    ~ScannerApp() override {
        if (timer_) lv_timer_delete(timer_);
    }

    void build(lv_obj_t* root) override {
        BackgroundRadio::get().stop();   // this app needs the receiver
        title_ = label(root, "", &lv_font_montserrat_14, kAccent);
        lv_obj_set_pos(title_, 4, 0);
        state_ = label(root, "", &lv_font_montserrat_14, kText);
        lv_obj_align(state_, LV_ALIGN_TOP_RIGHT, -4, 0);

        pixels_.assign(size_t(kW) * kStripH, rgb565(kBg));
        canvas_ = lv_canvas_create(root);
        lv_canvas_set_buffer(canvas_, pixels_.data(), kW, kStripH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 0, kStripY);

        for (int i = 0; i < kRows; ++i) {
            rows_[i] = label(root, "", &lv_font_montserrat_12, kText);
            lv_obj_set_pos(rows_[i], 8, kStripY + kStripH + 3 + i * 14);
        }
        empty_ = label(root, "Nothing heard yet", &lv_font_montserrat_12, kMuted);
        lv_obj_set_pos(empty_, 8, kStripY + kStripH + 3);

        hint_ = label(root, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " band   Space skip   H hold   L lock out",
                      &lv_font_montserrat_12, kMuted);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -1);

        start_band();
        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<ScannerApp*>(lv_timer_get_user_data(t))->pump(); },
                                 40, this);
        pump();
    }

    void tick() override {
        if (++ticks_ % 10 == 0) update_rows();   // ages
    }

    bool on_key(uint32_t key) override {
        switch (key) {
        // One more choice than there are bands: your saved channels.
        case LV_KEY_LEFT: band_ = (band_ + int(bands_.size())) % (int(bands_.size()) + 1); start_band(); return true;
        case LV_KEY_RIGHT: band_ = (band_ + 1) % (int(bands_.size()) + 1); start_band(); return true;
        case ' ':
            if (stopped_) resume();
            return true;
        case 'h': case 'H':
            if (stopped_) { held_ = !held_; update_state(); }
            return true;
        case 'l': case 'L':
            if (stopped_) {
                locked_.push_back(channel_hz_);
                shell_toast(fmt("%.4f MHz locked out", channel_hz_ / 1e6));
                resume();
            }
            return true;
        case 'c': case 'C':
            hits_.clear();
            locked_.clear();
            update_rows();
            return true;
        case LV_KEY_UP: case LV_KEY_DOWN: case LV_KEY_ENTER: return true;
        default: return false;
        }
    }

private:
    bool saved_mode() const { return band_ == int(bands_.size()); }

    // Scanning saved channels reuses the band sweep: each channel in turn is
    // treated as a band one channel wide.
    const Band& band() const { return saved_mode() ? saved_band_ : kBands[bands_[size_t(band_)]]; }

    void use_saved(size_t i) {
        saved_index_ = i;
        const zrf::Memory& m = saved_[i];
        saved_mode_name_ = m.mode == "off" ? "nfm" : m.mode;
        double width = saved_mode_name_ == "wfm" ? 100e3 : 12.5e3;
        saved_band_ = Band{"Saved channels", m.hz, m.hz, width, m.hz, saved_mode_name_.c_str(), 1, false, true};
    }

    // Move on: the next sweep step of a band, or the next saved channel.
    void advance() {
        if (saved_mode()) {
            if (saved_.empty()) return;
            use_saved((saved_index_ + 1) % saved_.size());
            chunk_ = 0;
        } else {
            chunk_ = (chunk_ + 1) % chunk_count();
        }
    }

    void start_band() {
        stopped_ = held_ = false;
        chunk_ = 0;
        link_.client.send("mode off");
        if (saved_mode()) {
            saved_ = zrf::load_memories(home_dir() + "/.config/zeroos/rf-memories.tsv");
            lv_label_set_text(title_, "Saved channels");
            if (saved_.empty()) {
                centre_ = 0;   // nothing to tune to: frames are ignored
                lv_label_set_text(state_, "None yet: A in Spectrum");
                lv_obj_set_style_text_color(state_, c(kMuted), 0);
                return;
            }
            use_saved(0);
        }
        tune_chunk();
        lv_label_set_text(title_, band().name);
        update_state();
    }

    // Centre of sweep step k, placed half a channel off the grid so the
    // receiver's own centre spike never sits on a channel.
    double chunk_centre(int k) const {
        const Band& b = band();
        double step = 2 * kUsable * kRate;
        // A band that fits in one step sits in the middle of the view.
        double c = b.end - b.start < step ? (b.start + b.end) / 2 : b.start + (k + 0.5) * step;
        return b.start + std::floor((c - b.start) / b.channel) * b.channel + b.channel / 2;
    }
    int chunk_count() const {
        const Band& b = band();
        return std::max(1, int(std::ceil((b.end - b.start + b.channel) / (2 * kUsable * kRate))));
    }

    void tune_chunk() {
        centre_ = chunk_centre(chunk_);
        link_.client.send(fmt("tune %.0f", centre_));
        settle_ = 0;
    }

    void resume() {
        stopped_ = held_ = false;
        link_.client.send("mode off");
        advance();
        tune_chunk();
        update_state();
    }

    void pump() {
        bool fresh, unavailable;
        if (!link_.ensure(fresh, unavailable)) {
            if (unavailable) lv_label_set_text(state_, "Radio not available");
            return;
        }
        if (fresh) {
            link_.client.send(fmt("rate %.0f", kRate));
            link_.client.send("gain auto");
            link_.client.send("squelch off");
            link_.client.send(fmt("spectrum %d 15", kW));
            start_band();
        }
        zrf::RadioEvents ev;
        if (!link_.client.poll(RadioLink::poll_wait_ms(), ev)) {
            link_.lost();
            return;
        }
        if (ev.has_error && ev.error == "no dongle") lv_label_set_text(state_, "No radio found");
        for (auto& f : ev.frames) {
            if (int(f.cols.size()) != kW || std::fabs(f.freq - centre_) > 1) continue;   // from before the retune
            on_frame(f.cols);
        }
    }

    // Strongest level within half a channel of hz, in a frame centred on centre_.
    int level_at(const std::vector<uint8_t>& cols, double hz) const {
        double half = band().channel / 2;
        int a = int(std::floor((hz - half - centre_) / kRate * kW + kW / 2.0));
        int b = int(std::ceil((hz + half - centre_) / kRate * kW + kW / 2.0));
        a = std::clamp(a, 0, kW - 1);
        b = std::clamp(b, a, kW - 1);
        return *std::max_element(cols.begin() + a, cols.begin() + b + 1);
    }

    void on_frame(const std::vector<uint8_t>& cols) {
        std::vector<uint8_t> sorted = cols;
        std::nth_element(sorted.begin(), sorted.begin() + kW / 2, sorted.end());
        int floor = sorted[kW / 2];
        draw_strip(cols, floor);

        if (stopped_) {
            if (level_at(cols, channel_hz_) - floor >= kThreshold) last_signal_ = lv_tick_get();
            else if (!held_ && lv_tick_elaps(last_signal_) > kHangMs) resume();
            return;
        }
        if (++settle_ < kSettleFrames) return;

        // Look for the strongest signal in this step, ignoring locked-out
        // channels. A signal is wider than one column, so take the middle of
        // everything within 6 dB of its peak, then snap that to the channel grid.
        const Band& b = band();
        double lo = std::max(b.start, centre_ - kUsable * kRate), hi = std::min(b.end, centre_ + kUsable * kRate);
        auto col_hz = [&](double col) { return centre_ + (col + 0.5 - kW / 2.0) * kRate / kW; };
        std::vector<uint8_t> view = cols;
        double guard = std::max(b.channel, 10e3);
        for (int x = 0; x < kW; ++x) {
            double hz = col_hz(x);
            bool locked = std::any_of(locked_.begin(), locked_.end(), [&](double l) { return std::fabs(l - hz) <= guard; });
            if (hz < lo - b.channel / 2 || hz > hi + b.channel / 2 || locked) view[size_t(x)] = 0;
        }
        double best_hz = 0;
        int peak = int(std::max_element(view.begin(), view.end()) - view.begin());
        if (view[size_t(peak)] - floor >= kThreshold) {
            int l = peak, r = peak;
            while (l > 0 && view[size_t(l - 1)] + 12 >= view[size_t(peak)]) --l;
            while (r + 1 < kW && view[size_t(r + 1)] + 12 >= view[size_t(peak)]) ++r;
            double hz = col_hz((l + r) / 2.0);
            best_hz = std::clamp(b.start + std::round((hz - b.start) / b.channel) * b.channel, b.start, b.end);
        }
        if (best_hz == 0) {
            advance();
            tune_chunk();
            update_state();
            return;
        }
        // Stop here and listen.
        stopped_ = true;
        channel_hz_ = centre_ = best_hz;
        last_signal_ = lv_tick_get();
        link_.client.send(fmt("tune %.0f", best_hz));
        link_.client.send(std::string("mode ") + b.mode);
        auto it = std::find_if(hits_.begin(), hits_.end(), [&](const Hit& h) { return std::fabs(h.hz - best_hz) < 1; });
        if (it == hits_.end()) hits_.push_back({best_hz, 1, lv_tick_get()});
        else { ++it->count; it->last = lv_tick_get(); }
        update_state();
        update_rows();
    }

    void draw_strip(const std::vector<uint8_t>& cols, int floor) {
        const uint16_t bg = rgb565(kBg), fill = rgb565(0x12384a), line = rgb565(0x39c5cf), mark = rgb565(kAccent);
        int threshold_y = int((1 - (kThreshold + 12) / 120.0f) * (kStripH - 1));
        for (int x = 0; x < kW; ++x) {
            float t = std::clamp((cols[size_t(x)] - (floor - 12)) / 120.0f, 0.0f, 1.0f);
            int top = int((1 - t) * (kStripH - 1));
            for (int y = 0; y < kStripH; ++y) {
                uint16_t c = y < top ? bg : y == top ? line : fill;
                if (y == threshold_y && x % 4 < 2) c = mark;   // dashed line: signals above it stop the scan
                pixels_[size_t(y) * kW + x] = c;
            }
        }
        lv_obj_invalidate(canvas_);
    }

    void update_state() {
        if (stopped_) {
            lv_label_set_text(state_, fmt("%s %.4f MHz", held_ ? "Hold" : LV_SYMBOL_VOLUME_MAX, channel_hz_ / 1e6).c_str());
            lv_obj_set_style_text_color(state_, c(kGood), 0);
        } else {
            lv_label_set_text(state_, saved_mode() ? fmt("Checking %s", saved_[saved_index_].name.substr(0, 14).c_str()).c_str()
                                                   : fmt("Scanning %.1f MHz", centre_ / 1e6).c_str());
            lv_obj_set_style_text_color(state_, c(kMuted), 0);
        }
    }

    void update_rows() {
        // Most recently heard first.
        std::vector<Hit> sorted = hits_;
        std::sort(sorted.begin(), sorted.end(), [](const Hit& a, const Hit& b) { return a.last > b.last; });
        for (int i = 0; i < kRows; ++i) {
            if (i >= int(sorted.size())) { lv_label_set_text(rows_[i], ""); continue; }
            uint32_t age = lv_tick_elaps(sorted[size_t(i)].last) / 1000;
            std::string when = age < 2 ? "now" : age < 60 ? fmt("%us ago", age) : fmt("%um ago", age / 60);
            lv_label_set_text(rows_[i], fmt("%.4f MHz      heard %d time%s      %s", sorted[size_t(i)].hz / 1e6,
                                            sorted[size_t(i)].count, sorted[size_t(i)].count == 1 ? "" : "s", when.c_str()).c_str());
        }
        if (hits_.empty()) lv_obj_remove_flag(empty_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(empty_, LV_OBJ_FLAG_HIDDEN);
    }

    RadioLink link_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *title_ = nullptr, *state_ = nullptr, *canvas_ = nullptr, *hint_ = nullptr, *empty_ = nullptr;
    lv_obj_t* rows_[kRows] = {};
    std::vector<uint16_t> pixels_;
    std::vector<int> bands_;
    std::vector<zrf::Memory> saved_;
    size_t saved_index_ = 0;
    std::string saved_mode_name_;
    Band saved_band_{};
    std::vector<Hit> hits_;
    std::vector<double> locked_;
    int band_ = 0, chunk_ = 0, settle_ = 0, ticks_ = 0;
    double centre_ = 0, channel_hz_ = 0;
    bool stopped_ = false, held_ = false;
    uint32_t last_signal_ = 0;
};
}  // namespace

std::unique_ptr<App> make_scanner_app() { return std::make_unique<ScannerApp>(); }

}  // namespace zs
