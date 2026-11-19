// Unit tests for the DSP and source layers. Exits non-zero on the first failure.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "adsb.h"
#include "dsp/demod.h"
#include "dsp/spectrum.h"
#include "json_flat.h"
#include "proto.h"
#include "sdr/source.h"

using namespace zrf;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

static size_t argmax(const std::vector<float>& v) {
    size_t best = 0;
    for (size_t i = 1; i < v.size(); ++i)
        if (v[i] > v[best]) best = i;
    return best;
}

static std::vector<cf32> tone(size_t n, double cycles_per_sample, float amp) {
    std::vector<cf32> v(n);
    for (size_t i = 0; i < n; ++i) {
        double a = 2 * M_PI * cycles_per_sample * double(i);
        v[i] = cf32(amp * float(std::cos(a)), amp * float(std::sin(a)));
    }
    return v;
}

static void test_fft() {
    const size_t n = 64;
    Fft fft(n);
    auto v = tone(n, 5.0 / n, 1.0f);
    fft.run(v.data());
    for (size_t i = 0; i < n; ++i) CHECK(std::fabs(std::abs(v[i]) - (i == 5 ? float(n) : 0.0f)) < 1e-2f);
}

static void test_spectrum_level_and_position() {
    const size_t n = 1024;
    Spectrum sp(n);
    CHECK(sp.take().empty());
    // Full-scale tone 100 bins above centre reads 0 dB at bin n/2 + 100.
    auto v = tone(n, 100.0 / n, 1.0f);
    sp.add(v.data());
    auto out = sp.take();
    CHECK(out.size() == n);
    CHECK(argmax(out) == n / 2 + 100);
    CHECK(std::fabs(out[n / 2 + 100]) < 0.1f);
    CHECK(out[n / 2 - 100] < -80.0f);
    // Negative frequencies land below centre; half amplitude is 6 dB down.
    v = tone(n, -200.0 / n, 0.5f);
    sp.add(v.data());
    out = sp.take();
    CHECK(argmax(out) == n / 2 - 200);
    CHECK(std::fabs(out[n / 2 - 200] + 6.02f) < 0.1f);
}

static void test_reduce_peak() {
    std::vector<float> in(1024, -100.0f);
    in[517] = -10.0f;   // one narrow signal must survive reduction
    auto out = reduce_peak(in, 320);
    CHECK(out.size() == 320);
    CHECK(argmax(out) == 517 * 320 / 1024);
    CHECK(out[argmax(out)] == -10.0f);
    CHECK(reduce_peak(in, 1024) == in);
}

static void test_proto() {
    CHECK(db_to_byte(-200) == 0 && db_to_byte(50) == 255);
    CHECK(std::fabs(byte_to_db(db_to_byte(-42.5f)) + 42.5f) < 0.01f);
    std::vector<uint8_t> bytes = {0, 1, 0x7f, 0xff}, back;
    CHECK(to_hex(bytes) == "00017fff");
    CHECK(from_hex("00017fff", back) && back == bytes);
    CHECK(!from_hex("0g", back) && !from_hex("abc", back));
}

// The simulated FM station at 98.3 MHz shows up where the tuning says it should.
static void test_sim_source() {
    std::string err;
    auto src = make_source("sim", err);
    CHECK(src && src->open(err));
    const double rate = 1.024e6;
    const size_t n = 1024;
    CHECK(src->set_rate(rate) && src->set_gain(std::nullopt));
    CHECK(!src->set_rate(1.0));
    Spectrum sp(n);
    std::vector<cf32> block(n);
    for (double tuned : {98.3e6, 98.2e6}) {
        CHECK(src->set_freq(tuned));
        for (int i = 0; i < 8; ++i) {
            CHECK(src->read(block.data(), n) == n);
            sp.add(block.data());
        }
        auto out = sp.take();
        double peak_hz = tuned + (double(argmax(out)) - n / 2.0) * rate / n;
        CHECK(std::fabs(peak_hz - 98.3e6) < 80e3);   // within the station's width
        CHECK(out[argmax(out)] > -40.0f);
        CHECK(out[10] < -60.0f);                      // noise floor at the band edge
    }
    CHECK(!make_source("nonsense", err) && !err.empty());
}

static void test_file_source() {
    const char* path = "zrf_test_iq.u8";
    {
        std::ofstream f(path, std::ios::binary);
        // 256 samples of a tone at +rate/4: I/Q steps through (1,0),(0,1),(-1,0),(0,-1).
        const unsigned char cyc[8] = {255, 127, 127, 255, 0, 127, 127, 0};
        for (int i = 0; i < 64; ++i) f.write(reinterpret_cast<const char*>(cyc), 8);
    }
    std::string err;
    auto src = make_source(std::string("file:") + path, err);
    CHECK(src && src->open(err));
    CHECK(src->set_rate(2e6));
    const size_t n = 1024;   // longer than the file: playback loops
    std::vector<cf32> block(n);
    CHECK(src->read(block.data(), n) == n);
    Spectrum sp(n);
    sp.add(block.data());
    CHECK(argmax(sp.take()) == n / 2 + n / 4);
    std::remove(path);

    auto missing = make_source("file:/nonexistent/iq.u8", err);
    CHECK(missing && !missing->open(err));
}

