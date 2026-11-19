// Spectrum: live spectrum trace over a waterfall, from the radio daemon.
//
// Left/Right tune by one step, Up/Down change the step, Z/X narrow or widen
// the view, G cycles gain, P toggles peak hold, C centres on the strongest
// signal, M cycles listening (off, broadcast FM, narrow FM, AM, upper and
// lower sideband, CW), S toggles squelch, B opens the band list, R records
// raw IQ to ~/Recordings. F follows a transceiver's dial (set the rig up in
// Station) and T sends this frequency to it. Type a frequency in MHz and
// press Enter to jump to it. Audio and recording stop on leaving.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "apps/common.h"
#include <sys/stat.h>

#include "apps/bands.h"
#include "apps/radio_link.h"
#include "apps/rig_link.h"
#include "ui/prefs.h"

namespace zs {
namespace {
using namespace theme;

const int kW = kScreenW;
const int kTopH = 18;                      // frequency line
const int kTraceH = 44;
const int kFallH = 74;
const int kCanvasH = kTraceH + kFallH;
const int kRangeDb = 60;                   // shown above the noise floor

const double kMinHz = 0.5e6, kMaxHz = 1766e6;   // RTL-SDR Blog V4 range
const double kSteps[] = {10, 100, 1e3, 10e3, 100e3, 1e6, 10e6};
const double kSpans[] = {250e3, 1.024e6, 2.048e6};   // sample rates an RTL-SDR accepts
const int kGains[] = {-1, 0, 10, 20, 30, 40};       // -1 = automatic
const char* const kModes[] = {"off", "wfm", "nfm", "am", "usb", "lsb", "cw"};
const char* const kModeNames[] = {"", "FM", "NFM", "AM", "USB", "LSB", "CW"};
const int kSquelchDb = 10;   // above the noise floor

uint16_t rgb565(int r, int g, int b) { return uint16_t((r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)); }
uint16_t rgb565(uint32_t hex) { return rgb565(hex >> 16 & 255, hex >> 8 & 255, hex & 255); }

std::string hz_text(double hz) {
    if (hz >= 1e6) return fmt("%g M", hz / 1e6);
    if (hz >= 1e3) return fmt("%g k", hz / 1e3);
    return fmt("%g Hz", hz);
}

class SpectrumApp : public App {
public:
    ~SpectrumApp() override {
        if (timer_) lv_timer_delete(timer_);
    }

    void build(lv_obj_t* root) override {
        freq_label_ = label(root, "", &lv_font_montserrat_14, kAccent);
        lv_obj_set_pos(freq_label_, 4, 0);
        info_label_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(info_label_, LV_ALIGN_TOP_RIGHT, -4, 2);

        pixels_.assign(size_t(kW) * kCanvasH, rgb565(kBg));
        canvas_ = lv_canvas_create(root);
        lv_canvas_set_buffer(canvas_, pixels_.data(), kW, kCanvasH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 0, kTopH);

        message_ = label(root, "", &lv_font_montserrat_14, kText);
        lv_obj_set_style_bg_color(message_, c(kSurface), 0);
        lv_obj_set_style_bg_opa(message_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(message_, 6, 0);
        lv_obj_set_style_radius(message_, 6, 0);
        lv_obj_align(message_, LV_ALIGN_CENTER, 0, 10);

        low_label_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(low_label_, LV_ALIGN_BOTTOM_LEFT, 4, -1);
        high_label_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(high_label_, LV_ALIGN_BOTTOM_RIGHT, -4, -1);
        hint_label_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(hint_label_, LV_ALIGN_BOTTOM_MID, 0, -1);

        // Blue -> cyan -> yellow -> white, weakest to strongest.
        for (int i = 0; i < 256; ++i) {
            float t = i / 255.0f;
            auto ramp = [t](float a, float b) { return int(255 * std::clamp((t - a) / (b - a), 0.0f, 1.0f)); };
            int blue = int(40 + 215 * std::clamp(t / 0.3f, 0.0f, 1.0f)) - ramp(0.5f, 0.8f);
            palette_[i] = rgb565(ramp(0.45f, 0.8f), ramp(0.15f, 0.65f), std::max(blue, ramp(0.85f, 1.0f)));
        }

        band_list_ = make_list(root, 60, kTopH, 200, kContentH - kTopH - 2);
        for (const Band& b : kBands) list_button(band_list_, nullptr, b.name, on_band_clicked, this);
        finish_list(band_list_);
        lv_obj_set_style_border_width(band_list_, 1, 0);
        lv_obj_set_style_border_color(band_list_, c(kAccent), 0);
        lv_obj_add_flag(band_list_, LV_OBJ_FLAG_HIDDEN);

        set_message("Starting radio...");
        update_labels();
        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<SpectrumApp*>(lv_timer_get_user_data(t))->pump(); },
                                 40, this);
        pump();
    }

