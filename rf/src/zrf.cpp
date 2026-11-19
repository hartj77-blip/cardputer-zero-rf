// zrf: command-line client for the radio daemon. For scripts, tests and
// trying things out before the on-screen apps exist.
//
//   zrf send <command...>          send one command, print the reply
//   zrf peak [cols]                strongest column of one spectrum frame
//   zrf watch [cols]               live spectrum, one text line per frame
//   zrf listen <wfm|nfm|am> [seconds]   play audio until stopped (or for N seconds)
//   zrf record <file.cu8> [seconds]     save raw IQ at the current tuning
//   zrf sensors [seconds]               print decoded sensor messages (rtl_433 JSON)
//   zrf aircraft [seconds]              print raw ADS-B frames from aircraft
//   zrf server [seconds]                serve the dongle on the network (rtl_tcp, port 1234)
//   zrf snapshot <out.ppm> [cols] [rows]   spectrum + waterfall picture
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "proto.h"

using namespace zrf;

namespace {

int g_fd = -1;
std::string g_buf;

bool connect_to(const std::string& path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof addr.sun_path) return false;
    std::strcpy(addr.sun_path, path.c_str());
    g_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    return g_fd >= 0 && connect(g_fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
}

bool send_line(const std::string& s) {
    std::string line = s + "\n";
    return write(g_fd, line.data(), line.size()) == ssize_t(line.size());
}

// One line from the daemon; false on timeout or disconnect.
bool read_line(std::string& line, int timeout_ms = 5000) {
    for (;;) {
        size_t nl = g_buf.find('\n');
        if (nl != std::string::npos) {
            line = g_buf.substr(0, nl);
            g_buf.erase(0, nl + 1);
            return true;
        }
        pollfd p{g_fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return false;
        char buf[4096];
        ssize_t n = read(g_fd, buf, sizeof buf);
        if (n <= 0) return false;
        g_buf.append(buf, size_t(n));
    }
}

struct Frame {
    double freq = 0, rate = 0;
    std::vector<uint8_t> cols;
};

// Next spectrum frame. Daemon errors ("E ...") are printed and end the wait.
bool read_frame(Frame& f) {
    std::string line;
    while (read_line(line)) {
        if (line.rfind("E ", 0) == 0) { std::fprintf(stderr, "zrf: %s\n", line.c_str() + 2); return false; }
        if (line.rfind("S ", 0) != 0) continue;
        std::istringstream is(line.substr(2));
        std::string hex;
        if (is >> f.freq >> f.rate >> hex && from_hex(hex, f.cols)) return true;
    }
    std::fprintf(stderr, "zrf: no data from the daemon\n");
    return false;
}

bool subscribe(int cols) {
    std::string reply;
    if (!send_line("spectrum " + std::to_string(cols)) || !read_line(reply)) return false;
    if (reply.rfind("ok", 0) != 0) { std::fprintf(stderr, "zrf: %s\n", reply.c_str()); return false; }
    return true;
}

// Dark blue -> cyan -> yellow -> white.
void heat(float t, uint8_t* rgb) {
    t = std::clamp(t, 0.0f, 1.0f);
    auto ramp = [&](float a, float b) { return uint8_t(255 * std::clamp((t - a) / (b - a), 0.0f, 1.0f)); };
    rgb[0] = ramp(0.45f, 0.8f);
    rgb[1] = ramp(0.15f, 0.65f);
    rgb[2] = uint8_t(std::max(60 + 195 * std::clamp(t / 0.3f, 0.0f, 1.0f) - 255 * std::clamp((t - 0.5f) / 0.3f, 0.0f, 1.0f),
                              float(ramp(0.85f, 1.0f))));
}

int snapshot(const std::string& out, int cols, int rows) {
    const int trace_h = 50;
    if (!subscribe(cols)) return 1;
    std::vector<Frame> frames(size_t(rows), Frame{});
    for (auto& f : frames)
        if (!read_frame(f) || int(f.cols.size()) != cols) return 1;

    // Scale colours from the quietest levels to the strongest seen.
    std::vector<uint8_t> all;
    for (auto& f : frames) all.insert(all.end(), f.cols.begin(), f.cols.end());
    std::sort(all.begin(), all.end());
    float lo = all[all.size() / 10], hi = std::max<float>(all.back(), lo + 20);

    std::vector<uint8_t> img(size_t(cols) * size_t(trace_h + rows) * 3, 0);
    auto px = [&](int x, int y) { return &img[(size_t(y) * size_t(cols) + size_t(x)) * 3]; };
    const Frame& last = frames.back();
    for (int x = 0; x < cols; ++x) {
        float t = std::clamp((last.cols[size_t(x)] - lo) / (hi - lo), 0.0f, 1.0f);
        int top = int((1 - t) * (trace_h - 1));
        for (int y = top; y < trace_h; ++y) {
            uint8_t* p = px(x, y);
            p[0] = y == top ? 255 : 20; p[1] = y == top ? 220 : 70; p[2] = y == top ? 80 : 110;
        }
    }
    for (int r = 0; r < rows; ++r)   // newest row at the top of the waterfall
        for (int x = 0; x < cols; ++x)
            heat((frames[size_t(rows - 1 - r)].cols[size_t(x)] - lo) / (hi - lo), px(x, trace_h + r));

    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) { std::fprintf(stderr, "zrf: can't write %s\n", out.c_str()); return 1; }
    std::fprintf(f, "P6\n%d %d\n255\n", cols, trace_h + rows);
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    std::printf("%s: %dx%d, centre %.0f Hz, span %.0f Hz\n", out.c_str(), cols, trace_h + rows, last.freq, last.rate);
    return 0;
}

int usage() {
    std::fprintf(stderr,
                 "usage: zrf [--socket <path>] send <command...>\n"
                 "       zrf [--socket <path>] peak [cols]\n"
                 "       zrf [--socket <path>] watch [cols]\n"
                 "       zrf [--socket <path>] record <file.cu8> [seconds]\n"
                 "       zrf [--socket <path>] listen <wfm|nfm|am> [seconds]\n"
                 "       zrf [--socket <path>] sensors|aircraft|aprs|server [seconds]\n"
                 "       zrf [--socket <path>] snapshot <out.ppm> [cols] [rows]\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path = default_socket_path();
    int i = 1;
    if (i + 1 < argc && std::string(argv[i]) == "--socket") { path = argv[i + 1]; i += 2; }
    if (i >= argc) return usage();
    std::string cmd = argv[i++];
    auto int_arg = [&](int fallback) { return i < argc ? std::atoi(argv[i++]) : fallback; };

    if (!connect_to(path)) {
        std::fprintf(stderr, "zrf: can't reach the radio daemon at %s\n", path.c_str());
        return 1;
    }

    if (cmd == "send") {
        std::string line, reply;
        for (; i < argc; ++i) line += (line.empty() ? "" : " ") + std::string(argv[i]);
        if (line.empty()) return usage();
        if (!send_line(line) || !read_line(reply)) { std::fprintf(stderr, "zrf: no reply\n"); return 1; }
        std::printf("%s\n", reply.c_str());
        return reply.rfind("ok", 0) == 0 ? 0 : 1;
    }
    if (cmd == "peak") {
        int cols = int_arg(320);
        Frame f;
        if (!subscribe(cols) || !read_frame(f) || !read_frame(f)) return 1;   // skip the first frame after a retune
        size_t best = size_t(std::max_element(f.cols.begin(), f.cols.end()) - f.cols.begin());
        // A wide signal's strongest column wanders; report the middle of
        // everything within 6 dB of it instead.
        size_t lo = best, hi = best;
        while (lo > 0 && f.cols[lo - 1] + 12 >= f.cols[best]) --lo;
        while (hi + 1 < f.cols.size() && f.cols[hi + 1] + 12 >= f.cols[best]) ++hi;
        double hz = f.freq + ((lo + hi) / 2.0 + 0.5 - cols / 2.0) * f.rate / cols;
        std::printf("col=%zu db=%.1f hz=%.0f\n", best, byte_to_db(f.cols[best]), hz);
        return 0;
    }
    if (cmd == "watch") {
        int cols = int_arg(64);
        static const char* bars[] = {" ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
        Frame f;
        if (!subscribe(cols)) return 1;
        while (read_frame(f)) {
            uint8_t lo = *std::min_element(f.cols.begin(), f.cols.end());
            std::string line;
            for (uint8_t b : f.cols) line += bars[std::min(8, (b - lo) / 12)];
            std::printf("%s %.3f MHz\n", line.c_str(), f.freq / 1e6);
            std::fflush(stdout);
        }
        return 1;
    }
    if (cmd == "record") {
        if (i >= argc) return usage();
        std::string path = argv[i++], reply, line;
        int seconds = int_arg(0);
        if (!send_line("record " + path) || !read_line(reply)) return 1;
        if (reply.rfind("ok", 0) != 0) { std::fprintf(stderr, "zrf: %s\n", reply.c_str()); return 1; }
        for (int waited = 0; seconds == 0 || waited < seconds * 10; ++waited)
            if (read_line(line, 100) && line.rfind("E ", 0) == 0) { std::fprintf(stderr, "zrf: %s\n", line.c_str() + 2); return 1; }
        return 0;
    }
    if (cmd == "listen") {
        if (i >= argc) return usage();
        std::string mode = argv[i++], reply;
        int seconds = int_arg(0);
        if (!send_line("mode " + mode) || !read_line(reply)) return 1;
        if (reply.rfind("ok", 0) != 0) { std::fprintf(stderr, "zrf: %s\n", reply.c_str()); return 1; }
        // Audio plays for as long as we stay connected.
        std::string line;
        for (int waited = 0; seconds == 0 || waited < seconds * 10; ++waited)
            if (read_line(line, 100) && line.rfind("E ", 0) == 0) std::fprintf(stderr, "zrf: %s\n", line.c_str() + 2);
        return 0;
    }
    if (cmd == "sensors" || cmd == "aircraft" || cmd == "aprs" || cmd == "server") {
        int seconds = int_arg(0);
        std::string reply, line;
        if (!send_line("decode " + cmd) || !read_line(reply)) return 1;
        if (reply.rfind("ok", 0) != 0) { std::fprintf(stderr, "zrf: %s\n", reply.c_str()); return 1; }
        for (int waited = 0; seconds == 0 || waited < seconds * 10; ++waited) {
            if (!read_line(line, 100)) continue;
            if (line.rfind("D ", 0) == 0) { std::printf("%s\n", line.c_str() + 2); std::fflush(stdout); }
            else if (line.rfind("E ", 0) == 0) std::fprintf(stderr, "zrf: %s\n", line.c_str() + 2);
        }
        return 0;
    }
    if (cmd == "snapshot") {
        if (i >= argc) return usage();
        std::string out = argv[i++];
        int cols = int_arg(320), rows = int_arg(80);
        if (cols < 1 || rows < 1) return usage();
        return snapshot(out, cols, rows);
    }
    return usage();
}
