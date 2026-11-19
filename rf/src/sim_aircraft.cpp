// zrf-sim-aircraft: stands in for rtl_adsb / dump1090 when the radio is
// simulated. Prints valid ADS-B frames for a few aircraft flying straight
// lines, a few a second, forever.
#include <cmath>
#include <cstdio>
#include <unistd.h>

#include "adsb.h"

struct Plane {
    unsigned icao;
    const char* callsign;
    double lat, lon, speed_kt, heading;
    int alt, climb;
};

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    Plane planes[] = {
        {0x4840D6, "KLM1023", 52.31, 4.76, 250, 265, 4200, 1856},
        {0xA05F21, "UAL924", 51.90, 3.10, 478, 92, 37000, 0},
        {0x3C6444, "DLH5YA", 52.80, 5.90, 441, 215, 34000, -64},
        {0x406B90, "EZY83JK", 51.60, 4.20, 310, 12, 18350, -1408},
        {0x4CA2D1, "", 52.10, 3.60, 402, 130, 39000, 0},   // sends no callsign
    };
    const double dt = 0.25;
    for (int n = 0;; ++n) {
        Plane& p = planes[n % 5];
        // Each visit advances this aircraft by the time since its last one.
        double step = dt * 5, nm = p.speed_kt * step / 3600;
        p.lat += nm / 60 * std::cos(p.heading * M_PI / 180);
        p.lon += nm / 60 * std::sin(p.heading * M_PI / 180) / std::cos(p.lat * M_PI / 180);
        p.alt += int(p.climb * step / 60);
        int kind = (n / 5) % 4;
        std::string f;
        if (kind == 0) f = zrf::adsb_position_frame(p.icao, p.alt / 25 * 25, p.lat, p.lon, false);
        else if (kind == 1) f = zrf::adsb_position_frame(p.icao, p.alt / 25 * 25, p.lat, p.lon, true);
        else if (kind == 2) f = zrf::adsb_velocity_frame(p.icao, p.speed_kt, p.heading, p.climb);
        else if (*p.callsign) f = zrf::adsb_ident_frame(p.icao, p.callsign);
        if (!f.empty()) std::printf("%s\n", f.c_str());
        usleep(useconds_t(dt * 1e6));
    }
}
