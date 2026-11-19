#include "adif.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace zrf {
namespace {

struct BandEdge { const char* name; double lo, hi; };
// Wide enough for every region's allocation.
const BandEdge kAdifBands[] = {
    {"160m", 1.8, 2.0},   {"80m", 3.5, 4.0},     {"60m", 5.06, 5.45},   {"40m", 7.0, 7.3},     {"30m", 10.1, 10.15},
    {"20m", 14.0, 14.35}, {"17m", 18.068, 18.168}, {"15m", 21.0, 21.45},  {"12m", 24.89, 24.99}, {"10m", 28.0, 29.7},
    {"6m", 50, 54},       {"2m", 144, 148},      {"1.25m", 222, 225},   {"70cm", 420, 450},    {"33cm", 902, 928},
    {"23cm", 1240, 1300},
};

void field(std::string& out, const char* name, const std::string& value) {
    if (value.empty()) return;
    out += "<" + std::string(name) + ":" + std::to_string(value.size()) + ">" + value + " ";
}

std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

std::string adif_band(double mhz) {
    for (const BandEdge& b : kAdifBands)
        if (mhz >= b.lo && mhz <= b.hi) return b.name;
    return "";
}

std::string adif_header() {
    return "ZeroOS RF logbook\n<ADIF_VER:5>3.1.4 <PROGRAMID:9>ZeroOS RF <EOH>\n";
}

std::string adif_record(const Qso& q) {
    std::string out;
    field(out, "CALL", upper(q.call));
    field(out, "QSO_DATE", q.date);
    field(out, "TIME_ON", q.time);
    if (q.freq_mhz > 0) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.5f", q.freq_mhz);
        field(out, "FREQ", buf);
        field(out, "BAND", adif_band(q.freq_mhz));
    }
    // ADIF files sideband as a kind of SSB. A digital contact with no named
    // mode goes under "DATA", with our own tag so it reads back as DIGI.
    std::string mode = upper(q.mode);
    if (mode == "USB" || mode == "LSB") {
        field(out, "MODE", "SSB");
        field(out, "SUBMODE", mode);
    } else if (mode == "DIGI") {
        field(out, "MODE", "DATA");
        field(out, "APP_ZEROOS_MODE", "DIGI");
    } else {
        field(out, "MODE", mode);
    }
    field(out, "RST_SENT", q.rst_sent);
    field(out, "RST_RCVD", q.rst_rcvd);
    field(out, "COMMENT", q.comment);
    field(out, "STATION_CALLSIGN", upper(q.my_call));
    field(out, "MY_GRIDSQUARE", q.my_grid);
    return out + "<EOR>\n";
}

std::vector<Qso> adif_parse(const std::string& text) {
    std::vector<Qso> out;
    Qso q;
    std::string mode, submode, app_mode;
    size_t i = 0;
    // A file that starts with '<' has no header.
    size_t eoh = std::string::npos;
    for (size_t k = 0; k + 4 < text.size(); ++k)
        if (text[k] == '<' && upper(text.substr(k + 1, 4)) == "EOH>") { eoh = k + 5; break; }
    if (!text.empty() && text[0] != '<' && eoh != std::string::npos) i = eoh;

    while (i < text.size()) {
        size_t lt = text.find('<', i);
        if (lt == std::string::npos) break;
        size_t gt = text.find('>', lt);
        if (gt == std::string::npos) break;
        std::string tag = text.substr(lt + 1, gt - lt - 1);
        i = gt + 1;
        std::string name = upper(tag.substr(0, tag.find(':')));
        if (name == "EOH") continue;
        if (name == "EOR") {
            if (!submode.empty()) q.mode = submode;
            else if (!app_mode.empty()) q.mode = app_mode;
            else q.mode = mode;
            if (!q.call.empty()) out.push_back(q);
            q = Qso();
            mode.clear(); submode.clear(); app_mode.clear();
            continue;
        }
        size_t colon = tag.find(':');
        if (colon == std::string::npos) continue;
        size_t len = size_t(std::atoi(tag.c_str() + colon + 1));
        if (i + len > text.size()) break;   // cut-off file: keep what was complete
        std::string value = text.substr(i, len);
        i += len;
        if (name == "CALL") q.call = upper(value);
        else if (name == "QSO_DATE") q.date = value;
        else if (name == "TIME_ON") q.time = value;
        else if (name == "FREQ") q.freq_mhz = std::atof(value.c_str());
        else if (name == "MODE") mode = upper(value);
        else if (name == "SUBMODE") submode = upper(value);
        else if (name == "APP_ZEROOS_MODE") app_mode = upper(value);
        else if (name == "RST_SENT") q.rst_sent = value;
        else if (name == "RST_RCVD") q.rst_rcvd = value;
        else if (name == "COMMENT") q.comment = value;
        else if (name == "STATION_CALLSIGN") q.my_call = value;
        else if (name == "MY_GRIDSQUARE") q.my_grid = value;
    }
    return out;
}

}  // namespace zrf