    bool on_key(uint32_t key) override {
        // The band list takes the arrow keys and Enter while it is open.
        if (bands_open_) {
            if (key == 'b' || key == 'B') { show_bands(false); return true; }
            return key != LV_KEY_UP && key != LV_KEY_DOWN && key != LV_KEY_ENTER;
        }
        // Typing a frequency.
        if ((key >= '0' && key <= '9') || key == '.') {
            if (entry_.size() < 9) entry_ += char(key);
            update_labels();
            return true;
        }
        if (!entry_.empty()) {
            if (key == LV_KEY_BACKSPACE) {
                entry_.pop_back();
            } else if (key == LV_KEY_ENTER) {
                double mhz = std::atof(entry_.c_str());
                entry_.clear();
                if (mhz * 1e6 >= kMinHz && mhz * 1e6 <= kMaxHz) tune(mhz * 1e6);
                else shell_toast("Out of range (0.5 to 1766 MHz)");
            } else {
                return true;   // other keys do nothing while typing
            }
            update_labels();
            return true;
        }

        switch (key) {
        case LV_KEY_LEFT: tune(freq_ - kSteps[step_]); return true;
        case LV_KEY_RIGHT: tune(freq_ + kSteps[step_]); return true;
        case LV_KEY_UP: step_ = std::min(step_ + 1, int(std::size(kSteps)) - 1); break;
        case LV_KEY_DOWN: step_ = std::max(step_ - 1, 0); break;
        case 'z': case 'Z': set_span(span_ - 1); break;
        case 'x': case 'X': set_span(span_ + 1); break;
        case 'g': case 'G':
            gain_ = (gain_ + 1) % int(std::size(kGains));
            send_gain();
            break;
        case 'm': case 'M':
            // The first press picks the mode that suits this frequency: the
            // right sideband on an amateur band, AM on the airband, and so on.
            if (mode_ == 0 && !mode_picked_) {
                const char* m = suggested_mode(freq_);
                for (int i = 1; i < int(std::size(kModes)); ++i)
                    if (std::strcmp(kModes[i], m) == 0) mode_ = i;
                mode_picked_ = true;
            } else {
                mode_ = (mode_ + 1) % int(std::size(kModes));
            }
            client_.send(std::string("mode ") + kModes[mode_]);
            break;
        case 's': case 'S':
            squelch_ = !squelch_;
            send_squelch();
            shell_toast(squelch_ ? "Squelch on" : "Squelch off");
            break;
        case 'b': case 'B': show_bands(true); return true;
        case 'r': case 'R': toggle_record(); break;
        case 'f': case 'F':
            if (rig_.active()) {
                rig_.stop();
                shell_toast("Stopped following the rig");
            } else if (!RigLink::configured()) {
                shell_toast("Set up your rig in Station first");
            } else {
                rig_.start();
                shell_toast("Following the rig's dial");
            }
            break;
        case 't': case 'T':
            if (!rig_.active() || !rig_.connected()) shell_toast("Not connected to a rig (F to connect)");
            else { rig_.set_freq(freq_); shell_toast(fmt("Rig set to %.5f MHz", freq_ / 1e6)); }
            break;
        case LV_KEY_ENTER: return true;   // nothing to confirm; keep it from the hidden list
        case 'p': case 'P':
            peak_hold_ = !peak_hold_;
            std::fill(peak_.begin(), peak_.end(), 0);
            shell_toast(peak_hold_ ? "Peak hold on" : "Peak hold off");
            break;
        case 'c': case 'C':
            if (!last_.empty()) {
                int best = int(std::max_element(last_.begin(), last_.end()) - last_.begin());
                tune(freq_ + (best + 0.5 - kW / 2.0) * kSpans[span_] / kW);
            }
            return true;
        default: return false;
        }
        update_labels();
        return true;
    }

