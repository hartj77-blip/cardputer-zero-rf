// zrf-sim-aprs: stands in for rtl_fm + multimon-ng when the radio is
// simulated. Prints APRS packets the way multimon-ng -A does. The callsigns
// are made up.
#include <cstdio>
#include <unistd.h>

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* packets[] = {
        "APRS: W1ABC-9>APDR16,WIDE1-1,WIDE2-1:=4143.50N/07242.10W>Heading to the club meeting",
        "APRS: K1XYZ>APRS,WIDE2-2:!4145.02N/07238.55W#PHG5360 W2, fill-in digipeater",
        "APRS: N1WX>APRS,WIDE1-1:@092345z4138.20N/07251.00W_090/005g010t052r000p000h65b10182",
        "APRS: KB1QRP-7>T1SUWV,WIDE1-1:`c5Pl <0x1c>[/>On the trail",
        "APRS: W1ABC-9>APDR16,WIDE1-1::K1XYZ    :Are you on 146.52 tonight?{07",
        "APRS: AA1HAM>APRS:>Monitoring 146.940 -600 PL 110.9",
        "APRS: W1ABC-9>APDR16,WIDE1-1,WIDE2-1:=4143.62N/07241.80W>Heading to the club meeting",
    };
    for (int n = 0;; ++n) {
        std::printf("%s\n", packets[n % 7]);
        usleep(400 * 1000);
    }
}
