// FT8: the weak-signal digital mode. Every 15 seconds the stations heard on
// the band's FT8 frequency are listed with their signal strength, audio
// frequency and message. Receive only.
//
// Left/Right change band, Up/Down scroll back, C clears. Stations calling CQ
// are highlighted, messages that mention your callsign more so, and a grid
// in a message is turned into a distance from yours (both set in Station).
// The device's clock must be right to within about a second.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <vector>

#include "apps/bands.h"
#include "apps/background_radio.h"
#include "apps/common.h"
#include "apps/radio_link.h"
#include "apps/rf_state.h"
#include "aprs.h"
#include "ui/prefs.h"

namespace zs {
namespace {
using namespace theme;

struct Dial { const char* band; double hz; };
// The customary FT8 dial frequencies (upper sideband).
const Dial kDials[] = {
    {"160 m", 1.840e6}, {"80 m", 3.573e6}, {"60 m", 5.357e6}, {"40 m", 7.074e6}, {"30 m", 10.136e6}, {"20 m", 14.074e6},
    {"17 m", 18.100e6}, {"15 m", 21.074e6}, {"12 m", 24.915e6}, {"10 m", 28.074e6}, {"6 m", 50.313e6}, {"2 m", 144.174e6},
};
const int kRows = 7, kRowH = 16, kTopH = 18;
const size_t kKeep = 80;
// Column left edges: time, dB, audio Hz, message.
const int kCol[] = {6, 46, 76, 116};

struct Decode {
    std::string time, text, extra;   // extra: distance, when the message carries a grid
    int snr = 0, hz = 0;
    uint32_t color = kText;
};

bool is_grid(const std::string& s) {
    return s.size() == 4 && s[0] >= 'A' && s[0] <= 'R' && s[1] >= 'A' && s[1] <= 'R' && std::isdigit(static_cast<unsigned char>(s[2])) &&
           std::isdigit(static_cast<unsigned char>(s[3])) && s != "RR73";
}

class Ft8App : public App {
public:
    ~Ft8App() override {
        if (timer_) lv_timer_delete(timer_);
    }