    bool on_back() override {
        if (bands_open_) { show_bands(false); return true; }
        if (entry_.empty()) return false;
        entry_.clear();
        update_labels();
        return true;
    }

private:
    // Runs every 40 ms: keep the connection up and draw what has arrived.
    void pump() {
        bool fresh, unavailable;
        // The rig's dial moves the display even while the radio is starting.
        double rig_hz = 0;
        bool rig_lost = false;
        if (rig_.poll(rig_hz, rig_lost) && std::fabs(rig_hz - freq_) >= 1) tune(rig_hz);
        if (rig_lost) {
            rig_.stop();
            shell_toast("Rig not answering. Check Station and the cable.");
            update_labels();
        }
        if (!link_.ensure(fresh, unavailable)) {
            if (unavailable) set_message("Radio service not available");
            return;
        }
        if (fresh) {
            // Station settings: crystal correction and power for an antenna amplifier.
            client_.send(fmt("ppm %d", prefs::get_int("rf.ppm", 0)));
            client_.send(prefs::get_int("rf.biastee", 0) ? "biastee on" : "biastee off");
            if (recording_) {
                // The radio service restarted; whatever was recording has stopped.
                recording_ = false;
                shell_toast("Recording stopped (radio restarted)");
                update_labels();
            }
            client_.send(fmt("rate %.0f", kSpans[span_]));
            send_gain();
            client_.send(fmt("tune %.0f", freq_));
            client_.send(fmt("spectrum %d 15", kW));
            send_squelch();
            if (mode_) client_.send(std::string("mode ") + kModes[mode_]);
        }

        zrf::RadioEvents ev;
        if (!client_.poll(RadioLink::poll_wait_ms(), ev)) {
            link_.lost();
            set_message("Radio stopped. Restarting...");
            return;
        }
        auto& frames = ev.frames;
        if (ev.has_error && ev.error != error_) {
            error_ = ev.error;
            if (error_.rfind("recording stopped", 0) == 0 || error_ == "can't write the recording") {
                recording_ = false;
                shell_toast(error_ == "recording stopped: storage full" ? "Recording stopped: storage full"
                            : error_ == "recording stopped: retuned" ? "Recording stopped (retuned)" : "Can't write the recording");
                update_labels();
                error_.clear();
            } else if (error_ == "audio output failed") shell_toast("No audio output");
            else if (error_ == "no dongle") set_message("No radio found.\nPlug in an SDR dongle.");
            else if (!error_.empty()) set_message(("Radio: " + error_).c_str());
        }
        bool drew = false;
        for (auto& f : frames) {
            // Frames still in flight from before a retune would smear the picture.
            if (int(f.cols.size()) != kW || std::fabs(f.freq - freq_) > 1 || std::fabs(f.rate - kSpans[span_]) > 1) continue;
            add_row(f.cols);
            drew = true;
        }
        if (drew) {
            // Strength of what is tuned in: the strongest of the middle few
            // columns against the noise floor (levels are 0.5 dB steps).
            uint8_t mid = *std::max_element(last_.begin() + kW / 2 - 3, last_.begin() + kW / 2 + 4);
            signal_db_ = (mid - floor_) / 2;
            if (mode_ && ++label_ticks_ % 8 == 0) update_labels();
            error_.clear();
            set_message("");
            draw_trace();
            lv_obj_invalidate(canvas_);
        }
    }

    void tune(double hz) {
        hz = std::clamp(hz, kMinHz, kMaxHz);
        if (hz == freq_) return;
        // Slide the picture so signals stay under their frequency.
        shift_columns(int(std::lround((freq_ - hz) / kSpans[span_] * kW)));
        freq_ = hz;
        client_.send(fmt("tune %.0f", freq_));
        update_labels();
        lv_obj_invalidate(canvas_);
    }

    void set_span(int index) {
        index = std::clamp(index, 0, int(std::size(kSpans)) - 1);
        if (index == span_) return;
        span_ = index;
        shift_columns(kW);   // the old picture is at the wrong scale: clear it
        client_.send(fmt("rate %.0f", kSpans[span_]));
        lv_obj_invalidate(canvas_);
    }