// Strongest audio frequency in Hz, by FFT of the last 8192 samples.
static double audio_peak_hz(const std::vector<int16_t>& pcm, double rate) {
    const size_t n = 8192;
    CHECK(pcm.size() >= n);
    Spectrum sp(n);
    std::vector<cf32> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = cf32(pcm[pcm.size() - n + i] / 32768.0f, 0);
    sp.add(v.data());
    auto out = sp.take();
    // Real input is symmetric; look at positive frequencies above 100 Hz only.
    size_t lo = n / 2 + size_t(100 * n / rate), best = lo;
    for (size_t i = lo; i < n; ++i)
        if (out[i] > out[best]) best = i;
    return (double(best) - n / 2.0) * rate / n;
}

static double rms(const std::vector<int16_t>& pcm, size_t from) {
    double sum = 0;
    for (size_t i = from; i < pcm.size(); ++i) sum += double(pcm[i]) * pcm[i];
    return std::sqrt(sum / double(pcm.size() - from)) / 32768.0;
}

// Each simulated signal demodulates to the tone it carries.
static void test_demod() {
    Mode m;
    CHECK(parse_mode("nfm", m) && m == Mode::Nfm && !parse_mode("ssb", m));
    struct Case { Mode mode; double hz, rate, tone; };
    const Case cases[] = {
        {Mode::Wfm, 98.3e6, 1.024e6, 1000},    // broadcast FM
        {Mode::Wfm, 98.3e6, 250e3, 1000},      // same station through the narrow view
        {Mode::Nfm, 162.55e6, 1.024e6, 700},   // narrow FM
        {Mode::Nfm, 162.55e6, 2.048e6, 700},
        {Mode::Am, 121.5e6, 1.024e6, 1000},    // airband AM
        {Mode::Usb, 14.2e6, 1.024e6, 700},     // upper sideband: tones 700 and 1900 Hz above the dial
        {Mode::Lsb, 7.15e6, 250e3, 700},       // lower sideband
    };
    for (const Case& c : cases) {
        std::string err;
        auto src = make_source("sim", err);
        CHECK(src && src->open(err) && src->set_rate(c.rate) && src->set_freq(c.hz));
        Demod demod(c.mode, c.rate);
        CHECK(demod.audio_rate() > 30000 && demod.audio_rate() < 34000);
        std::vector<cf32> block(4096);
        std::vector<int16_t> pcm;
        while (pcm.size() < size_t(demod.audio_rate() * 0.45)) {
            CHECK(src->read(block.data(), block.size()) == block.size());
            demod.run(block.data(), block.size(), pcm);
        }
        double peak = audio_peak_hz(pcm, demod.audio_rate());
        if (std::fabs(peak - c.tone) > 15) std::fprintf(stderr, "%s at %.0f: tone %.1f Hz\n", mode_name(c.mode), c.rate, peak);
        CHECK(std::fabs(peak - c.tone) < 15);
        double level = rms(pcm, pcm.size() / 2);
        CHECK(level > 0.1 && level < 0.9);   // audible, not clipped
    }

    // The wrong sideband is rejected. Input: a strong tone 700 Hz below the
    // dial and one 20 dB weaker 1000 Hz above it. On USB the weak wanted tone
    // must still come out at least 25 dB above what leaks of the strong one,
    // which makes the rejection at least 45 dB.
    const double rate = 1.024e6;
    Demod demod(Mode::Usb, rate);
    std::vector<cf32> block(4096);
    std::vector<int16_t> pcm;
    double t = 0;
    while (pcm.size() < 16000) {
        for (cf32& v : block) {
            double a = -2 * M_PI * 700 * t, b = 2 * M_PI * 1000 * t;
            v = cf32(float(0.1 * std::cos(a) + 0.01 * std::cos(b)), float(0.1 * std::sin(a) + 0.01 * std::sin(b)));
            t += 1 / rate;
        }
        demod.run(block.data(), block.size(), pcm);
    }
    const size_t n = 8192;
    Spectrum sp(n);
    std::vector<cf32> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = cf32(pcm[pcm.size() - n + i] / 32768.0f, 0);
    sp.add(v.data());
    auto out = sp.take();
    auto at = [&](double hz) {
        size_t bin = n / 2 + size_t(hz * n / demod.audio_rate() + 0.5);
        return std::max({out[bin - 1], out[bin], out[bin + 1]});
    };
    if (at(1000) - at(700) < 25) std::fprintf(stderr, "wanted %.1f dB, leak %.1f dB\n", at(1000), at(700));
    CHECK(at(1000) - at(700) > 25);
}

