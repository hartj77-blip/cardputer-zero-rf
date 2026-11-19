#include "aprs.h"

#include <cctype>
#include <cmath>
#include <cstdlib>

namespace zrf {
namespace {

bool digits(const std::string& s, size_t at, size_t n) {
    if (at + n > s.size()) return false;
    for (size_t i = 0; i < n; ++i)
        if (!std::isdigit(static_cast<unsigned char>(s[at + i])) && s[at + i] != ' ') return false;
    return true;
}

// "DDMM.mmN" + symbol table + "DDDMM.mmW" + symbol, 19 characters.
bool plain_position(const std::string& s, size_t at, AprsPacket& p) {
    if (at + 19 > s.size()) return false;
    if (!digits(s, at, 4) || s[at + 4] != '.' || !digits(s, at + 5, 2)) return false;
    if (!digits(s, at + 9, 5) || s[at + 14] != '.' || !digits(s, at + 15, 2)) return false;
    char ns = s[at + 7], ew = s[at + 17];
    if ((ns != 'N' && ns != 'S') || (ew != 'E' && ew != 'W')) return false;
    // Spaces stand for digits left out to blur the position; read them as 0.
    auto num = [&](size_t a, size_t n) {
        std::string t = s.substr(a, n);
        for (char& c : t)
            if (c == ' ') c = '0';
        return std::atof(t.c_str());
    };
    p.lat = num(at, 2) + num(at + 2, 5) / 60;
    p.lon = num(at + 9, 3) + num(at + 12, 5) / 60;
    if (ns == 'S') p.lat = -p.lat;
    if (ew == 'W') p.lon = -p.lon;
    p.has_position = true;
    p.text = s.substr(at + 19);
    return true;
}

// Symbol table + 4 latitude + 4 longitude characters in base 91 + symbol + 3 more.
bool compressed_position(const std::string& s, size_t at, AprsPacket& p) {
    if (at + 13 > s.size()) return false;
    double v[2] = {0, 0};
    for (int k = 0; k < 2; ++k)
        for (int i = 0; i < 4; ++i) {
            int c = static_cast<unsigned char>(s[at + 1 + size_t(k) * 4 + size_t(i)]) - 33;
            if (c < 0 || c > 90) return false;
            v[k] = v[k] * 91 + c;
        }
    p.lat = 90 - v[0] / 380926;
    p.lon = -180 + v[1] / 190463;
    p.has_position = true;
    p.text = s.substr(at + 13);
    return true;
}

bool position_at(const std::string& s, size_t at, AprsPacket& p) {
    if (at >= s.size()) return false;
    return std::isdigit(static_cast<unsigned char>(s[at])) ? plain_position(s, at, p) : compressed_position(s, at, p);
}

}  // namespace

bool parse_aprs(const std::string& line_in, AprsPacket& p) {
    p = AprsPacket();
    std::string line = line_in;
    if (line.rfind("APRS: ", 0) == 0) line = line.substr(6);
    size_t gt = line.find('>'), colon = line.find(':');
    if (gt == std::string::npos || colon == std::string::npos || gt == 0 || gt > colon) return false;
    p.source = line.substr(0, gt);
    if (!looks_like_callsign(p.source)) return false;
    std::string route = line.substr(gt + 1, colon - gt - 1);
    size_t comma = route.find(',');
    p.destination = route.substr(0, comma);
    p.path = comma == std::string::npos ? "" : route.substr(comma + 1);
    p.info = line.substr(colon + 1);
    if (p.info.empty()) return false;

    const std::string& s = p.info;
    p.kind = "other";
    p.text = s.substr(1);
    switch (s[0]) {
    case '!': case '=':
        p.kind = "position";
        position_at(s, 1, p);
        break;
    case '/': case '@':   // with a 7-character time first
        p.kind = "position";
        position_at(s, 8, p);
        break;
    case ':':
        if (s.size() >= 11 && s[10] == ':') {
            p.kind = "message";
            p.addressee = s.substr(1, 9);
            while (!p.addressee.empty() && p.addressee.back() == ' ') p.addressee.pop_back();
            p.text = s.substr(11);
            size_t brace = p.text.rfind('{');   // message number, not for reading
            if (brace != std::string::npos) p.text.erase(brace);
        }
        break;
    case '>': p.kind = "status"; break;
    case '_': p.kind = "weather"; break;
    case '`': case '\'': p.kind = "Mic-E"; break;   // position packed into the address; not decoded yet
    case ';': case ')': p.kind = "object"; break;
    case 'T': p.kind = "telemetry"; break;
    default: break;
    }
    // A position report whose comment starts with weather data.
    if (p.kind == "position" && p.has_position && p.text.size() > 7 && p.text[3] == '/' && p.text[7] == 'g') p.kind = "weather";
    return true;
}

bool grid_to_latlon(const std::string& g, double& lat, double& lon) {
    if (g.size() != 4 && g.size() != 6) return false;
    auto up = [](char c) { return char(std::toupper(static_cast<unsigned char>(c))); };
    char a = up(g[0]), b = up(g[1]);
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R') return false;
    if (!std::isdigit(static_cast<unsigned char>(g[2])) || !std::isdigit(static_cast<unsigned char>(g[3]))) return false;
    lon = -180 + (a - 'A') * 20 + (g[2] - '0') * 2;
    lat = -90 + (b - 'A') * 10 + (g[3] - '0');
    if (g.size() == 6) {
        char c = up(g[4]), d = up(g[5]);
        if (c < 'A' || c > 'X' || d < 'A' || d > 'X') return false;
        lon += (c - 'A') * (2.0 / 24) + 1.0 / 24;
        lat += (d - 'A') * (1.0 / 24) + 0.5 / 24;
    } else {
        lon += 1;
        lat += 0.5;
    }
    return true;
}

void distance_bearing(double lat1, double lon1, double lat2, double lon2, double& km, double& bearing) {
    const double rad = M_PI / 180;
    double p1 = lat1 * rad, p2 = lat2 * rad, dl = (lon2 - lon1) * rad, dp = p2 - p1;
    double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    km = 6371.0 * 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
    bearing = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) / rad;
    if (bearing < 0) bearing += 360;
}

bool looks_like_callsign(const std::string& s) {
    if (s.size() < 3 || s.size() > 12) return false;
    bool letter = false, digit = false;
    for (char c : s) {
        if (std::isalpha(static_cast<unsigned char>(c))) letter = true;
        else if (std::isdigit(static_cast<unsigned char>(c))) digit = true;
        else if (c != '-' && c != '/') return false;
    }
    return letter && digit;
}

}  // namespace zrf
