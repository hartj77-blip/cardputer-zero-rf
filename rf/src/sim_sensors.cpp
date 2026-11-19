// zrf-sim-sensors: stands in for rtl_433 when the radio is simulated. Prints
// the kind of JSON lines rtl_433 prints, a few a second, forever.
#include <cmath>
#include <cstdio>
#include <unistd.h>

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    for (int n = 0;; ++n) {
        double drift = std::sin(n / 40.0);
        switch (n % 5) {
        case 0:
            std::printf("{\"time\":\"2026-01-01 12:00:%02d\",\"model\":\"Acurite-Tower\",\"id\":11524,\"channel\":\"A\","
                        "\"battery_ok\":1,\"temperature_C\":%.1f,\"humidity\":%d,\"mic\":\"CHECKSUM\","
                        "\"mod\":\"ASK\",\"freq\":433.94,\"rssi\":-4.2,\"snr\":24.1,\"noise\":-28.3}\n",
                        n % 60, 21.3 + drift, 45 + int(drift * 3));
            break;
        case 1:
            std::printf("{\"time\":\"2026-01-01 12:00:%02d\",\"model\":\"LaCrosse-TX141THBv2\",\"id\":168,\"channel\":0,"
                        "\"battery_ok\":0,\"temperature_C\":%.1f,\"humidity\":%d,\"test\":\"No\",\"mic\":\"CRC\","
                        "\"mod\":\"ASK\",\"freq\":433.90,\"rssi\":-11.0,\"snr\":17.3,\"noise\":-28.3}\n",
                        n % 60, 4.8 - drift, 81);
            break;
        case 2:
            std::printf("{\"time\":\"2026-01-01 12:00:%02d\",\"model\":\"Toyota\",\"type\":\"TPMS\",\"id\":\"f1b3a2c7\","
                        "\"status\":128,\"pressure_kPa\":%.1f,\"temperature_C\":%.0f,\"mic\":\"CRC\","
                        "\"mod\":\"FSK\",\"freq1\":433.88,\"freq2\":433.96,\"rssi\":-9.5,\"snr\":18.8,\"noise\":-28.3}\n",
                        n % 60, 228.5 + drift, 18.0);
            break;
        case 3:
            std::printf("{\"time\":\"2026-01-01 12:00:%02d\",\"model\":\"Generic-Remote\",\"id\":43011,\"cmd\":%d,"
                        "\"tristate\":\"0F0FF00F1010\",\"mod\":\"ASK\",\"freq\":433.92,\"rssi\":-1.8,\"snr\":26.5,\"noise\":-28.3}\n",
                        n % 60, 1 + (n / 5) % 4);
            break;
        default:
            std::printf("{\"time\":\"2026-01-01 12:00:%02d\",\"model\":\"Fineoffset-WH24\",\"id\":92,\"battery_ok\":1,"
                        "\"temperature_C\":%.1f,\"humidity\":%d,\"wind_dir_deg\":212,\"wind_avg_m_s\":%.1f,"
                        "\"wind_max_m_s\":4.2,\"rain_mm\":12.3,\"uv\":310,\"uvi\":1,\"light_lux\":18700.0,\"mic\":\"CRC\","
                        "\"mod\":\"FSK\",\"freq1\":433.90,\"freq2\":433.96,\"rssi\":-14.6,\"snr\":13.7,\"noise\":-28.3}\n",
                        n % 60, 9.6 + drift, 72, 2.1 + drift);
        }
        usleep(300 * 1000);
    }
}