    void build(lv_obj_t* root) override {
        BackgroundRadio::get().stop();   // this app needs the receiver
        my_call_ = prefs::get_str("rf.callsign", "");
        have_home_ = zrf::grid_to_latlon(prefs::get_str("rf.grid", ""), home_lat_, home_lon_);
        // Start on the band Spectrum was last on, if it has an FT8 frequency.
        band_ = 5;   // 20 m
        if (const Band* b = band_at(g_rf_last_hz))
            for (int i = 0; i < int(std::size(kDials)); ++i)
                if (std::string(kDials[i].band) == b->name) band_ = i;

        title_ = label(root, "", &lv_font_montserrat_14, kAccent);
        lv_obj_set_pos(title_, 4, 0);
        status_ = label(root, "", &lv_font_montserrat_12, kMuted);
        lv_obj_align(status_, LV_ALIGN_TOP_RIGHT, -4, 2);
        for (int r = 0; r < kRows; ++r)
            for (int c = 0; c < 4; ++c) {
                cells_[r][c] = label(root, "", &lv_font_montserrat_12, kText);
                lv_obj_set_pos(cells_[r][c], kCol[c], kTopH + r * kRowH);
            }
        hint_ = label(root, LV_SYMBOL_LEFT LV_SYMBOL_RIGHT " band   " LV_SYMBOL_UP LV_SYMBOL_DOWN " scroll   C clear",
                      &lv_font_montserrat_12, kMuted);
        lv_obj_align(hint_, LV_ALIGN_BOTTOM_MID, 0, -1);
        message_ = label(root, "", &lv_font_montserrat_14, kMuted);
        lv_obj_set_style_text_align(message_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(message_, LV_ALIGN_CENTER, 0, 4);
        set_message("Starting radio...");
        update_title();
        timer_ = lv_timer_create([](lv_timer_t* t) { static_cast<Ft8App*>(lv_timer_get_user_data(t))->pump(); }, 40, this);
        pump();
    }

    void tick() override {
        if (++ticks_ % 5 == 0) update_status();
    }

    bool on_key(uint32_t key) override {
        int n = int(log_.size());
        switch (key) {
        case LV_KEY_LEFT: set_band(band_ + int(std::size(kDials)) - 1); return true;
        case LV_KEY_RIGHT: set_band(band_ + 1); return true;
        case LV_KEY_UP: back_ = std::min(back_ + 1, std::max(0, n - kRows)); refresh(); return true;
        case LV_KEY_DOWN: back_ = std::max(back_ - 1, 0); refresh(); return true;
        case 'c': case 'C': log_.clear(); back_ = 0; slots_ = 0; refresh(); set_message(waiting()); return true;
        case LV_KEY_ENTER: return true;
        default: return false;
        }
    }

private:
    const char* waiting() const { return "Listening...\nDecodes arrive every 15 seconds"; }

    void set_band(int i) {
        band_ = i % int(std::size(kDials));
        link_.client.send(fmt("tune %.0f", kDials[band_].hz));
        log_.clear();
        back_ = slots_ = 0;
        refresh();
        set_message(waiting());
        update_title();
    }

    void pump() {
        bool fresh, unavailable;
        if (!link_.ensure(fresh, unavailable)) {
            if (unavailable) set_message("Radio service not available");
            return;
        }
        if (fresh) {
            link_.client.send(fmt("ppm %d", prefs::get_int("rf.ppm", 0)));
            link_.client.send("rate 250000");   // FT8 needs 3 kHz; the narrowest rate costs least
            link_.client.send("gain auto");
            link_.client.send(fmt("tune %.0f", kDials[band_].hz));
            link_.client.send("decode ft8");
            if (log_.empty()) set_message(waiting());
        }
        zrf::RadioEvents ev;
        if (!link_.client.poll(RadioLink::poll_wait_ms(), ev)) {
            link_.lost();
            set_message("Radio stopped. Restarting...");
            return;
        }
        if (ev.has_error) {
            if (ev.error == "no dongle") set_message("No radio found.\nPlug in an SDR dongle.");
            else if (log_.empty()) set_message(("Radio: " + ev.error).c_str());
        }
        bool changed = false;
        for (auto& line : ev.decoded) changed = add_line(line) || changed;
        if (changed) refresh();
    }

    // "FT8 HHMMSS <dB> <dt> <Hz> <message>" or "FT8 HHMMSS done <count>".
    bool add_line(const std::string& line) {
        std::istringstream is(line);
        std::string tag, time, third;
        if (!(is >> tag >> time >> third) || tag != "FT8" || time.size() != 6) return false;
        if (third == "done") {
            ++slots_;
            int n = 0;
            is >> n;
            if (log_.empty() && n == 0) set_message("Nothing heard in the last slot.\nIs the band open? Is the clock right?");
            return false;
        }
        Decode d;
        double dt = 0;
        d.snr = std::atoi(third.c_str());
        if (!(is >> dt >> d.hz)) return false;
        std::getline(is, d.text);
        while (!d.text.empty() && d.text[0] == ' ') d.text.erase(0, 1);
        d.time = time.substr(2, 2) + ":" + time.substr(4, 2);

        // What kind of message, for colour; and how far away, if it says where.
        if (d.text.rfind("CQ ", 0) == 0) d.color = kAccent;
        if (!my_call_.empty() && (" " + d.text + " ").find(" " + my_call_ + " ") != std::string::npos) d.color = kGood;
        std::string last = d.text.substr(d.text.rfind(' ') + 1);
        double lat, lon;
        if (have_home_ && is_grid(last) && zrf::grid_to_latlon(last, lat, lon)) {
            double km, bearing;
            zrf::distance_bearing(home_lat_, home_lon_, lat, lon, km, bearing);
            d.extra = fmt("  %.0f km", km);
        }
        log_.push_back(std::move(d));
        if (log_.size() > kKeep) log_.erase(log_.begin());
        if (back_ > 0) ++back_;   // keep the view where the reader left it
        back_ = std::min(back_, std::max(0, int(log_.size()) - kRows));
        set_message("");
        return true;
    }

    void refresh() {
        // The newest decode sits on the bottom row, as in other FT8 programs.
        int n = int(log_.size());
        int first = std::max(0, n - kRows - back_);
        for (int r = 0; r < kRows; ++r) {
            int i = first + r;
            if (i >= n) {
                for (auto* c : cells_[r]) lv_label_set_text(c, "");
                continue;
            }
            const Decode& d = log_[size_t(i)];
            std::string text[4] = {d.time, fmt("%+d", d.snr), fmt("%d", d.hz), d.text + d.extra};
            for (int c = 0; c < 4; ++c) {
                lv_label_set_text(cells_[r][c], text[c].c_str());
                lv_obj_set_style_text_color(cells_[r][c], theme::c(c == 3 ? d.color : kMuted), 0);
            }
        }
        update_status();
    }

    void update_status() {
        double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        int left = 15 - int(std::fmod(now, 15.0));
        std::string s = fmt("next in %2d s", left);
        if (back_ > 0) s = fmt("%d newer " LV_SYMBOL_DOWN "   ", back_) + s;
        lv_label_set_text(status_, s.c_str());
    }

    void update_title() {
        lv_label_set_text(title_, fmt("FT8  %s  %.3f MHz", kDials[band_].band, kDials[band_].hz / 1e6).c_str());
    }

    void set_message(const char* text) {
        lv_label_set_text(message_, text);
        if (*text) lv_obj_remove_flag(message_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(message_, LV_OBJ_FLAG_HIDDEN);
    }

    RadioLink link_;
    lv_timer_t* timer_ = nullptr;
    lv_obj_t *title_ = nullptr, *status_ = nullptr, *hint_ = nullptr, *message_ = nullptr;
    lv_obj_t* cells_[kRows][4] = {};
    std::vector<Decode> log_;
    std::string my_call_;
    bool have_home_ = false;
    double home_lat_ = 0, home_lon_ = 0;
    int band_ = 5, back_ = 0, slots_ = 0, ticks_ = 0;
};
}  // namespace

std::unique_ptr<App> make_ft8_app() { return std::make_unique<Ft8App>(); }

}  // namespace zs