static void test_json() {
    auto f = parse_flat_json(R"({"model":"Acurite-Tower","id":11524,"temperature_C":21.3,"ok":true,)"
                             R"("name":"a \"b\", c","rows":[{"len":3,"data":"a,b}"},[1,2]],"last":-4.5} )");
    CHECK(f.size() == 7);
    CHECK(*json_get(f, "model") == "Acurite-Tower" && *json_get(f, "id") == "11524");
    CHECK(*json_get(f, "temperature_C") == "21.3" && *json_get(f, "ok") == "true");
    CHECK(*json_get(f, "name") == "a \"b\", c");
    CHECK(*json_get(f, "rows") == R"([{"len":3,"data":"a,b}"},[1,2]])");
    CHECK(*json_get(f, "last") == "-4.5" && !json_get(f, "missing"));
    CHECK(parse_flat_json("{}").empty() && parse_flat_json("").empty());
    CHECK(parse_flat_json("not json").empty() && parse_flat_json(R"({"a":1)").empty());
    CHECK(parse_flat_json(R"({"a" 1})").empty() && parse_flat_json(R"({"a":"unterminated})").empty());
}

// Published reference frames with known answers, then our own encoder
// checked against the decoder.
static void test_adsb() {
    AdsbTracker t;
    CHECK(t.add("*8D4840D6202CC371C32CE0576098;", 0));
    CHECK(t.aircraft().size() == 1 && t.aircraft()[0].icao == 0x4840D6 && t.aircraft()[0].callsign == "KLM1023");

    CHECK(t.add("*8D40621D58C386435CC412692AD6;", 1));   // odd half
    CHECK(!t.aircraft()[1].has_position);                // one half is not enough
    CHECK(t.add("*8d40621d58c382d690c8ac2863a7;", 2));   // even half, lower case as rtl_adsb prints
    const Aircraft& p = t.aircraft()[1];
    CHECK(p.icao == 0x40621D && p.has_altitude && p.altitude_ft == 38000);
    CHECK(p.has_position && std::fabs(p.lat - 52.2572) < 0.001 && std::fabs(p.lon - 3.91937) < 0.001);

    CHECK(t.add("*8D485020994409940838175B284F;", 3));   // ground speed
    const Aircraft& v = t.aircraft()[2];
    CHECK(v.has_velocity && std::fabs(v.speed_kt - 159.2) < 0.1 && std::fabs(v.heading_deg - 182.88) < 0.01);
    CHECK(v.climb_fpm == -832);
    CHECK(t.add("*8DA05F219B06B6AF189400CBC33F;", 3));   // airspeed and heading
    const Aircraft& w = t.aircraft()[3];
    CHECK(w.has_velocity && std::fabs(w.speed_kt - 375) < 0.1 && std::fabs(w.heading_deg - 243.98) < 0.01);
    CHECK(w.climb_fpm == -2304);

    // Damaged, short and foreign lines are refused.
    CHECK(!t.add("*8D4840D6202CC371C32CE0576099;", 4));  // one bit wrong
    CHECK(!t.add("*5D4840D6A1B2C3;", 4) && !t.add("hello", 4) && !t.add("", 4) && !t.add("*8D48;", 4));
    CHECK(t.aircraft().size() == 4);

    // Round trip through our encoder at awkward places on the globe.
    struct { double lat, lon; int alt; double spd, hdg; int climb; } cases[] = {
        {40.6413, -73.7781, 11000, 250, 45, 1280}, {-33.9461, 151.1772, 35000, 480, 271, 0},
        {64.13, -21.94, 2025, 140, 359, -640},     {0.01, 0.01, 41000, 510, 180, 64},
        {51.47, -0.4543, 0, 120, 90, -1920},
    };
    uint32_t icao = 0xABC000;
    for (auto& c : cases) {
        AdsbTracker r;
        CHECK(r.add(adsb_ident_frame(icao, "TEST12"), 0));
        CHECK(r.add(adsb_position_frame(icao, c.alt, c.lat, c.lon, false), 0));
        CHECK(r.add(adsb_position_frame(icao, c.alt, c.lat, c.lon, true), 1));
        CHECK(r.add(adsb_velocity_frame(icao, c.spd, c.hdg, c.climb), 1));
        const Aircraft& a = r.aircraft()[0];
        CHECK(a.callsign == "TEST12" && a.altitude_ft == c.alt && a.has_position);
        CHECK(std::fabs(a.lat - c.lat) < 0.0005 && std::fabs(a.lon - c.lon) < 0.0005);
        CHECK(std::fabs(a.speed_kt - c.spd) < 1.5 && a.climb_fpm == c.climb);
        double dh = std::fabs(a.heading_deg - c.hdg);
        CHECK(std::min(dh, 360 - dh) < 0.5);
        ++icao;
    }
    // Halves heard too far apart are not combined.
    AdsbTracker far;
    far.add(adsb_position_frame(1, 30000, 48, 11, false), 0);
    far.add(adsb_position_frame(1, 30000, 48, 11, true), 30);
    CHECK(!far.aircraft()[0].has_position);
    far.expire(100);
    CHECK(far.aircraft().empty());
}

int main() {
    test_fft();
    test_spectrum_level_and_position();
    test_reduce_peak();
    test_proto();
    test_sim_source();
    test_file_source();
    test_demod();
    test_json();
    test_adsb();
    std::puts("all DSP tests passed");
    return 0;
}