    void toggle_record() {
        if (recording_) {
            recording_ = false;
            client_.send("record off");
            shell_toast("Saved " + record_name_);
            return;
        }
        std::string dir = home_dir() + "/Recordings";
        mkdir(dir.c_str(), 0755);
        char when[32];
        time_t t = time(nullptr);
        tm lt{};
        localtime_r(&t, &lt);
        strftime(when, sizeof when, "%Y%m%d-%H%M%S", &lt);
        record_name_ = fmt("iq_%.4fMHz_%s.cu8", freq_ / 1e6, when);
        client_.send("record " + dir + "/" + record_name_);
        recording_ = true;
        shell_toast(fmt("Recording, %.1f MB a second", kSpans[span_] * 2 / 1e6));
    }

    void send_squelch() {
        client_.send(squelch_ ? fmt("squelch %d", kSquelchDb) : std::string("squelch off"));
    }

    void show_bands(bool on) {
        bands_open_ = on;
        if (on) {
            lv_obj_remove_flag(band_list_, LV_OBJ_FLAG_HIDDEN);
            lv_group_focus_obj(lv_obj_get_child(band_list_, 0));
        } else {
            lv_obj_add_flag(band_list_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    static void on_band_clicked(lv_event_t* e) {
        auto* self = static_cast<SpectrumApp*>(lv_event_get_user_data(e));
        if (!self->bands_open_) return;
        const Band& b = kBands[lv_obj_get_index(static_cast<lv_obj_t*>(lv_event_get_target(e)))];
        self->show_bands(false);
        self->set_span(b.span);
        self->tune(b.home);
        // A new band: the next M press picks its mode afresh; if already
        // listening, switch to it at once.
        self->mode_picked_ = false;
        if (self->mode_)
            for (int i = 1; i < int(std::size(kModes)); ++i)
                if (std::strcmp(kModes[i], b.mode) == 0) {
                    self->mode_ = i;
                    self->client_.send(std::string("mode ") + kModes[i]);
                }
        // Sideband and Morse want fine tuning steps.
        self->step_ = b.mode[0] == 'u' || b.mode[0] == 'l' || b.mode[0] == 'c' ? 2 : 4;
        self->update_labels();
    }

    void send_gain() {
        client_.send(kGains[gain_] < 0 ? std::string("gain auto") : fmt("gain %d", kGains[gain_]));
    }

    // Move the waterfall and held peaks dx pixels right (negative = left).
    void shift_columns(int dx) {
        if (dx == 0) return;
        const uint16_t bg = rgb565(kBg);
        std::vector<uint8_t> peak(kW, 0);
        for (int x = 0; x < kW; ++x)
            if (x - dx >= 0 && x - dx < kW) peak[size_t(x)] = peak_[size_t(x - dx)];
        peak_ = peak;
        last_.clear();
        for (int y = kTraceH; y < kCanvasH; ++y) {
            uint16_t* row = &pixels_[size_t(y) * kW];
            if (std::abs(dx) >= kW) { std::fill(row, row + kW, bg); continue; }
            if (dx > 0) {
                std::memmove(row + dx, row, size_t(kW - dx) * 2);
                std::fill(row, row + dx, bg);
            } else {
                std::memmove(row, row - dx, size_t(kW + dx) * 2);
                std::fill(row + kW + dx, row + kW, bg);
            }
        }
        std::fill(pixels_.begin(), pixels_.begin() + long(kW) * kTraceH, bg);
    }

    void add_row(const std::vector<uint8_t>& cols) {
        // Follow the noise floor slowly so the colours don't pump.
        std::vector<uint8_t> sorted = cols;
        std::nth_element(sorted.begin(), sorted.begin() + kW / 4, sorted.end());
        float floor_now = sorted[kW / 4];
        floor_ = floor_ < 0 ? floor_now : floor_ * 0.9f + floor_now * 0.1f;

        uint16_t* fall = &pixels_[size_t(kTraceH) * kW];
        std::memmove(fall + kW, fall, size_t(kW) * (kFallH - 1) * 2);
        for (int x = 0; x < kW; ++x) fall[x] = palette_[size_t(level(cols[size_t(x)]) * 255)];
        last_ = cols;
        for (int x = 0; x < kW; ++x) peak_[size_t(x)] = std::max(peak_[size_t(x)], cols[size_t(x)]);
    }

    // 0..1 within the shown range above the noise floor. Levels are 0.5 dB steps.
    float level(uint8_t v) const {
        return std::clamp((v - (floor_ - 6)) / float(kRangeDb * 2), 0.0f, 1.0f);
    }

    void draw_trace() {
        const uint16_t bg = rgb565(kBg), fill = rgb565(0x12384a), line = rgb565(0x39c5cf);
        const uint16_t centre = rgb565(kSurface2), held = rgb565(kAccent);
        for (int x = 0; x < kW; ++x) {
            int top = int((1 - level(last_[size_t(x)])) * (kTraceH - 1));
            for (int y = 0; y < kTraceH; ++y)
                pixels_[size_t(y) * kW + x] = y < top ? (x == kW / 2 ? centre : bg) : y == top ? line : fill;
            if (peak_hold_) {
                int p = int((1 - level(peak_[size_t(x)])) * (kTraceH - 1));
                if (p < top) pixels_[size_t(p) * kW + x] = held;
            }
        }
    }

    void set_message(const char* text) {
        lv_label_set_text(message_, text);
        if (*text) lv_obj_remove_flag(message_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(message_, LV_OBJ_FLAG_HIDDEN);
    }

    void update_labels() {
        if (!entry_.empty()) {
            lv_label_set_text(freq_label_, ("Go to " + entry_ + "_ MHz").c_str());
        } else {
            lv_label_set_text(freq_label_, fmt("%.5f MHz", freq_ / 1e6).c_str());
        }
        std::string gain = kGains[gain_] < 0 ? "auto" : fmt("%d dB", kGains[gain_]);
        std::string info = fmt("step %s", hz_text(kSteps[step_]).c_str());
        if (mode_) info += fmt("  " LV_SYMBOL_VOLUME_MAX " %s%s", kModeNames[mode_], squelch_ ? " SQ" : "");
        else info += fmt("  gain %s", gain.c_str());
        if (recording_) info = LV_SYMBOL_SAVE " REC  " + info;
        if (rig_.active()) info = LV_SYMBOL_LOOP " " + info;
        lv_label_set_text(info_label_, info.c_str());
        double half = kSpans[span_] / 2;
        lv_label_set_text(low_label_, fmt("%.3f", (freq_ - half) / 1e6).c_str());
        lv_label_set_text(high_label_, fmt("%.3f", (freq_ + half) / 1e6).c_str());
        // Bottom line: the band we're in, then the signal strength while
        // listening or the key hints otherwise.
        const Band* band = band_at(freq_);
        std::string hint = band ? std::string(band->name) + "   " : "";
        if (!entry_.empty()) hint = "Enter go   Esc cancel";
        else if (mode_ && signal_db_ > -100) hint += fmt("signal %+.0f dB", signal_db_);
        else hint += band ? "B bands  M listen" : "0-9 go to  B bands  M listen";
        lv_label_set_text(hint_label_, hint.c_str());
    }

    RadioLink link_;
    zrf::RadioClient& client_ = link_.client;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *canvas_ = nullptr, *freq_label_ = nullptr, *info_label_ = nullptr, *message_ = nullptr;
    lv_obj_t *low_label_ = nullptr, *high_label_ = nullptr, *hint_label_ = nullptr;
    std::vector<uint16_t> pixels_;
    uint16_t palette_[256] = {};
    std::vector<uint8_t> last_, peak_ = std::vector<uint8_t>(kW, 0);
    float floor_ = -1;
    double freq_ = 98.3e6;
    int step_ = 4, span_ = 1, gain_ = 0, mode_ = 0;
    bool mode_picked_ = false;
    float signal_db_ = -200;   // tuned signal above the noise floor
    int label_ticks_ = 0;
    RigLink rig_;
    bool squelch_ = false, bands_open_ = false, recording_ = false;
    std::string record_name_;
    lv_obj_t* band_list_ = nullptr;
    bool peak_hold_ = false;
    std::string entry_, error_;
};
}  // namespace

std::unique_ptr<App> make_spectrum_app() { return std::make_unique<SpectrumApp>(); }

}  // namespace zs
